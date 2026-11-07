// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include "android_state.h"
#include "cheat_core.h"
#include "cheat_psv.h"

#include <app/session_controller.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <io/state.h>
#include <util/fs.h>

#include <jni.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

cheat::Scanner &scanner() {
    static auto *instance = new cheat::Scanner(); // intentionally leaked: the freezer thread outlives statics
    return *instance;
}

cheat::CheatList &cheats() {
    static auto *instance = new cheat::CheatList();
    return *instance;
}

// Cheat files live in the emulated system storage (vita_fs_path), next to ux0/ and vs0/, so they
// follow the storage folder when it is switched. Files from the old location (<config>/cheats) are
// copied over once, without overwriting anything already there.
fs::path cheats_dir(const EmuEnvState &emuenv) {
    const fs::path dir = emuenv.vita_fs_path / "cheats";
    boost::system::error_code ec;
    if (!fs::exists(dir, ec)) {
        fs::create_directories(dir, ec);
        const fs::path old_dir = emuenv.config_path / "cheats";
        if (old_dir != dir && fs::is_directory(old_dir, ec)) {
            for (fs::directory_iterator it(old_dir, ec), end; !ec && it != end; it.increment(ec)) {
                const fs::path src = it->path();
                if (!fs::is_regular_file(src, ec))
                    continue;
                const fs::path dst = dir / src.filename();
                if (!fs::exists(dst, ec))
                    fs::copy_file(src, dst, ec);
            }
        }
    }
    return dir;
}

std::string list_file(const EmuEnvState &emuenv) {
    return (cheats_dir(emuenv) / (emuenv.io.app_path + ".txt")).string();
}

std::string psv_file(const EmuEnvState &emuenv) {
    return (cheats_dir(emuenv) / (emuenv.io.app_path + ".psv")).string();
}

// Loads this game's list if it was not loaded yet (the freezer thread may not have ticked yet).
void ensure_loaded(EmuEnvState &emuenv) {
    cheats().ensure_loaded(emuenv.io.app_path, list_file(emuenv), psv_file(emuenv));
}

// Every change to the list is stored right away.
void autosave(EmuEnvState &emuenv) {
    const auto file = list_file(emuenv);
    boost::system::error_code ec;
    fs::create_directories(fs::path(file).parent_path(), ec);
    cheats().save(file);
}

// JNI's NewStringUTF needs valid (modified) UTF-8; names from .psv files may be ANSI. Also keeps names on one line.
std::string clean_text(const std::string &in) {
    std::string out;
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        size_t len = c < 0x80 ? 1 : (c >= 0xC2 && c < 0xE0) ? 2 : (c >= 0xE0 && c < 0xF0) ? 3 : 0;
        bool ok = len > 0 && i + len <= in.size();
        for (size_t k = 1; ok && k < len; k++)
            ok = (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
        if (len == 4 || !ok) { // 4-byte sequences are not valid modified UTF-8 either
            out += '?';
            ++i;
            continue;
        }
        if (len == 1 && (c == '\t' || c == '\n' || c == '\r' || c < 0x20))
            out += ' ';
        else
            out.append(in, i, len);
        i += len;
    }
    return out;
}

// The live environment, only while a game session exists (pause menu included).
EmuEnvState *active_env() {
    auto *emuenv = get_emuenv();
    auto *controller = get_app_session_controller();
    return (emuenv && controller && controller->has_active_session()) ? emuenv : nullptr;
}

void freezer_loop() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto *emuenv = get_emuenv();
        auto *controller = get_app_session_controller();
        if (!emuenv || !controller || !controller->has_active_session()) {
            cheats().reset_app();
            continue;
        }
        if (!controller->is_running())
            continue;
        cheats().tick(emuenv->mem, emuenv->io.app_path, list_file(*emuenv), psv_file(*emuenv));
    }
}

struct FreezerStarter {
    FreezerStarter() {
        std::thread(freezer_loop).detach();
    }
};
const FreezerStarter g_freezer_starter;

// ---------------------------------------------------------------------------------------------
// Watch list: polls addresses ~120x per second and logs every change, so the log shows how a value
// moves over time and which values only move when something specific happens. Saved as a text file.
// ---------------------------------------------------------------------------------------------
struct WatchItem {
    uint32_t address;
    int type;
    int32_t first;
    int32_t last;
    int32_t min_raw;
    int32_t max_raw;
    uint32_t changes;
};

