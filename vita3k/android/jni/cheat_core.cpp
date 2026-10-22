// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include "cheat_core.h"

#include <mem/state.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace cheat {

namespace {

constexpr uint64_t PAGE = 4096;
constexpr uint64_t SCAN_START = 0x80000000ull; // user_main_memory_start
constexpr uint64_t SCAN_END = 0x100000000ull;
constexpr uint32_t CHUNK = 1u << 20;

struct Region {
    uint32_t start;
    uint64_t size;
};

std::vector<Region> valid_regions(const MemState &mem) {
    std::vector<Region> out;
    uint64_t page = SCAN_START / PAGE;
    const uint64_t end = SCAN_END / PAGE;
    while (page < end) {
        if (!is_valid_addr(mem, static_cast<Address>(page * PAGE))) {
            ++page;
            continue;
        }
        const uint64_t first = page;
        while (page < end && is_valid_addr(mem, static_cast<Address>(page * PAGE)))
            ++page;
        out.push_back({ static_cast<uint32_t>(first * PAGE), (page - first) * PAGE });
    }
    return out;
}

int32_t load_value(const uint8_t *p, int size) {
    if (size == 1)
        return p[0];
    if (size == 2) {
        uint16_t x;
        std::memcpy(&x, p, 2);
        return x;
    }
    int32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

float as_float(int32_t raw) {
    float f;
    std::memcpy(&f, &raw, sizeof(f));
    return f;
}

bool matches(int32_t value, int32_t needle, ValueType type, float tolerance) {
    if (type != ValueType::Float32)
        return value == needle;
    const float f = as_float(value);
    const float n = as_float(needle);
    if (!std::isfinite(f) || !std::isfinite(n))
        return false;
    return tolerance <= 0.0f ? value == needle : std::fabs(f - n) <= tolerance;
}

bool compare_ok(int32_t cur, int32_t prev, Compare cmp, ValueType type) {
    if (type != ValueType::Float32) {
        switch (cmp) {
        case Compare::Increased: return cur > prev;
        case Compare::Decreased: return cur < prev;
        case Compare::Unchanged: return cur == prev;
        case Compare::Changed: return cur != prev;
        }
        return false;
    }
    const float a = as_float(cur);
    const float b = as_float(prev);
    if (!std::isfinite(a) || !std::isfinite(b))
        return false;
    switch (cmp) {
    case Compare::Increased: return a > b;
    case Compare::Decreased: return a < b;
    case Compare::Unchanged: return a == b;
    case Compare::Changed: return a != b;
    }
    return false;
}

// Calls fn(base_address, buffer, size) for every readable chunk of guest memory.
template <typename Fn>
void for_each_chunk(MemState &mem, Fn &&fn) {
    std::vector<uint8_t> buf(CHUNK);
    for (const Region &r : valid_regions(mem)) {
        for (uint64_t off = 0; off < r.size; off += CHUNK) {
            const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(CHUNK, r.size - off));
            const uint32_t base = r.start + static_cast<uint32_t>(off);
            if (!debug_safe_copy_guest(mem, base, buf.data(), n))
                continue;
            if (!fn(base, buf.data(), n))
                return;
        }
    }
}

} // namespace

int64_t Scanner::search(MemState &mem, int32_t needle, ValueType type, float tolerance) {
    std::lock_guard<std::mutex> lock(mutex_);
    candidates_.clear();
    snapshot_.clear();
    const int sz = value_size(type);
    for_each_chunk(mem, [&](uint32_t base, const uint8_t *buf, uint32_t n) {
        for (uint32_t i = 0; i + sz <= n; i += sz) {
            const int32_t v = load_value(buf + i, sz);
            if (matches(v, needle, type, tolerance)) {
                candidates_.push_back({ base + i, v });
                if (candidates_.size() >= MAX_RESULTS)
                    return false;
            }
        }
        return true;
    });
    return static_cast<int64_t>(candidates_.size());
}

