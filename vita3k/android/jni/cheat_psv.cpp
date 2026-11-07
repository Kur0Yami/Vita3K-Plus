// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include "cheat_psv.h"

#include "cheat_core.h"

#include <algorithm>
#include <random>
#include <unordered_map>
#include <utility>
#include <cstdio>
#include <sstream>

namespace cheat::psv {

namespace {

constexpr int MAX_LEVEL = 16;
// Angka acak untuk kode $6: dipilih sekali per alamat lalu dipakai terus sampai emulator dibuka ulang.
std::mt19937 &random_engine() {
    static std::mt19937 engine{ std::random_device{}() };
    return engine;
}

std::unordered_map<uint32_t, int32_t> &random_chosen() {
    static std::unordered_map<uint32_t, int32_t> chosen;
    return chosen;
}


int size_of_nibble(unsigned nib) {
    return nib == 0 ? 1 : (nib == 1 ? 2 : 4);
}

// Number of lines the code starting at lines[i] occupies, or 0 if unsupported / malformed.
size_t code_length(const std::vector<Line> &lines, size_t i) {
    const Line &l = lines[i];
    const unsigned type = l.op >> 12;
    const unsigned level = l.op & 0xFF;
    const size_t n = lines.size();
    switch (type) {
    case 0:
    case 5:
    case 0xD:
        return 1;
    case 6: // $6SS0 alamat min / $0000 max: angka acak min..max
        return i + 1 < n ? 2 : 0;
    case 4:
        return i + 1 < n ? 2 : 0;
    case 3:
        return (level >= 1 && level <= MAX_LEVEL && i + level < n) ? level + 1 : 0;
    case 7:
        return (level >= 1 && level <= MAX_LEVEL && i + level + 1 < n) ? level + 2 : 0;
    case 8: {
        if (level < 1 || level > MAX_LEVEL || i + level >= n)
            return 0;
        const size_t second = i + level + 1;
        if (second >= n)
            return 0;
        const unsigned level2 = lines[second].op & 0xFF;
        if (level2 < 1 || level2 > MAX_LEVEL || second + level2 >= n)
            return 0;
        return level + 1 + level2 + 1;
    }
    default:
        return 0;
    }
}

bool validate(const std::vector<Line> &lines) {
    for (size_t i = 0; i < lines.size();) {
        const size_t len = code_length(lines, i);
        if (len == 0)
            return false;
        i += len;
    }
    return true;
}

bool follow(MemState &mem, uint32_t base, const uint32_t *offsets, int count, uint32_t &out) {
    uint32_t addr = base;
    for (int k = 0; k < count; k++) {
        int32_t p;
        if (!read_value(mem, addr, p, 4))
            return false;
        addr = static_cast<uint32_t>(p) + offsets[k];
    }
    out = addr;
    return true;
}

// Returns false when the address cannot be read (readable=false) or the condition is not met (readable=true).
bool condition_holds(MemState &mem, const Line &l, uint32_t address, bool &readable) {
    readable = false;
    const unsigned sub = (l.op >> 8) & 0xF;
    if (sub > 0xB)
        return false;
    const int size = size_of_nibble(sub % 3);
    int32_t cur;
    if (!read_value(mem, address, cur, size))
        return false;
    readable = true;
    const uint32_t mask = size == 1 ? 0xFFu : (size == 2 ? 0xFFFFu : 0xFFFFFFFFu);
    const uint32_t have = static_cast<uint32_t>(cur) & mask;
    const uint32_t want = l.b & mask;
    switch (sub / 3) {
    case 0: return have == want;
    case 1: return have != want;
    case 2: return have > want;
    default: return have < want;
    }
}

} // namespace

std::vector<Cheat> parse(const std::string &text) {
    std::vector<Cheat> cheats;
    std::istringstream in(text);
    std::string line;
    Cheat *current = nullptr;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line.size() > 3 && line[0] == '_' && line[1] == 'V') {
            cheats.emplace_back();
            current = &cheats.back();
            current->autostart = line[2] == '1';
            size_t p = 3;
            while (p < line.size() && line[p] == ' ')
                ++p;
            current->name = line.substr(p);
            continue;
        }
        if (!current || line.empty() || line[0] != '$')
            continue;
        unsigned op = 0, a = 0, b = 0;
        if (std::sscanf(line.c_str(), "$%x %x %x", &op, &a, &b) == 3)
            current->lines.push_back({ static_cast<uint16_t>(op), a, b });
    }
    std::vector<Cheat> out;
    for (Cheat &c : cheats) {
        if (c.lines.empty())
            continue;
        c.supported = validate(c.lines);
        out.push_back(std::move(c));
    }
    return out;
}