struct WatchEvent {
    uint32_t t_ms;
    uint32_t vblank;
    uint32_t address; // 0 = marker (the "Tandai" button)
    int32_t old_value;
    int32_t new_value; // marker: its number
};

struct WatchState {
    std::mutex mutex;
    std::vector<WatchItem> items;
    std::deque<WatchEvent> events;
    uint64_t dropped = 0;
    int marks = 0;
    std::string app;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

constexpr size_t WATCH_MAX_ITEMS = 3000;
constexpr size_t WATCH_MAX_EVENTS = 400000;
constexpr uint32_t WATCH_NOISY_CHANGES = 600; // items changing more often are only summarised in the file

WatchState &watch_state() {
    static auto *instance = new WatchState(); // leaked on purpose, the watch thread outlives statics
    return *instance;
}

uint32_t watch_ms(const WatchState &w) {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - w.start).count());
}

float raw_to_float(int32_t raw) {
    float f;
    std::memcpy(&f, &raw, sizeof f);
    return f;
}

double watch_number(int32_t raw, int type) {
    return type == 1 ? static_cast<double>(raw_to_float(raw)) : static_cast<double>(raw);
}

const char *type_name(int type) {
    return type == 1 ? "float" : (type == 2 ? "byte" : (type == 3 ? "short" : "int"));
}

std::string hex_addr(uint32_t address) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", address);
    return buf;
}

std::string raw_text(int32_t raw, int type) {
    char buf[64];
    if (type == 1)
        std::snprintf(buf, sizeof buf, "%.7g", static_cast<double>(raw_to_float(raw)));
    else
        std::snprintf(buf, sizeof buf, "%d", raw);
    return buf;
}

// Both readings of a 4 byte value, e.g. "1122566144 (f=116.5)": a value found as Int may really be a float.
std::string raw_pair_text(int32_t raw, int type) {
    if (type == 2 || type == 3)
        return raw_text(raw, type);
    char buf[96];
    if (type == 1)
        std::snprintf(buf, sizeof buf, "%.7g (i=%d)", static_cast<double>(raw_to_float(raw)), raw);
    else
        std::snprintf(buf, sizeof buf, "%d (f=%.7g)", raw, static_cast<double>(raw_to_float(raw)));
    return buf;
}

std::string file_stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof buf, "%m%d_%H%M%S", &tm_buf);
    return buf;
}

// Writes <cheats dir>/<game>_<kind>_<MMDD_HHMMSS>.txt, returns its path or "" on failure.
std::string write_report(EmuEnvState &emuenv, const std::string &kind, const std::string &text) {
    const fs::path dir = cheats_dir(emuenv);
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / (emuenv.io.app_path + "_" + kind + "_" + file_stamp() + ".txt");
    std::ofstream out(file.string(), std::ios::binary);
    if (!out)
        return {};
    out << text;
    out.flush();
    return out ? file.string() : std::string();
}

bool watch_add_locked(WatchState &w, EmuEnvState &emuenv, uint32_t address, int type) {
    if (w.items.size() >= WATCH_MAX_ITEMS)
        return false;
    for (const auto &it : w.items) {
        if (it.address == address && it.type == type)
            return false;
    }
    int32_t v = 0;
    if (!cheat::read_value(emuenv.mem, address, v, cheat::type_size(type)))
        return false;
    if (w.items.empty())
        w.start = std::chrono::steady_clock::now();
    w.app = emuenv.io.app_path;
    w.items.push_back({ address, type, v, v, v, v, 0 });
    return true;
}

void watch_poll(EmuEnvState &emuenv) {
    auto &w = watch_state();
    std::lock_guard<std::mutex> lock(w.mutex);
    if (w.items.empty())
        return;
    if (w.app != emuenv.io.app_path) { // another game started: the old addresses mean nothing
        w.items.clear();
        w.events.clear();
        w.dropped = 0;
        w.marks = 0;
        return;
    }
    const uint32_t t = watch_ms(w);
    const uint32_t vblank = static_cast<uint32_t>(static_cast<uint64_t>(emuenv.display.vblank_count));
    for (auto &item : w.items) {
        int32_t v = 0;
        if (!cheat::read_value(emuenv.mem, item.address, v, cheat::type_size(item.type)))
            continue;
        if (v == item.last)
            continue;
        w.events.push_back({ t, vblank, item.address, item.last, v });
        if (w.events.size() > WATCH_MAX_EVENTS) {
            w.events.pop_front();
            w.dropped++;
        }
        item.last = v;
        item.changes++;
        const double n = watch_number(v, item.type);
        if (n < watch_number(item.min_raw, item.type))
            item.min_raw = v;
        if (n > watch_number(item.max_raw, item.type))
            item.max_raw = v;
    }
}