int64_t Scanner::search_bytes(MemState &mem, const std::vector<uint8_t> &pattern) {
    std::lock_guard<std::mutex> lock(mutex_);
    candidates_.clear();
    snapshot_.clear();
    if (pattern.empty())
        return 0;

    const size_t plen = pattern.size();
    const size_t keep = plen - 1; // byte yang disimpan untuk pencocokan lintas chunk
    std::vector<uint8_t> carry;
    uint32_t carry_base = 0;

    for_each_chunk(mem, [&](uint32_t base, const uint8_t *buf, uint32_t n) {
        std::vector<uint8_t> window;
        uint32_t wbase = base;
        if (!carry.empty() && carry_base + carry.size() == base) {
            window = carry;
            wbase = carry_base;
        }
        window.insert(window.end(), buf, buf + n);

        if (window.size() >= plen) {
            for (size_t i = 0; i + plen <= window.size(); ++i) {
                if (std::memcmp(window.data() + i, pattern.data(), plen) != 0)
                    continue;
                int32_t v = 0;
                std::memcpy(&v, window.data() + i, std::min<size_t>(4, plen));
                candidates_.push_back({ wbase + static_cast<uint32_t>(i), v });
                if (candidates_.size() >= MAX_RESULTS)
                    return false;
            }
        }

        const size_t k = std::min(keep, window.size());
        carry.assign(window.end() - k, window.end());
        carry_base = wbase + static_cast<uint32_t>(window.size() - k);
        return true;
    });
    return static_cast<int64_t>(candidates_.size());
}

int64_t Scanner::snapshot(MemState &mem) {
    std::lock_guard<std::mutex> lock(mutex_);
    candidates_.clear();
    snapshot_.clear();
    uint64_t total = 0;
    for (const Region &r : valid_regions(mem))
        total += r.size;
    if (total > MAX_SNAPSHOT_BYTES)
        return -1;
    uint64_t captured = 0;
    for_each_chunk(mem, [&](uint32_t base, const uint8_t *buf, uint32_t n) {
        snapshot_.push_back({ base, std::vector<uint8_t>(buf, buf + n) });
        captured += n;
        return true;
    });
    return static_cast<int64_t>(captured);
}

int64_t Scanner::compare_snapshot(MemState &mem, Compare cmp, ValueType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.empty())
        return -2;
    candidates_.clear();
    const int sz = value_size(type);
    std::vector<uint8_t> buf(CHUNK);
    for (const Run &run : snapshot_) {
        const uint32_t n = static_cast<uint32_t>(run.data.size());
        if (!debug_safe_copy_guest(mem, run.start, buf.data(), n))
            continue;
        for (uint32_t i = 0; i + sz <= n; i += sz) {
            const int32_t cur = load_value(buf.data() + i, sz);
            const int32_t prev = load_value(run.data.data() + i, sz);
            if (compare_ok(cur, prev, cmp, type)) {
                candidates_.push_back({ run.start + i, cur });
                if (candidates_.size() >= MAX_RESULTS)
                    break;
            }
        }
        if (candidates_.size() >= MAX_RESULTS)
            break;
    }
    snapshot_.clear();
    return static_cast<int64_t>(candidates_.size());
}

int64_t Scanner::refine(MemState &mem, std::optional<Compare> cmp, int32_t needle, ValueType type, float tolerance) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Match> kept;
    kept.reserve(candidates_.size());
    const int sz = value_size(type);
    for (const Match &c : candidates_) {
        uint8_t raw[4] = {};
        if (!debug_safe_copy_guest(mem, c.address, raw, sz))
            continue;
        const int32_t cur = load_value(raw, sz);
        const bool ok = cmp ? compare_ok(cur, c.value, *cmp, type) : matches(cur, needle, type, tolerance);
        if (ok)
            kept.push_back({ c.address, cur });
    }
    candidates_.swap(kept);
    return static_cast<int64_t>(candidates_.size());
}

std::vector<Match> Scanner::results(size_t offset, size_t max) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (offset >= candidates_.size())
        return {};
    const size_t end = std::min(candidates_.size(), offset + max);
    return std::vector<Match>(candidates_.begin() + offset, candidates_.begin() + end);
}