void run(MemState &mem, const Cheat &cheat, uint32_t offset, RunStats *stats) {
    if (!cheat.supported)
        return;
    RunStats unused;
    RunStats &st = stats ? *stats : unused;
    auto put = [&](uint32_t addr, int32_t value, int size) {
        if (write_value(mem, addr, value, size))
            ++st.writes_ok;
        else
            ++st.writes_bad;
    };
    const auto &lines = cheat.lines;
    for (size_t i = 0; i < lines.size();) {
        const Line &l = lines[i];
        const unsigned type = l.op >> 12;
        const int size = size_of_nibble((l.op >> 8) & 0xF);
        const unsigned level = l.op & 0xFF;
        const size_t len = code_length(lines, i);
        if (len == 0)
            return;
        switch (type) {
        case 0:
            put(l.a + offset, static_cast<int32_t>(l.b), size);
            break;
        case 6: {
            const uint32_t addr = l.a + offset;
            int32_t lo = static_cast<int32_t>(l.b);
            int32_t hi = static_cast<int32_t>(lines[i + 1].a);
            if (lo > hi)
                std::swap(lo, hi);
            auto &chosen = random_chosen();
            auto it = chosen.find(addr);
            int32_t v;
            if (it == chosen.end()) {
                std::uniform_int_distribution<int32_t> dist(lo, hi);
                v = dist(random_engine());
                chosen[addr] = v;
            } else {
                v = it->second;
            }
            put(addr, v, size);
            break;
        }
        case 5: {
            int32_t v;
            if (read_value(mem, l.b + offset, v, size))
                put(l.a + offset, v, size);
            else
                ++st.writes_bad;
            break;
        }
        case 4: {
            const Line &g = lines[i + 1];
            for (uint32_t k = 0; k < g.op; k++)
                put(l.a + offset + k * g.a, static_cast<int32_t>(l.b + k * g.b), size);
            break;
        }
        case 3: {
            uint32_t offsets[MAX_LEVEL];
            offsets[0] = l.b;
            for (unsigned k = 1; k < level; k++)
                offsets[k] = lines[i + k].b;
            uint32_t target;
            if (follow(mem, l.a + offset, offsets, static_cast<int>(level), target))
                put(target, static_cast<int32_t>(lines[i + level].b), size);
            else
                ++st.pointer_bad;
            break;
        }
        case 7: {
            uint32_t offsets[MAX_LEVEL];
            offsets[0] = l.b;
            for (unsigned k = 1; k < level; k++)
                offsets[k] = lines[i + k].b;
            const Line &val = lines[i + level];
            const Line &g = lines[i + level + 1];
            for (uint32_t k = 0; k < g.op; k++) {
                uint32_t o[MAX_LEVEL];
                for (unsigned j = 0; j < level; j++)
                    o[j] = offsets[j];
                o[level - 1] += k * g.a;
                uint32_t target;
                if (follow(mem, l.a + offset, o, static_cast<int>(level), target))
                    put(target, static_cast<int32_t>(val.b + k * g.b), size);
                else
                    ++st.pointer_bad;
            }
            break;
        }
        case 8: {
            uint32_t dst_off[MAX_LEVEL], src_off[MAX_LEVEL];
            dst_off[0] = l.b;
            for (unsigned k = 1; k < level; k++)
                dst_off[k] = lines[i + k].b;
            const size_t second = i + level + 1;
            const Line &s = lines[second];
            const unsigned level2 = s.op & 0xFF;
            src_off[0] = s.b;
            for (unsigned k = 1; k < level2; k++)
                src_off[k] = lines[second + k].b;
            const int src_size = size_of_nibble(((s.op >> 8) & 0xF) - 4);
            uint32_t dst, src;
            int32_t v;
            if (follow(mem, l.a + offset, dst_off, static_cast<int>(level), dst) && follow(mem, s.a + offset, src_off, static_cast<int>(level2), src) && read_value(mem, src, v, src_size))
                put(dst, v, size);
            else
                ++st.pointer_bad;
            break;
        }
        case 0xD: {
            bool readable;
            if (!condition_holds(mem, l, l.a + offset, readable)) {
                if (readable)
                    ++st.cond_false;
                else
                    ++st.cond_bad;
                i += 1 + level; // skip the related lines
                continue;
            }
            break;
        }
        default:
            return;
        }
        i += len;
    }
}

void reset_random() {
    random_chosen().clear();
}

} // namespace cheat::psv
