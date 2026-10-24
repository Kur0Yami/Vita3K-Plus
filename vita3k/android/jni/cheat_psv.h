// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#pragma once

// Interpreter for VitaCheat ".psv" cheat files (https://github.com/r0ah/vitacheat/wiki).
// Supported code types: $0 write, $3 pointer write, $4 compression, $5 mov, $7 pointer compression,
// $8 pointer mov, $D condition. Button-pad ($C) and ARM-write codes are reported as unsupported.

#include <cstdint>
#include <string>
#include <vector>

struct MemState;

namespace cheat::psv {

struct Line {
    uint16_t op;
    uint32_t a;
    uint32_t b;
};

struct Cheat {
    std::string name;
    std::vector<Line> lines;
    bool autostart = false; // "_V1" in the file
    bool supported = true;
};

// Cheats without any code line (the "notes" some files use) are dropped.
std::vector<Cheat> parse(const std::string &text);

// What one execution of a cheat did; used to tell the user why a cheat has no effect.
struct RunStats {
    int writes_ok = 0;
    int writes_bad = 0;   // target address not mapped / not writable
    int pointer_bad = 0;  // a pointer in a chain could not be read
    int cond_false = 0;   // a condition was not met
    int cond_bad = 0;     // a condition's address could not be read
};

// Executes the cheat once (one "frame" of it). Safe on invalid addresses.
// `offset` is added to every static address of the code (pointer offsets are left alone).
void run(MemState &mem, const Cheat &cheat, uint32_t offset = 0, RunStats *stats = nullptr);
// Hapus angka acak yang sudah dipilih, supaya $6 memilih ulang saat cheat dijalankan lagi.
void reset_random();

} // namespace cheat::psv