size_t Scanner::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return candidates_.size();
}

void Scanner::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    candidates_.clear();
    snapshot_.clear();
}

void CheatList::set_manual(uint32_t address, int32_t value, int type, bool enabled, const std::string &name) {
    std::lock_guard<std::mutex> lock(mutex_);
    manual_[address] = { address, value, type, enabled, name };
}

bool CheatList::set_enabled(int64_t id, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id >= 0) {
        auto it = manual_.find(static_cast<uint32_t>(id));
        if (it == manual_.end())
            return false;
        it->second.enabled = enabled;
        return true;
    }
    const size_t index = static_cast<size_t>(-id - 1);
    if (index >= psv_.size())
        return false;
    psv_[index].enabled = enabled && psv_[index].cheat.supported;
    psv_[index].ran = false;
    return true;
}

bool CheatList::remove(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return id >= 0 && manual_.erase(static_cast<uint32_t>(id)) > 0;
}

void CheatList::set_all_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &kv : manual_)
        kv.second.enabled = enabled;
    for (auto &p : psv_) {
        p.enabled = enabled && p.cheat.supported;
        p.ran = false;
    }
}

void CheatList::set_psv_offset(int32_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    psv_offset_ = offset;
    for (auto &p : psv_)
        p.ran = false;
}

int32_t CheatList::psv_offset() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return psv_offset_;
}

namespace {
std::string describe(const psv::RunStats &s) {
    std::string out;
    auto add = [&](const char *label, int n) {
        if (n > 0) {
            if (!out.empty())
                out += ", ";
            out += label;
            out += std::to_string(n);
        }
    };
    add("ditulis ", s.writes_ok);
    add("alamat tidak valid ", s.writes_bad);
    add("pointer gagal ", s.pointer_bad);
    add("kondisi belum terpenuhi ", s.cond_false);
    add("alamat kondisi tidak valid ", s.cond_bad);
    return out.empty() ? std::string("tidak ada aksi") : out;
}
} // namespace

std::vector<CheatList::Item> CheatList::items() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Item> out;
    for (const auto &kv : manual_) {
        const Manual &m = kv.second;
        out.push_back({ static_cast<int64_t>(m.address), false, m.enabled, true, m.address, m.value, m.type, m.name, std::string() });
    }
    for (size_t i = 0; i < psv_.size(); i++)
        out.push_back({ -static_cast<int64_t>(i) - 1, true, psv_[i].enabled, psv_[i].cheat.supported, 0, 0, 0, psv_[i].cheat.name,
                        (psv_[i].enabled && psv_[i].ran) ? describe(psv_[i].stats) : std::string() });
    return out;
}

void CheatList::tick(MemState &mem, const std::string &app, const std::string &list_file, const std::string &psv_file) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (app != active_app_) {
        active_app_ = app;
        load_locked(list_file, psv_file);
    }
    for (const auto &kv : manual_) {
        if (kv.second.enabled)
            write_value(mem, kv.first, kv.second.value, type_size(kv.second.type));
    }
    for (PsvItem &p : psv_) {
        if (!p.enabled)
            continue;
        psv::RunStats stats;
        psv::run(mem, p.cheat, static_cast<uint32_t>(psv_offset_), &stats);
        p.stats = stats;
        p.ran = true;
    }
}

void CheatList::ensure_loaded(const std::string &app, const std::string &list_file, const std::string &psv_file) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (app != active_app_) {
        active_app_ = app;
        load_locked(list_file, psv_file);
    }
}

void CheatList::reset_app() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_app_.clear();
    manual_.clear();
    psv_.clear();
    psv_offset_ = 0;
}

namespace {
std::string one_line(std::string s) {
    for (char &ch : s) {
        if (ch == '\n' || ch == '\r' || ch == '\t')
            ch = ' ';
    }
    return s;
}
} // namespace

