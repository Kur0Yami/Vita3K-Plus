// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

// Save state, stage 2 (experimental, same session only).
//
// Save: with the game paused, record every guest thread (status, whether it is blocked inside an HLE call,
// registers), the layout of valid guest memory and all non-zero guest RAM pages to
//   <emulated system storage>/savestate/<title id>/slot-<N>.v3ks
//
// Load: guarded. Nothing is touched unless every guest thread is still where the file says it was:
//  - threads blocked inside an HLE call must wait on the same object, at the same pc/sp;
//  - threads parked in JIT code (suspend) get their saved registers back;
//  - the set of valid memory pages must be identical.
// Then guest RAM is rewritten. Kernel objects (semaphores, mutexes, ...), GPU and audio state are NOT restored,
// so this only works when those are in the same state as when the slot was saved (for example the same
// idle point of the game loop).

#include "android_state.h"

#include <app/session_controller.h>
#include <cpu/common.h>
#include <cpu/functions.h>
#include <emuenv/state.h>
#include <io/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <mem/state.h>
#include <util/fs.h>

#include <jni.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t PAGE = 4096;
constexpr uint64_t RAM_START = 0x80000000ull; // user_main_memory_start
constexpr uint64_t RAM_END = 0x100000000ull;
constexpr uint32_t CHUNK_PAGES = 256; // 1 MiB per read
constexpr int MAX_SLOT = 3;
constexpr uint32_t FILE_VERSION = 2;

using Range = std::pair<uint32_t, uint32_t>; // first page address, page count

std::mutex g_state_mutex;

EmuEnvState *active_env() {
    auto *emuenv = get_emuenv();
    auto *controller = get_app_session_controller();
    return (emuenv && controller && controller->has_active_session()) ? emuenv : nullptr;
}

std::string safe_id(const std::string &in) {
    std::string out;
    for (char c : in)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') ? c : '_';
    return out.empty() ? std::string("game") : out;
}

fs::path slot_file(const EmuEnvState &emuenv, int slot) {
    return emuenv.vita_fs_path / "savestate" / safe_id(emuenv.io.app_path) / ("slot-" + std::to_string(slot) + ".v3ks");
}

std::string ascii(const std::string &in) {
    std::string out;
    for (unsigned char c : in)
        out += ((c >= 0x20 && c < 0x7F) || c == '\n') ? static_cast<char>(c) : '?';
    return out;
}

template <typename T>
void put(std::ofstream &out, const T &v) {
    out.write(reinterpret_cast<const char *>(&v), sizeof(T));
}

void put_str(std::ofstream &out, const std::string &s) {
    const uint16_t n = static_cast<uint16_t>(std::min<size_t>(s.size(), 0xFFFF));
    put(out, n);
    out.write(s.data(), n);
}

bool page_is_zero(const uint8_t *p) {
    const uint64_t *w = reinterpret_cast<const uint64_t *>(p);
    for (size_t i = 0; i < PAGE / 8; ++i)
        if (w[i])
            return false;
    return true;
}

std::vector<Range> valid_ranges(const MemState &mem) {
    std::vector<Range> out;
    for (uint64_t page = RAM_START / PAGE; page < RAM_END / PAGE;) {
        if (!is_valid_addr(mem, static_cast<Address>(page * PAGE))) {
            ++page;
            continue;
        }
        const uint64_t first = page;
        while (page < RAM_END / PAGE && is_valid_addr(mem, static_cast<Address>(page * PAGE)))
            ++page;
        out.emplace_back(static_cast<uint32_t>(first * PAGE), static_cast<uint32_t>(page - first));
    }
    return out;
}

template <typename T>
bool get(std::ifstream &in, T &v) {
    in.read(reinterpret_cast<char *>(&v), sizeof(T));
    return static_cast<bool>(in);
}

