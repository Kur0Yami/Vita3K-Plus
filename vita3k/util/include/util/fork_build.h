#pragma once

// Fork build metadata, dipakai oleh vita3k/util/src/logging.cpp.
// Header ini tidak ikut ter-commit di repo upstream fork, jadi dibuat di sini
// supaya build CI (clean checkout) tidak gagal.
#define FORK_BUILD_SEQ 16
#define FORK_BUILD_CHANGES "combo keys; grid picker; keystore signing; java pad input; virtual combo buttons; overlay theme import (main menu only); dpad and joystick themed; per-button hide and scale (compile fixes; live refresh; merge with upstream focus handling)"