void watch_loop() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        auto *emuenv = get_emuenv();
        auto *controller = get_app_session_controller();
        if (!emuenv || !controller || !controller->has_active_session() || !controller->is_running())
            continue;
        watch_poll(*emuenv);
    }
}

struct WatchStarter {
    WatchStarter() {
        std::thread(watch_loop).detach();
    }
};
const WatchStarter g_watch_starter;

std::string watch_report(EmuEnvState &emuenv) {
    auto &w = watch_state();
    std::lock_guard<std::mutex> lock(w.mutex);
    std::string out;
    char buf[320];
    std::snprintf(buf, sizeof buf, "Vita3K watch log\ngame: %s\nitems: %zu  events: %zu  dropped: %llu  durasi_ms: %u  vblank_sekarang: %llu\n\n",
        emuenv.io.app_path.c_str(), w.items.size(), w.events.size(), static_cast<unsigned long long>(w.dropped), watch_ms(w),
        static_cast<unsigned long long>(static_cast<uint64_t>(emuenv.display.vblank_count)));
    out += buf;

    std::vector<const WatchItem *> sorted;
    for (const auto &it : w.items)
        sorted.push_back(&it);
    std::stable_sort(sorted.begin(), sorted.end(), [](const WatchItem *a, const WatchItem *b) { return a->changes > b->changes; });
    out += "[ITEMS] urut dari paling sering berubah: alamat tipe awal akhir min maks jumlah_perubahan\n";
    std::map<uint32_t, const WatchItem *> by_address;
    for (const auto *it : sorted) {
        by_address.emplace(it->address, it);
        out += hex_addr(it->address) + ' ' + type_name(it->type) + ' ' + raw_text(it->first, it->type) + ' ' + raw_text(it->last, it->type) + ' '
            + raw_text(it->min_raw, it->type) + ' ' + raw_text(it->max_raw, it->type) + ' ' + std::to_string(it->changes);
        if (it->changes > WATCH_NOISY_CHANGES)
            out += "  (berisik, tidak ditulis di log)";
        out += '\n';
    }

    out += "\n[LOG] t=ms v=vblank alamat tipe lama -> baru   (MARK = penanda dari tombol Tandai)\n";
    for (const auto &e : w.events) {
        if (e.address == 0) {
            out += "t=" + std::to_string(e.t_ms) + " v=" + std::to_string(e.vblank) + " MARK " + std::to_string(e.new_value) + '\n';
            continue;
        }
        const auto found = by_address.find(e.address);
        if (found == by_address.end() || found->second->changes > WATCH_NOISY_CHANGES)
            continue;
        const int type = found->second->type;
        out += "t=" + std::to_string(e.t_ms) + " v=" + std::to_string(e.vblank) + ' ' + hex_addr(e.address) + ' ' + type_name(type) + ' '
            + raw_text(e.old_value, type) + " -> " + raw_text(e.new_value, type) + '\n';
    }
    return out;
}


constexpr jlong ERR_NO_GAME = -3;

} // namespace

namespace macro {
void start_record(int slot);
void stop_record();
void start_play(int slot, int repeat);
void stop_play();
void clear(int slot);
int get_mode();
int get_active_slot();
size_t length(int slot);
bool save_slots(const std::string &path);
bool load_slots(const std::string &path);
} // namespace macro

