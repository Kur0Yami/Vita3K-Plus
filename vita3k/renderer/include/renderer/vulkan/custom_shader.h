// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#pragma once

// User screen shaders written in Dolphin's post-processing GLSL dialect
// (GetCoordinates(), Sample(), SetOutput(), ...), compiled to SPIR-V at runtime with glslang.

#include <cstdint>
#include <string>
#include <vector>

namespace renderer::vulkan {

// Wraps a Dolphin-style user shader into a complete Vulkan GLSL 4.50 fragment shader.
std::string build_custom_fragment_source(const std::string &user_code);

// Compiles a complete GLSL 4.50 Vulkan fragment shader. On failure returns false and fills `log`.
bool compile_fragment_shader(const std::string &source, std::vector<uint32_t> &spirv, std::string &log);

} // namespace renderer::vulkan
