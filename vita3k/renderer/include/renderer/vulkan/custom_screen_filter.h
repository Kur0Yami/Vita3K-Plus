// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#pragma once

#include "renderer/vulkan/screen_filters.h"

#include <string>

namespace renderer::vulkan {

// Screen filter running a user shader from <emulator folder>/shaders-custom/<file> (Dolphin post-processing GLSL).
// Selected with the config value "Shader:<file>". If the shader fails to compile the plain bilinear shader is used
// and the compiler messages are written to <file>.log next to the shader.
class CustomScreenFilter : public SinglePassScreenFilter {
    std::string file;
    std::string name;

protected:
    vk::ShaderModule load_fragment_shader(const std::string &builtin_file) override;
    vk::Sampler create_sampler() override;

public:
    CustomScreenFilter(ScreenRenderer &screen, std::string file_name);

    std::string_view get_name() override {
        return name;
    }
};

} // namespace renderer::vulkan