extern "C" {

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_clear(JNIEnv *, jclass) {
    scanner().clear();
}

JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_search(JNIEnv *, jclass, jint raw, jint type, jfloat tolerance) {
    auto *emuenv = active_env();
    if (!emuenv)
        return ERR_NO_GAME;
    return scanner().search(emuenv->mem, raw, static_cast<cheat::ValueType>(type), tolerance);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_resetRandom(JNIEnv *, jclass) {
    cheat::psv::reset_random();
}

JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_searchBytes(JNIEnv *env, jclass, jbyteArray pattern) {
    auto *emuenv = active_env();
    if (!emuenv)
        return ERR_NO_GAME;
    const jsize len = env->GetArrayLength(pattern);
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    if (len > 0)
        env->GetByteArrayRegion(pattern, 0, len, reinterpret_cast<jbyte *>(bytes.data()));
    return scanner().search_bytes(emuenv->mem, bytes);
}

JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_snapshot(JNIEnv *, jclass) {
    auto *emuenv = active_env();
    if (!emuenv)
        return ERR_NO_GAME;
    return scanner().snapshot(emuenv->mem);
}

JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_compare(JNIEnv *, jclass, jint mode, jint type) {
    auto *emuenv = active_env();
    if (!emuenv)
        return ERR_NO_GAME;
    return scanner().compare_snapshot(emuenv->mem, static_cast<cheat::Compare>(mode), static_cast<cheat::ValueType>(type));
}

// mode < 0: exact match against `raw`; otherwise compare against each candidate's previous value.
JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_refine(JNIEnv *, jclass, jint mode, jint raw, jint type, jfloat tolerance) {
    auto *emuenv = active_env();
    if (!emuenv)
        return ERR_NO_GAME;
    std::optional<cheat::Compare> cmp;
    if (mode >= 0)
        cmp = static_cast<cheat::Compare>(mode);
    return scanner().refine(emuenv->mem, cmp, raw, static_cast<cheat::ValueType>(type), tolerance);
}

JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_count(JNIEnv *, jclass) {
    return static_cast<jlong>(scanner().count());
}

// Interleaved [address, value, address, value, ...]
JNIEXPORT jlongArray JNICALL
Java_org_vita3k_emulator_CheatNative_results(JNIEnv *env, jclass, jint offset, jint max) {
    const auto matches = scanner().results(static_cast<size_t>(std::max(0, static_cast<int>(offset))), static_cast<size_t>(std::max(0, static_cast<int>(max))));
    std::vector<jlong> flat;
    flat.reserve(matches.size() * 2);
    for (const auto &m : matches) {
        flat.push_back(static_cast<jlong>(m.address));
        flat.push_back(static_cast<jlong>(m.value));
    }
    jlongArray array = env->NewLongArray(static_cast<jsize>(flat.size()));
    if (array && !flat.empty())
        env->SetLongArrayRegion(array, 0, static_cast<jsize>(flat.size()), flat.data());
    return array;
}

// Returns the raw value, or Long.MIN_VALUE if the address cannot be read.
JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_readValue(JNIEnv *, jclass, jlong address, jint type) {
    auto *emuenv = active_env();
    int32_t value = 0;
    if (!emuenv || !cheat::read_value(emuenv->mem, static_cast<uint32_t>(address), value, cheat::type_size(type)))
        return INT64_MIN;
    return static_cast<jlong>(value);
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_CheatNative_writeValue(JNIEnv *, jclass, jlong address, jint raw, jint type) {
    auto *emuenv = active_env();
    return emuenv && cheat::write_value(emuenv->mem, static_cast<uint32_t>(address), raw, cheat::type_size(type)) ? JNI_TRUE : JNI_FALSE;
}

// One line per item: id \t psv(0/1) \t enabled(0/1) \t supported(0/1) \t address \t type \t raw value \t info \t name
JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_CheatNative_cheatList(JNIEnv *env, jclass) {
    auto *emuenv = active_env();
    std::string text;
    if (emuenv) {
        ensure_loaded(*emuenv);
        for (const auto &item : cheats().items()) {
            text += std::to_string(item.id) + '\t' + (item.psv ? '1' : '0') + '\t' + (item.enabled ? '1' : '0') + '\t' + (item.supported ? '1' : '0') + '\t' + std::to_string(item.address) + '\t' + std::to_string(item.type) + '\t' + std::to_string(item.value) + '\t' + clean_text(item.info) + '\t' + clean_text(item.name) + '\n';
        }
    }
    return env->NewStringUTF(text.c_str());
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_cheatSetManual(JNIEnv *env, jclass, jlong address, jint raw, jint type, jboolean enabled, jstring name) {
    auto *emuenv = active_env();
    if (!emuenv)
        return;
    ensure_loaded(*emuenv);
    std::string text;
    if (name) {
        const char *chars = env->GetStringUTFChars(name, nullptr);
        if (chars) {
            text = clean_text(chars);
            env->ReleaseStringUTFChars(name, chars);
        }
    }
    cheats().set_manual(static_cast<uint32_t>(address), raw, type, enabled == JNI_TRUE, text);
    autosave(*emuenv);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_cheatSetEnabled(JNIEnv *, jclass, jlong id, jboolean enabled) {
    auto *emuenv = active_env();
    if (!emuenv)
        return;
    ensure_loaded(*emuenv);
    cheats().set_enabled(id, enabled == JNI_TRUE);
    autosave(*emuenv);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_cheatRemove(JNIEnv *, jclass, jlong id) {
    auto *emuenv = active_env();
    if (!emuenv)
        return;
    ensure_loaded(*emuenv);
    cheats().remove(id);
    autosave(*emuenv);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_cheatSetAll(JNIEnv *, jclass, jboolean enabled) {
    auto *emuenv = active_env();
    if (!emuenv)
        return;
    ensure_loaded(*emuenv);
    cheats().set_all_enabled(enabled == JNI_TRUE);
    autosave(*emuenv);
}

// Re-reads the list file and the .psv file from disk; returns the number of items, -1 if no game runs.
JNIEXPORT jint JNICALL
Java_org_vita3k_emulator_CheatNative_cheatReload(JNIEnv *, jclass) {
    auto *emuenv = active_env();
    if (!emuenv)
        return -1;
    ensure_loaded(*emuenv);
    return cheats().reload(list_file(*emuenv), psv_file(*emuenv));
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_cheatSetPsvOffset(JNIEnv *, jclass, jlong offset) {
    auto *emuenv = active_env();
    if (!emuenv)
        return;
    ensure_loaded(*emuenv);
    cheats().set_psv_offset(static_cast<int32_t>(offset));
    autosave(*emuenv);
}

JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_cheatPsvOffset(JNIEnv *, jclass) {
    return static_cast<jlong>(cheats().psv_offset());
}

JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_CheatNative_cheatDir(JNIEnv *env, jclass) {
    auto *emuenv = active_env();
    const std::string dir = emuenv ? clean_text(cheats_dir(*emuenv).string()) : std::string();
    return env->NewStringUTF(dir.c_str());
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_macroRecordStart(JNIEnv *, jclass, jint slot) {
    macro::start_record(slot);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_macroRecordStop(JNIEnv *, jclass) {
    macro::stop_record();
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_macroPlayStart(JNIEnv *, jclass, jint slot, jint repeat) {
    macro::start_play(slot, repeat);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_macroPlayStop(JNIEnv *, jclass) {
    macro::stop_play();
}

JNIEXPORT jint JNICALL
Java_org_vita3k_emulator_CheatNative_macroMode(JNIEnv *, jclass) {
    return macro::get_mode();
}

JNIEXPORT jint JNICALL
Java_org_vita3k_emulator_CheatNative_macroActiveSlot(JNIEnv *, jclass) {
    return macro::get_active_slot();
}

JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_macroLength(JNIEnv *, jclass, jint slot) {
    return static_cast<jlong>(macro::length(slot));
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_macroClear(JNIEnv *, jclass, jint slot) {
    macro::clear(slot);
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_CheatNative_hasSnapshot(JNIEnv *, jclass) {
    return scanner().has_snapshot() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_CheatNative_macroSave(JNIEnv *env, jclass, jstring path) {
    const char *p = env->GetStringUTFChars(path, nullptr);
    const bool ok = macro::save_slots(p);
    env->ReleaseStringUTFChars(path, p);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_CheatNative_macroLoad(JNIEnv *env, jclass, jstring path) {
    const char *p = env->GetStringUTFChars(path, nullptr);
    const bool ok = macro::load_slots(p);
    env->ReleaseStringUTFChars(path, p);
    return ok ? JNI_TRUE : JNI_FALSE;
}


// Saves the current scan results (address, value when scanned, value now; 4 byte values both as int and float) to a text file.
// Returns the file path, or "" when it failed.
JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_CheatNative_exportResults(JNIEnv *env, jclass, jint max, jint type) {
    auto *emuenv = active_env();
    if (!emuenv)
        return env->NewStringUTF("");
    const size_t limit = static_cast<size_t>(std::max(1, std::min(static_cast<int>(max), 50000)));
    const auto matches = scanner().results(0, limit);
    const int size = cheat::type_size(type);
    std::string text;
    char buf[160];
    std::snprintf(buf, sizeof buf, "Vita3K scan results\ngame: %s\ntipe: %s\ntotal hasil: %zu  ditulis: %zu\n\n", emuenv->io.app_path.c_str(), type_name(type),
        scanner().count(), matches.size());
    text += buf;
    text += "alamat | nilai saat scan | nilai sekarang\n";
    for (const auto &m : matches) {
        int32_t scanned = m.value;
        if (type == 2)
            scanned &= 0xFF;
        else if (type == 3)
            scanned &= 0xFFFF;
        int32_t now = 0;
        const bool ok = cheat::read_value(emuenv->mem, m.address, now, size);
        text += hex_addr(m.address) + " | " + raw_pair_text(scanned, type) + " | " + (ok ? raw_pair_text(now, type) : std::string("?")) + '\n';
    }
    const std::string path = write_report(*emuenv, "scan", text);
    return env->NewStringUTF(clean_text(path).c_str());
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_CheatNative_watchAdd(JNIEnv *, jclass, jlong address, jint type) {
    auto *emuenv = active_env();
    if (!emuenv)
        return JNI_FALSE;
    auto &w = watch_state();
    std::lock_guard<std::mutex> lock(w.mutex);
    return watch_add_locked(w, *emuenv, static_cast<uint32_t>(address), type) ? JNI_TRUE : JNI_FALSE;
}

// Adds the first `max` scan results to the watch list. Returns how many were added, -1 when no game runs.
JNIEXPORT jint JNICALL
Java_org_vita3k_emulator_CheatNative_watchAddResults(JNIEnv *, jclass, jint max, jint type) {
    auto *emuenv = active_env();
    if (!emuenv)
        return -1;
    const auto matches = scanner().results(0, static_cast<size_t>(std::max(0, static_cast<int>(max))));
    auto &w = watch_state();
    std::lock_guard<std::mutex> lock(w.mutex);
    int added = 0;
    for (const auto &m : matches) {
        if (watch_add_locked(w, *emuenv, m.address, type))
            added++;
    }
    return added;
}

// Puts a numbered marker into the log (press it when you start an action) and returns its number.
JNIEXPORT jint JNICALL
Java_org_vita3k_emulator_CheatNative_watchMark(JNIEnv *, jclass) {
    auto *emuenv = active_env();
    auto &w = watch_state();
    std::lock_guard<std::mutex> lock(w.mutex);
    const int number = ++w.marks;
    const uint32_t vblank = emuenv ? static_cast<uint32_t>(static_cast<uint64_t>(emuenv->display.vblank_count)) : 0;
    w.events.push_back({ watch_ms(w), vblank, 0, 0, number });
    if (w.events.size() > WATCH_MAX_EVENTS) {
        w.events.pop_front();
        w.dropped++;
    }
    return number;
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_watchClear(JNIEnv *, jclass) {
    auto &w = watch_state();
    std::lock_guard<std::mutex> lock(w.mutex);
    w.items.clear();
    w.events.clear();
    w.dropped = 0;
    w.marks = 0;
}

// [items, events, dropped events, marks]
JNIEXPORT jlongArray JNICALL
Java_org_vita3k_emulator_CheatNative_watchStatus(JNIEnv *env, jclass) {
    auto &w = watch_state();
    jlong values[4];
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        values[0] = static_cast<jlong>(w.items.size());
        values[1] = static_cast<jlong>(w.events.size());
        values[2] = static_cast<jlong>(w.dropped);
        values[3] = static_cast<jlong>(w.marks);
    }
    jlongArray array = env->NewLongArray(4);
    if (array)
        env->SetLongArrayRegion(array, 0, 4, values);
    return array;
}

// Writes the watch summary + change log to a text file. Returns the file path, or "" when it failed.
JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_CheatNative_watchSave(JNIEnv *env, jclass) {
    auto *emuenv = active_env();
    if (!emuenv)
        return env->NewStringUTF("");
    const std::string path = write_report(*emuenv, "watch", watch_report(*emuenv));
    return env->NewStringUTF(clean_text(path).c_str());
}

} // extern "C"
