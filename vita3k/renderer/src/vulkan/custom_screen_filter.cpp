// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include "renderer/vulkan/custom_screen_filter.h"

#include "renderer/vulkan/custom_shader.h"
#include "renderer/vulkan/screen_renderer.h"
#include "renderer/vulkan/state.h"
#include "util/fs.h"
#include "util/log.h"
#include "vkutil/vkutil.h"

#include <fstream>
#include <sstream>

namespace renderer::vulkan {

CustomScreenFilter::CustomScreenFilter(ScreenRenderer &screen, std::string file_name)
    : SinglePassScreenFilter(screen)
    , file(std::move(file_name))
    , name("Shader:" + file) {}

vk::ShaderModule CustomScreenFilter::load_fragment_shader(const std::string &builtin_file) {
    // only the file name counts, so the config can never point outside the shader folder
    const fs::path path = screen.state.vita_fs_path / "shaders-custom" / fs::path(file).filename();
    fs::path log_path = path;
    log_path += ".log";

    std::ifstream in(path.string(), std::ios::binary);
    if (!in) {
        LOG_ERROR("Custom screen shader {} not found, using the default filter", path.string());
        return SinglePassScreenFilter::load_fragment_shader(builtin_file);
    }
    std::stringstream text;
    text << in.rdbuf();

    std::vector<uint32_t> spirv;
    std::string log;
    if (!compile_fragment_shader(build_custom_fragment_source(text.str()), spirv, log)) {
        LOG_ERROR("Custom screen shader {} failed to compile:\n{}", path.string(), log);
        std::ofstream(log_path.string(), std::ios::trunc) << log;
        return SinglePassScreenFilter::load_fragment_shader(builtin_file);
    }

    boost::system::error_code ec;
    fs::remove(log_path, ec);
    LOG_INFO("Loaded custom screen shader {}", path.string());
    return vkutil::load_shader(screen.state.device, spirv.data(), static_cast<uint32_t>(spirv.size() * sizeof(uint32_t)));
}

vk::Sampler CustomScreenFilter::create_sampler() {
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    return screen.state.device.createSampler(sampler_info);
}

} // namespace renderer::vulkan
