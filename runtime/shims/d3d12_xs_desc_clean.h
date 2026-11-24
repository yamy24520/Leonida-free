// The PC form of a resource description the retail title builds for the console, for the Xbox-ABI device proxy.
//
// The title's D3D12_RESOURCE_DESC carries console values the PC device cannot take: the layout word is 0x101 (tiled)
// or 0x1 (row-major buffer), and the flags have bits above the PC ones set (docs/retail_boot_chain.md, #6982). Slot 76
// replaces the layout word the same way before it forwards to CreateCommittedResource; slot 25 and the slot 127
// stand-in need the same cleaning. The title's own description is never written: a caller gets a copy.
#pragma once

#include <windows.h>
#include <d3d12.h>

#include <cstdint>

namespace rt::d3d12_xs {

// The D3D12_RESOURCE_FLAGS bits the PC runtime defines; every other bit is a console one.
constexpr std::uint32_t kPcResourceFlagsMask =
    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE |
    D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
    D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY | D3D12_RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY |
    D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;

// A word, not a D3D12_TEXTURE_LAYOUT: 0x101 is outside the enum's range, which clang-cl rejects in a constant.
constexpr std::uint32_t kXboxTiledResourceLayout = 0x101;

// Strip Xbox-only flags and translate the Xbox tiled layout. Preserve all other layouts so the host can validate them.
inline D3D12_RESOURCE_DESC pc_resource_desc(const D3D12_RESOURCE_DESC& xbox) {
    D3D12_RESOURCE_DESC pc = xbox;
    if (static_cast<std::uint32_t>(pc.Layout) == kXboxTiledResourceLayout) {
        pc.Layout = pc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                   : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    pc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<std::uint32_t>(xbox.Flags) & kPcResourceFlagsMask);
    return pc;
}

}  // namespace rt::d3d12_xs
