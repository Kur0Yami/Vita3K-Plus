// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include <renderer/vulkan/custom_shader.h>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include <SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <mutex>
#include <sstream>

namespace renderer::vulkan {

namespace {

// Matches the screen pipeline: vertex shader outputs uv at location 0, the framebuffer is bound at binding 0 and
// the push constants are filled by SinglePassScreenFilter::render (see ScreenFilterPush).
const char *const HEADER = R"GLSL(#version 450
layout(location = 0) in vec2 uv_frag;
layout(location = 0) out vec4 ocol0;
layout(binding = 0) uniform sampler2D samp0;
layout(push_constant) uniform PushBlock {
    vec2 inv_tex_size;
    vec2 src_res;
    vec4 uv_rect;
    vec4 misc; // x = time (s), yz = window resolution
} pc;

#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4
#define lerp mix
#define frac fract
#define rsqrt inversesqrt
float saturate(float x) { return clamp(x, 0.0, 1.0); }
vec2 saturate(vec2 x) { return clamp(x, 0.0, 1.0); }
vec3 saturate(vec3 x) { return clamp(x, 0.0, 1.0); }
vec4 saturate(vec4 x) { return clamp(x, 0.0, 1.0); }

vec2 GetResolution() { return pc.src_res; }
vec2 GetInvResolution() { return 1.0 / pc.src_res; }
vec2 GetWindowResolution() { return pc.misc.yz; }
vec2 GetInvWindowResolution() { return 1.0 / pc.misc.yz; }
float GetTime() { return pc.misc.x; }
float GetLayer() { return 0.0; }
vec2 GetFragmentCoord() { return gl_FragCoord.xy; }
// 0..1 over the visible game image, (0,0) = top left
vec2 GetCoordinates() { return (uv_frag - pc.uv_rect.xy) / (pc.uv_rect.zw - pc.uv_rect.xy); }
vec4 SampleLocation(vec2 location) { return texture(samp0, pc.uv_rect.xy + location * (pc.uv_rect.zw - pc.uv_rect.xy)); }
vec4 SampleOffset(ivec2 offset) { return SampleLocation(GetCoordinates() + vec2(offset) * GetInvResolution()); }
vec4 Sample() { return texture(samp0, uv_frag); }
void SetOutput(vec4 color) { ocol0 = color; }
#define GetOption(x) (x)
#define OptionEnabled(x) ((x) != 0)
)GLSL";

std::string trim(const std::string &s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos)
        return {};
    return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

// Dolphin's "[configuration]" block: turn every option into a constant holding its default value.
std::string options_to_constants(const std::string &code) {
    const auto begin = code.find("[configuration]");
    const auto end = code.find("[/configuration]");
    if (begin == std::string::npos || end == std::string::npos || end < begin)
        return {};

    std::string out;
    std::string kind, name, def;
    auto flush = [&]() {
        if (!kind.empty() && !name.empty() && !def.empty()) {
            int comps = 1;
            for (char c : def)
                comps += c == ',';
            comps = std::min(comps, 4);
            if (kind == "OptionBool") {
                // like Dolphin, test it with OptionEnabled(NAME)
                out += "const int " + name + " = " + (def == "true" || def == "1" ? "1" : "0") + ";\n";
            } else {
                const bool is_float = kind == "OptionRangeFloat";
                const std::string base = is_float ? "vec" : "ivec";
                const std::string scalar = is_float ? "float" : "int";
                out += "const " + (comps == 1 ? scalar : base + std::to_string(comps)) + " " + name + " = ";
                out += (comps == 1 ? scalar : base + std::to_string(comps)) + "(" + def + ");\n";
            }
        }
        kind.clear();
        name.clear();
        def.clear();
    };

    std::istringstream in(code.substr(begin, end - begin));
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.rfind("[Option", 0) == 0) {
            flush();
            kind = line.substr(1, line.size() - 2);
        } else if (line.rfind("OptionName", 0) == 0 && line.find('=') != std::string::npos) {
            name = trim(line.substr(line.find('=') + 1));
        } else if (line.rfind("DefaultValue", 0) == 0 && line.find('=') != std::string::npos) {
            def = trim(line.substr(line.find('=') + 1));
        }
    }
    flush();
    return out;
}

} // namespace

std::string build_custom_fragment_source(const std::string &user_code) {
    // drop any "#version" the user may have written, the header provides it
    std::string body;
    std::istringstream in(user_code);
    std::string line;
    while (std::getline(in, line)) {
        if (trim(line).rfind("#version", 0) == 0)
            continue;
        body += line;
        body += '\n';
    }
    return std::string(HEADER) + options_to_constants(user_code) + "#line 1\n" + body;
}

bool compile_fragment_shader(const std::string &source, std::vector<uint32_t> &spirv, std::string &log) {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });

    glslang::TShader shader(EShLangFragment);
    const char *strings[1] = { source.c_str() };
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
        log = std::string(shader.getInfoLog()) + shader.getInfoDebugLog();
        return false;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        log = std::string(program.getInfoLog()) + program.getInfoDebugLog();
        return false;
    }

    spirv.clear();
    glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), spirv);
    return !spirv.empty();
}

} // namespace renderer::vulkan
