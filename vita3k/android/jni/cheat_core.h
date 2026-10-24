// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#pragma once

// Live guest-memory value scanner and freezer ("cheat search"), Cheat Engine / Game Guardian style.
// Values are 4 bytes, read as signed 32-bit integers or IEEE floats. Written for Vita3K's MemState.

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cheat_psv.h"

struct MemState;

namespace cheat {

enum class ValueType : int { Int32 = 0, Float32 = 1, Int8 = 2, Int16 = 3 };

// Size in bytes of a value type, given as the plain int used by the frozen list / JNI (0=int32, 1=float, 2=int8, 3=int16).
constexpr int type_size(int type) {
    return type == 2 ? 1 : (type == 3 ? 2 : 4);
}
constexpr int value_size(ValueType type) {
    return type_size(static_cast<int>(type));
}
enum class Compare : int { Increased = 0, Decreased = 1, Unchanged = 2, Changed = 3 };

struct Match {
    uint32_t address;
    int32_t value; // raw 32 bits as of the scan that produced it
};

constexpr size_t MAX_RESULTS = 4000000;
constexpr uint64_t MAX_SNAPSHOT_BYTES = 768ull * 1024 * 1024;

class Scanner {
public:
    // All scans return the candidate count (>= 0), or -1 if the snapshot would be too large,
    // -2 if there is no snapshot to compare against.
    int64_t search(MemState &mem, int32_t needle, ValueType type, float tolerance);
    // Cari urutan byte persis di memori (nama/teks). Kandidat menyimpan 4 byte pertama.
    int64_t search_bytes(MemState &mem, const std::vector<uint8_t> &pattern);
    int64_t snapshot(MemState &mem); // returns bytes captured
    int64_t compare_snapshot(MemState &mem, Compare cmp, ValueType type);
    int64_t refine(MemState &mem, std::optional<Compare> cmp, int32_t needle, ValueType type, float tolerance);
    std::vector<Match> results(size_t offset, size_t max) const;
    size_t count() const;
    void clear();

private:
    struct Run {
        uint32_t start;
        std::vector<uint8_t> data;
    };
    mutable std::mutex mutex_;
    std::vector<Match> candidates_;
    std::vector<Run> snapshot_;
};

// The user's cheat list: manual address entries (from the search) and cheats of a VitaCheat .psv file.
// Every item can be switched on/off without losing it; the list is stored per game.
class CheatList {
public:
    struct Item {
        int64_t id; // manual: the address; psv cheat #n: -(n + 1)
        bool psv;
        bool enabled;
        bool supported;
        uint32_t address;
        int32_t value;
        int type;
        std::string name;
        std::string info; // psv: what the last run did (empty if it has not run)
    };

    void set_manual(uint32_t address, int32_t value, int type, bool enabled, const std::string &name);
    bool set_enabled(int64_t id, bool enabled);
    bool remove(int64_t id); // manual entries only
    void set_all_enabled(bool enabled);
    void set_psv_offset(int32_t offset); // added to every static address of the .psv codes
    int32_t psv_offset() const;
    std::vector<Item> items() const;
    // Called periodically while a game runs. When `app` differs from the previous call the list is
    // reloaded from `list_file` (+ `psv_file`), then every enabled item is applied.
    void tick(MemState &mem, const std::string &app, const std::string &list_file, const std::string &psv_file);
    void reset_app(); // no game running: forget everything so the next game loads its own files
    void ensure_loaded(const std::string &app, const std::string &list_file, const std::string &psv_file);
    bool save(const std::string &list_file) const;
    int reload(const std::string &list_file, const std::string &psv_file); // returns item count

private:
    struct Manual {
        uint32_t address;
        int32_t value;
        int type;
        bool enabled;
        std::string name;
    };
    struct PsvItem {
        psv::Cheat cheat;
        bool enabled;
        bool ran = false;
        psv::RunStats stats;
    };
    void load_locked(const std::string &list_file, const std::string &psv_file);
    mutable std::mutex mutex_;
    std::map<uint32_t, Manual> manual_;
    std::vector<PsvItem> psv_;
    std::string active_app_;
    int32_t psv_offset_ = 0;
};

// `size` is 1, 2 or 4 bytes; values are zero-extended on read and truncated on write.
bool read_value(MemState &mem, uint32_t address, int32_t &out, int size = 4);
bool write_value(MemState &mem, uint32_t address, int32_t value, int size = 4);

} // namespace cheat
