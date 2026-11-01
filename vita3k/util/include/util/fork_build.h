#pragma once

// Fork build metadata, dipakai oleh vita3k/util/src/logging.cpp.
// Header ini tidak ikut ter-commit di repo upstream fork, jadi dibuat di sini
// supaya build CI (clean checkout) tidak gagal.
#define FORK_BUILD_SEQ 21
#define FORK_BUILD_CHANGES "combo keys; grid picker; keystore signing; java pad input; virtual combo buttons; overlay theme import (main menu only); dpad and joystick themed; per-button hide and scale; per-game stub pool toggle; overlay button grid panel with per-button opacity (compile fixes; live refresh; merge with upstream focus handling); L2/R2/L3/R3/touch/hide visibility moved into the button grid; combo key panel moved to Controls tab; combo keys saved per game id; cheat tab: watch list + save scan results to file; cheat tab keeps value/type when the menu is closed"