bool CheatList::save(const std::string &list_file) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream out(list_file, std::ios::trunc);
    if (!out)
        return false;
    out << "# Vita3K cheat list. M <address hex> <type 0=int32 1=float 2=int8 3=int16> <raw value> <enabled> <name>\n";
    out << "# P <enabled> <name of a cheat in the .psv file>\n";
    out << "# O <address offset hex, added to every static address of the .psv codes>\n";
    char offset_line[32];
    std::snprintf(offset_line, sizeof(offset_line), "O %08X\n", static_cast<unsigned>(psv_offset_));
    out << offset_line;
    for (const auto &kv : manual_) {
        const Manual &m = kv.second;
        char head[96];
        std::snprintf(head, sizeof(head), "M %08X %d %d %d ", m.address, m.type, m.value, m.enabled ? 1 : 0);
        out << head << one_line(m.name) << "\n";
    }
    for (const PsvItem &p : psv_)
        out << "P " << (p.enabled ? 1 : 0) << " " << one_line(p.cheat.name) << "\n";
    return static_cast<bool>(out);
}

int CheatList::reload(const std::string &list_file, const std::string &psv_file) {
    std::lock_guard<std::mutex> lock(mutex_);
    load_locked(list_file, psv_file);
    return static_cast<int>(manual_.size() + psv_.size());
}

void CheatList::load_locked(const std::string &list_file, const std::string &psv_file) {
    manual_.clear();
    psv_.clear();
    psv_offset_ = 0;
    std::map<std::string, bool> psv_state;
    {
        std::ifstream in(list_file);
        std::string line;
        while (in && std::getline(in, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            if (line[0] == 'O') {
                unsigned value = 0;
                if (std::sscanf(line.c_str(), "O %x", &value) == 1)
                    psv_offset_ = static_cast<int32_t>(value);
            } else if (line[0] == 'P') {
                int en = 0, consumed = 0;
                if (std::sscanf(line.c_str(), "P %d %n", &en, &consumed) >= 1 && consumed > 0)
                    psv_state[line.substr(static_cast<size_t>(consumed))] = en != 0;
            } else if (line[0] == 'M') {
                unsigned address = 0;
                int type = 0, value = 0, en = 1, consumed = 0;
                if (std::sscanf(line.c_str(), "M %x %d %d %d %n", &address, &type, &value, &en, &consumed) >= 4 && consumed > 0)
                    manual_[address] = { address, value, type, en != 0, line.substr(static_cast<size_t>(consumed)) };
            } else {
                // legacy: "<address hex> <type> <value>", always enabled
                unsigned address = 0;
                int type = 0, value = 0;
                if (std::sscanf(line.c_str(), "%x %d %d", &address, &type, &value) == 3)
                    manual_[address] = { address, value, type, true, std::string() };
            }
        }
    }
    std::ifstream psv_in(psv_file, std::ios::binary);
    if (psv_in) {
        std::ostringstream text;
        text << psv_in.rdbuf();
        for (psv::Cheat &c : psv::parse(text.str())) {
            bool enabled = c.autostart;
            const auto st = psv_state.find(c.name);
            if (st != psv_state.end())
                enabled = st->second;
            enabled = enabled && c.supported;
            psv_.push_back({ std::move(c), enabled, false, psv::RunStats() });
        }
    }
}

bool read_value(MemState &mem, uint32_t address, int32_t &out, int size) {
    uint8_t raw[4] = {};
    if (!debug_safe_copy_guest(mem, address, raw, size))
        return false;
    out = load_value(raw, size);
    return true;
}

bool write_value(MemState &mem, uint32_t address, int32_t value, int size) {
    if (size == 1) {
        const uint8_t b = static_cast<uint8_t>(value);
        return debug_safe_write_guest(mem, address, &b, 1);
    }
    if (size == 2) {
        const uint16_t w = static_cast<uint16_t>(value);
        return debug_safe_write_guest(mem, address, &w, 2);
    }
    return debug_safe_write_guest(mem, address, &value, 4);
}

} // namespace cheat