bool get_str(std::ifstream &in, std::string &s) {
    uint16_t n = 0;
    if (!get(in, n))
        return false;
    s.resize(n);
    if (n)
        in.read(s.data(), n);
    return static_cast<bool>(in);
}

struct SavedThread {
    uint32_t id = 0;
    uint8_t status = 0, hle = 0;
    std::string name, kind;
    uint32_t uid = 0;
    int32_t prio = 0;
    bool have_ctx = false;
    CPUContext ctx;
};

struct Counts {
    uint32_t total = 0, run = 0, wait = 0, suspend = 0, dormant = 0, in_hle = 0, saved_ctx = 0, not_parked = 0;
};

std::string do_save(EmuEnvState &emuenv, int slot) {
    const auto t0 = std::chrono::steady_clock::now();
    auto *controller = get_app_session_controller();
    if (!controller || !controller->is_paused())
        return "ERR: buka menu pause dulu (game harus dijeda).";

    // 1. Wait until no thread is inside the JIT.
    std::vector<ThreadStatePtr> threads;
    {
        const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        for (auto &[tid, th] : emuenv.kernel.threads)
            threads.push_back(th);
    }
    const auto deadline = t0 + std::chrono::milliseconds(2000);
    for (;;) {
        bool any_running = false;
        for (const auto &th : threads)
            if (th->status == ThreadStatus::run)
                any_running = true;
        if (!any_running || std::chrono::steady_clock::now() > deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // 2. Open the file.
    const fs::path file = slot_file(emuenv, slot);
    boost::system::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
    if (!out)
        return "ERR: gagal membuat file " + file.string();

    out.write("V3KS", 4);
    put<uint32_t>(out, FILE_VERSION); // version
    put<uint32_t>(out, 0); // flags
    put<uint64_t>(out, static_cast<uint64_t>(std::time(nullptr)));
    const std::streampos counts_pos = out.tellp();
    put<uint32_t>(out, 0); // thread count (patched below)
    put<uint32_t>(out, 0); // ram run count (patched below)

    // 3. Threads.
    Counts c;
    std::string thread_lines;
    for (const auto &th : threads) {
        ++c.total;
        const ThreadStatus st = th->status;
        switch (st) {
        case ThreadStatus::run:
            ++c.run;
            ++c.not_parked;
            break;
        case ThreadStatus::wait: ++c.wait; break;
        case ThreadStatus::suspend: ++c.suspend; break;
        case ThreadStatus::dormant: ++c.dormant; break;
        }
        const bool hle = th->inside_hle();
        if (hle)
            ++c.in_hle;

        put<uint32_t>(out, static_cast<uint32_t>(th->id));
        put_str(out, th->name);
        put<uint8_t>(out, static_cast<uint8_t>(st));
        put<uint8_t>(out, hle ? 1 : 0);
        const char *kind = th->wait_prim_kind;
        put_str(out, kind ? kind : "");
        put<uint32_t>(out, static_cast<uint32_t>(th->wait_prim_uid));
        put<int32_t>(out, static_cast<int32_t>(th->priority));

        CPUContext ctx;
        bool have_ctx = false;
        if (th->cpu && st != ThreadStatus::run) {
            try {
                ctx = save_context(*th->cpu);
                have_ctx = true;
                ++c.saved_ctx;
            } catch (...) {
            }
        }
        put<uint8_t>(out, have_ctx ? 1 : 0);
        out.write(reinterpret_cast<const char *>(ctx.cpu_registers.data()), sizeof(uint32_t) * 16);
        out.write(reinterpret_cast<const char *>(ctx.fpu_registers.data()), sizeof(float) * 64);
        put<uint32_t>(out, ctx.cpsr);
        put<uint32_t>(out, ctx.fpscr);

        if (thread_lines.size() < 1800) {
            static const char *names[] = { "run", "dormant", "suspend", "wait" };
            char buf[200];
            std::snprintf(buf, sizeof buf, "  #%d %s [%s]%s%s pc=%08X\n", static_cast<int>(th->id),
                ascii(th->name).substr(0, 24).c_str(), names[static_cast<int>(st)],
                hle ? " HLE" : "", kind ? (std::string(" ") + kind).c_str() : "",
                have_ctx ? ctx.get_pc() : 0u);
            thread_lines += buf;
        }
    }

    // 4. Layout of valid memory, then RAM: every non-zero page, grouped into runs.
    const std::vector<Range> ranges = valid_ranges(emuenv.mem);
    put<uint32_t>(out, static_cast<uint32_t>(ranges.size()));
    for (const auto &r : ranges) {
        put<uint32_t>(out, r.first);
        put<uint32_t>(out, r.second);
    }
    uint32_t runs = 0;
    uint64_t data_pages = 0, valid_pages = 0;
    std::vector<uint8_t> buf(static_cast<size_t>(CHUNK_PAGES) * PAGE);
    bool read_failed = false;
    for (const auto &rg : ranges) {
        const uint64_t first = rg.first / PAGE, page = first + rg.second;
        valid_pages += rg.second;
        bool in_run = false;
        uint64_t run_start = 0;
        std::vector<uint8_t> run_data;
        const auto flush = [&] {
            if (!in_run)
                return;
            put<uint32_t>(out, static_cast<uint32_t>(run_start * PAGE));
            put<uint32_t>(out, static_cast<uint32_t>(run_data.size() / PAGE));
            out.write(reinterpret_cast<const char *>(run_data.data()), static_cast<std::streamsize>(run_data.size()));
            ++runs;
            data_pages += run_data.size() / PAGE;
            run_data.clear();
            in_run = false;
        };
        for (uint64_t p = first; p < page; p += CHUNK_PAGES) {
            const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(CHUNK_PAGES, page - p));
            if (!debug_safe_copy_guest(emuenv.mem, static_cast<Address>(p * PAGE), buf.data(), n * PAGE)) {
                read_failed = true;
                flush();
                continue;
            }
            for (uint32_t i = 0; i < n; ++i) {
                const uint8_t *pg = buf.data() + static_cast<size_t>(i) * PAGE;
                if (page_is_zero(pg)) {
                    flush();
                    continue;
                }
                if (!in_run) {
                    in_run = true;
                    run_start = p + i;
                }
                run_data.insert(run_data.end(), pg, pg + PAGE);
                if (run_data.size() >= (32u << 20))
                    flush();
            }
        }
        flush();
    }

    // 5. Patch counts and close.
    out.seekp(counts_pos);
    put<uint32_t>(out, c.total);
    put<uint32_t>(out, runs);
    out.seekp(0, std::ios::end);
    const uint64_t size = static_cast<uint64_t>(out.tellp());
    out.flush();
    if (!out)
        return "ERR: gagal menulis file (storage penuh?): " + file.string();
    out.close();

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    char head[600];
    std::snprintf(head, sizeof head,
        "Slot %d tersimpan.\n"
        "Thread: %u (run %u, wait %u, suspend %u, dormant %u)\n"
        "Di dalam HLE: %u | konteks CPU tersimpan: %u | belum terparkir: %u\n"
        "RAM: %.1f MiB isi dari %.1f MiB valid, %u blok%s\n"
        "File: %s (%.1f MiB), %lld ms\n",
        slot, c.total, c.run, c.wait, c.suspend, c.dormant, c.in_hle, c.saved_ctx, c.not_parked,
        data_pages * PAGE / 1048576.0, valid_pages * PAGE / 1048576.0, runs, read_failed ? " (ada halaman gagal dibaca)" : "",
        file.string().c_str(), size / 1048576.0, static_cast<long long>(ms));
    return std::string(head) + thread_lines;
}


std::string do_load(EmuEnvState &emuenv, int slot) {
    const auto t0 = std::chrono::steady_clock::now();
    auto *controller = get_app_session_controller();
    if (!controller || !controller->is_paused())
        return "ERR: buka menu pause dulu (game harus dijeda).";

    const fs::path file = slot_file(emuenv, slot);
    std::ifstream in(file.string(), std::ios::binary | std::ios::ate);
    if (!in)
        return "ERR: slot " + std::to_string(slot) + " kosong.";
    const uint64_t file_size = static_cast<uint64_t>(in.tellg());
    in.seekg(0);

    char magic[4] = {};
    uint32_t version = 0, flags = 0, nthreads = 0, nruns = 0;
    uint64_t stamp = 0;
    in.read(magic, 4);
    if (!in || std::memcmp(magic, "V3KS", 4) != 0)
        return "ERR: bukan file state V3KS.";
    if (!get(in, version) || !get(in, flags) || !get(in, stamp) || !get(in, nthreads) || !get(in, nruns))
        return "ERR: header rusak.";
    if (version != FILE_VERSION)
        return "ERR: slot ini dari versi lama (v" + std::to_string(version) + "). Simpan ulang dulu.";

    // 1. Parse the file (nothing is changed yet).
    std::vector<SavedThread> saved(nthreads);
    for (auto &s : saved) {
        if (!get(in, s.id) || !get_str(in, s.name) || !get(in, s.status) || !get(in, s.hle) || !get_str(in, s.kind)
            || !get(in, s.uid) || !get(in, s.prio))
            return "ERR: tabel thread rusak.";
        uint8_t have = 0;
        if (!get(in, have))
            return "ERR: tabel thread rusak.";
        s.have_ctx = have != 0;
        in.read(reinterpret_cast<char *>(s.ctx.cpu_registers.data()), sizeof(uint32_t) * 16);
        in.read(reinterpret_cast<char *>(s.ctx.fpu_registers.data()), sizeof(float) * 64);
        if (!get(in, s.ctx.cpsr) || !get(in, s.ctx.fpscr))
            return "ERR: konteks thread rusak.";
    }
    uint32_t nranges = 0;
    if (!get(in, nranges) || nranges > 100000)
        return "ERR: tabel memori rusak.";
    std::vector<Range> saved_ranges(nranges);
    for (auto &r : saved_ranges)
        if (!get(in, r.first) || !get(in, r.second))
            return "ERR: tabel memori rusak.";
    struct Run {
        uint32_t addr, pages;
        uint64_t offset;
    };
    std::vector<Run> runs(nruns);
    for (auto &r : runs) {
        if (!get(in, r.addr) || !get(in, r.pages))
            return "ERR: tabel RAM rusak.";
        r.offset = static_cast<uint64_t>(in.tellg());
        if (r.offset + static_cast<uint64_t>(r.pages) * PAGE > file_size)
            return "ERR: file terpotong.";
        in.seekg(static_cast<std::streamoff>(static_cast<uint64_t>(r.pages) * PAGE), std::ios::cur);
    }

    // 2. Memory layout must be the same.
    const std::vector<Range> now_ranges = valid_ranges(emuenv.mem);
    if (now_ranges != saved_ranges) {
        uint64_t a = 0, b = 0;
        for (const auto &r : saved_ranges) a += r.second;
        for (const auto &r : now_ranges) b += r.second;
        char m[300];
        std::snprintf(m, sizeof m,
            "GAGAL (tidak ada yang diubah): alokasi memori beda dari saat disimpan\n(saat save %llu halaman, sekarang %llu halaman).\nCoba Load di titik game yang sama seperti saat Save.",
            static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
        return m;
    }

    // 3. Every thread must be where the file says.
    std::map<uint32_t, ThreadStatePtr> live;
    {
        const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        for (auto &[tid, th] : emuenv.kernel.threads)
            live[static_cast<uint32_t>(tid)] = th;
    }
    std::string problems;
    int problem_count = 0;
    const auto problem = [&](const std::string &s) {
        ++problem_count;
        if (problem_count <= 12)
            problems += "  " + s + "\n";
    };
    const auto ident = [](const SavedThread &s) { return "#" + std::to_string(s.id) + " " + ascii(s.name).substr(0, 24); };
    std::vector<std::pair<ThreadStatePtr, const SavedThread *>> to_restore;
    for (const auto &s : saved) {
        const auto it = live.find(s.id);
        if (it == live.end()) {
            problem(ident(s) + ": thread sudah tidak ada");
            continue;
        }
        const ThreadStatePtr &th = it->second;
        const ThreadStatus st = th->status;
        if (st == ThreadStatus::run) {
            problem(ident(s) + ": belum terparkir");
            continue;
        }
        const bool saved_blocked = s.status == static_cast<uint8_t>(ThreadStatus::wait);
        if (saved_blocked) {
            const std::string kind = th->wait_prim_kind ? th->wait_prim_kind : "";
            if (st != ThreadStatus::wait || kind != s.kind || static_cast<uint32_t>(th->wait_prim_uid) != s.uid) {
                problem(ident(s) + ": nunggu hal lain (sekarang " + (kind.empty() ? "-" : kind) + ")");
                continue;
            }
            if (!th->cpu || !s.have_ctx) {
                problem(ident(s) + ": konteks tidak ada");
                continue;
            }
            const CPUContext cur = save_context(*th->cpu);
            if (cur.get_pc() != s.ctx.get_pc() || cur.get_sp() != s.ctx.get_sp()) {
                char m[120];
                std::snprintf(m, sizeof m, ": pc/sp beda (%08X/%08X, seharusnya %08X/%08X)", cur.get_pc(), cur.get_sp(), s.ctx.get_pc(), s.ctx.get_sp());
                problem(ident(s) + m);
                continue;
            }
        } else if (s.status == static_cast<uint8_t>(ThreadStatus::suspend)) {
            if (st != ThreadStatus::suspend || th->inside_hle() || !th->cpu || !s.have_ctx) {
                problem(ident(s) + ": tidak terparkir di kode game seperti saat save");
                continue;
            }
            to_restore.emplace_back(th, &s);
        } else if (st != static_cast<ThreadStatus>(s.status)) {
            problem(ident(s) + ": status beda");
            continue;
        }
    }
    for (const auto &kv : live) {
        const uint32_t tid = kv.first;
        const ThreadStatePtr &th = kv.second;
        if (th->status != ThreadStatus::dormant && std::none_of(saved.begin(), saved.end(), [tid](const SavedThread &s) { return s.id == tid; }))
            problem("#" + std::to_string(tid) + " " + ascii(th->name).substr(0, 24) + ": thread baru sejak save");
    }
    if (problem_count) {
        return "GAGAL (tidak ada yang diubah): " + std::to_string(problem_count) + " thread tidak di titik yang sama.\n" + problems
            + "Tutup menu, tunggu sebentar, buka lagi dan coba Load (idealnya di titik game yang sama seperti saat Save).";
    }

    // 4. Rewrite guest RAM: zero every valid page that has no saved data, then write the saved runs.
    uint64_t zeroed = 0, written = 0;
    std::vector<uint8_t> zeros(1u << 20, 0), buf(1u << 20);
    bool write_failed = false;
    const auto zero_span = [&](uint64_t from, uint64_t to) {
        for (uint64_t a = from; a < to;) {
            const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(zeros.size(), to - a));
            if (!debug_safe_write_guest(emuenv.mem, static_cast<Address>(a), zeros.data(), n))
                write_failed = true;
            a += n;
            zeroed += n;
        }
    };
    size_t ri = 0;
    for (const auto &rg : saved_ranges) {
        const uint64_t s = rg.first, e = s + static_cast<uint64_t>(rg.second) * PAGE;
        uint64_t cursor = s;
        while (ri < runs.size() && runs[ri].addr < e) {
            if (runs[ri].addr > cursor)
                zero_span(cursor, runs[ri].addr);
            cursor = std::max<uint64_t>(cursor, static_cast<uint64_t>(runs[ri].addr) + static_cast<uint64_t>(runs[ri].pages) * PAGE);
            ++ri;
        }
        if (cursor < e)
            zero_span(cursor, e);
    }
    for (const auto &r : runs) {
        in.clear();
        in.seekg(static_cast<std::streamoff>(r.offset));
        uint64_t left = static_cast<uint64_t>(r.pages) * PAGE, addr = r.addr;
        while (left) {
            const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(buf.size(), left));
            in.read(reinterpret_cast<char *>(buf.data()), n);
            if (!in)
                return "ERR: gagal membaca file di tengah pemulihan, RAM mungkin setengah terpulihkan; tutup game.";
            if (!debug_safe_write_guest(emuenv.mem, static_cast<Address>(addr), buf.data(), n))
                write_failed = true;
            addr += n;
            left -= n;
            written += n;
        }
    }

    // 5. Registers of the threads parked in game code.
    for (const auto &[th, s] : to_restore)
        load_context(*th->cpu, s->ctx);

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    char head[400];
    std::snprintf(head, sizeof head,
        "Slot %d dimuat (eksperimental).\nRAM: %.1f MiB ditulis, %.1f MiB dikosongkan%s\nKonteks thread dipulihkan: %zu | thread menunggu tidak diubah: %zu | %lld ms\nTutup menu untuk lanjut. Kalau game aneh, Load ulang atau restart.",
        slot, written / 1048576.0, zeroed / 1048576.0, write_failed ? " (ada halaman gagal ditulis)" : "", to_restore.size(),
        saved.size() - to_restore.size(), static_cast<long long>(ms));
    return head;
}

} // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_StateNative_saveSlot(JNIEnv *env, jclass, jint slot) {
    std::string result;
    auto *emuenv = active_env();
    if (!emuenv)
        result = "ERR: tidak ada game yang berjalan.";
    else if (slot < 1 || slot > MAX_SLOT)
        result = "ERR: slot tidak valid.";
    else {
        const std::lock_guard<std::mutex> lock(g_state_mutex);
        try {
            result = do_save(*emuenv, slot);
        } catch (const std::exception &e) {
            result = std::string("ERR: ") + e.what();
        } catch (...) {
            result = "ERR: kesalahan tak dikenal saat menyimpan.";
        }
    }
    return env->NewStringUTF(ascii(result).c_str());
}

JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_StateNative_loadSlot(JNIEnv *env, jclass, jint slot) {
    std::string result;
    auto *emuenv = active_env();
    if (!emuenv)
        result = "ERR: tidak ada game yang berjalan.";
    else if (slot < 1 || slot > MAX_SLOT)
        result = "ERR: slot tidak valid.";
    else {
        const std::lock_guard<std::mutex> lock(g_state_mutex);
        try {
            result = do_load(*emuenv, slot);
        } catch (const std::exception &e) {
            result = std::string("ERR: ") + e.what();
        } catch (...) {
            result = "ERR: kesalahan tak dikenal saat memuat.";
        }
    }
    return env->NewStringUTF(ascii(result).c_str());
}

JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_StateNative_slotInfo(JNIEnv *env, jclass, jint slot) {
    std::string info;
    auto *emuenv = active_env();
    if (emuenv && slot >= 1 && slot <= MAX_SLOT) {
        std::ifstream in(slot_file(*emuenv, slot).string(), std::ios::binary | std::ios::ate);
        if (in) {
            const uint64_t size = static_cast<uint64_t>(in.tellg());
            in.seekg(12);
            uint64_t ts = 0;
            in.read(reinterpret_cast<char *>(&ts), sizeof ts);
            info = std::to_string(size) + "|" + std::to_string(ts);
        }
    }
    return env->NewStringUTF(info.c_str());
}

} // extern "C"
