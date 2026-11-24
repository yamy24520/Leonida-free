#pragma once

// LEONIDA_PROBE, a bring-up switch for the first frames (docs/boot.md, "Useful switches"): the graphics pipelines
// whose translated shader names (the entry names of spirv/index.json) contain a given text get built-in stages in
// place of their own, so a frame that stays black can be split into "the title's shader computes nothing visible"
// and "nothing reaches the target at all". PipelineState::create_graphics (shims/d3d12_vulkan_device.cpp) applies it.
//   LEONIDA_PROBE=<text>|all     which pipelines: a vertex, geometry or pixel shader name holds <text>, or every one
//   LEONIDA_PROBE_MODE=vs|ps|both which stages are replaced (default both)
//   LEONIDA_PROBE_POS=ndc|px[:<W>x<H>]  the built-in vertex shader passes the POSITION input through, as NDC or as
//                                pixels of a W x H target (bare px: 3840x2160), instead of a full-screen triangle
// The built-in pixel shader writes opaque green to render target 0. The built-in vertex shaders are in
// debug_probe_fullscreen.vert and debug_probe_position.vert, their SPIR-V in debug_probe_spirv.h.

#include "debug_probe_spirv.h"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace leonida {

struct DebugProbe {
    enum class Position { fullscreen, ndc, pixels };

    std::string match;  // empty: off; "all": every pipeline
    bool vertex = true;
    bool pixel = true;
    Position position = Position::fullscreen;
    float width = 3840.0f, height = 2160.0f;  // Position::pixels: the target the POSITION pixels are in
    std::string problem;                      // a value that was not understood, and what is used instead

    bool enabled() const { return !match.empty(); }
    bool matches(std::string_view name) const {
        return enabled() && !name.empty() && (match == "all" || name.find(match) != std::string_view::npos);
    }
};

// The three variables' values (null: unset), as LEONIDA_PROBE, LEONIDA_PROBE_MODE and LEONIDA_PROBE_POS.
inline DebugProbe parse_debug_probe(const char* probe, const char* mode, const char* pos) {
    DebugProbe out;
    if (!probe || !*probe) return out;
    out.match = probe;
    if (mode && *mode && std::strcmp(mode, "both") != 0) {
        if (std::strcmp(mode, "vs") == 0) out.pixel = false;
        else if (std::strcmp(mode, "ps") == 0) out.vertex = false;
        else out.problem = std::string("LEONIDA_PROBE_MODE=") + mode + " is not vs, ps or both: both";
    }
    if (pos && *pos) {
        if (std::strcmp(pos, "ndc") == 0) {
            out.position = DebugProbe::Position::ndc;
        } else if (std::strcmp(pos, "px") == 0) {
            out.position = DebugProbe::Position::pixels;
        } else if (std::strncmp(pos, "px:", 3) == 0) {
            char* end = nullptr;
            const unsigned long w = std::strtoul(pos + 3, &end, 10);
            const unsigned long h = end && *end == 'x' ? std::strtoul(end + 1, &end, 10) : 0;
            if (w && h && w <= 65536 && h <= 65536 && end && !*end) {
                out.position = DebugProbe::Position::pixels;
                out.width = float(w);
                out.height = float(h);
            } else {
                out.problem =
                    std::string("LEONIDA_PROBE_POS=") + pos + " is not px:<width>x<height>: full-screen triangle";
            }
        } else {
            out.problem = std::string("LEONIDA_PROBE_POS=") + pos + " is not ndc or px: full-screen triangle";
        }
    }
    return out;
}

namespace debug_probe_detail {

constexpr std::uint32_t kOpDecorate = 71, kOpSpecConstant = 50;
constexpr std::uint32_t kDecorationSpecId = 1, kDecorationLocation = 30;

}  // namespace debug_probe_detail

// debug_probe_position_vs with its input at `location` and its scale and offset specialization constants (SpecId
// 0 to 3) set to the given values; empty if the words do not have that shape.
inline std::vector<std::uint32_t> debug_probe_position_words(std::uint32_t location, const float (&scale_offset)[4]) {
    using namespace debug_probe_detail;
    std::vector<std::uint32_t> words(std::begin(debug_probe_position_vs), std::end(debug_probe_position_vs));
    std::uint32_t spec_target[4] = {};  // result id per SpecId
    unsigned locations = 0, spec_ids = 0, constants = 0;
    for (std::size_t i = 5; i < words.size();) {  // decorations come before the constants they name
        const std::uint32_t op = words[i] & 0xffffu, count = words[i] >> 16;
        if (!count || i + count > words.size()) return {};
        if (op == kOpDecorate && count == 4 && words[i + 2] == kDecorationLocation) {
            words[i + 3] = location;
            ++locations;
        } else if (op == kOpDecorate && count == 4 && words[i + 2] == kDecorationSpecId && words[i + 3] < 4) {
            spec_target[words[i + 3]] = words[i + 1];
            ++spec_ids;
        } else if (op == kOpSpecConstant && count == 4) {
            for (unsigned id = 0; id < 4; ++id) {
                if (spec_target[id] && spec_target[id] == words[i + 2]) {
                    words[i + 3] = std::bit_cast<std::uint32_t>(scale_offset[id]);
                    ++constants;
                }
            }
        }
        i += count;
    }
    if (locations != 1 || spec_ids != 4 || constants != 4) return {};
    return words;
}

// The probe vertex shader for these settings: the full-screen triangle, or the POSITION pass-through with its input
// at `position_location`.
inline std::vector<std::uint32_t> debug_probe_vertex_words(const DebugProbe& probe, std::uint32_t position_location) {
    switch (probe.position) {
    case DebugProbe::Position::ndc: return debug_probe_position_words(position_location, {1.0f, 1.0f, 0.0f, 0.0f});
    case DebugProbe::Position::pixels:  // D3D pixels are y down from the top-left corner; NDC y is up
        return debug_probe_position_words(position_location,
                                          {2.0f / probe.width, -2.0f / probe.height, -1.0f, 1.0f});
    case DebugProbe::Position::fullscreen: break;
    }
    return {std::begin(debug_probe_fullscreen_vs), std::end(debug_probe_fullscreen_vs)};
}

inline std::vector<std::uint32_t> debug_probe_pixel_words() {
    return {std::begin(debug_probe_ps), std::end(debug_probe_ps)};
}

}  // namespace leonida
