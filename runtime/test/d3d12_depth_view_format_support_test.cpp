// CheckFeatureSupport(FORMAT_SUPPORT) for the formats a shader reads depth through, on the Vulkan-backed device and
// on Windows D3D12 (the default adapter, or WARP with PROBE_WARP=1). Compared per format: the HRESULT and the
// Support1 bits that say how a shader resource view of the format is read (2D, cube, mips, load, sample, gather and
// their comparison forms). The depth- and stencil-plane formats (R24_UNORM_X8_TYPELESS, X24_TYPELESS_G8_UINT,
// R32_FLOAT_X8X24_TYPELESS, X32_TYPELESS_G8X24_UINT) are what an engine asks before it makes a depth SRV, and
// R32_FLOAT, R16_UNORM and the plane formats are where it looks for comparison sampling for shadow maps. A few
// colour, depth and typeless formats are controls. No game data.
#include "host_wine.h"
#include "../shims/d3d12_com.h"
#include <dxgi1_4.h>

#include <cstdio>

namespace {
template<class T>
struct Ref {
    T* p = nullptr;
    ~Ref() {
        if (p) p->Release();
    }
};

constexpr UINT kViewBits = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TEXTURECUBE |
                           D3D12_FORMAT_SUPPORT1_MIP | D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
                           D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON |
                           D3D12_FORMAT_SUPPORT1_SHADER_GATHER | D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON;

struct Row {
    const char* name;
    DXGI_FORMAT format;
};

const Row kRows[] = {
    {"R24_UNORM_X8_TYPELESS", DXGI_FORMAT_R24_UNORM_X8_TYPELESS},
    {"X24_TYPELESS_G8_UINT", DXGI_FORMAT_X24_TYPELESS_G8_UINT},
    {"R32_FLOAT_X8X24_TYPELESS", DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS},
    {"X32_TYPELESS_G8X24_UINT", DXGI_FORMAT_X32_TYPELESS_G8X24_UINT},
    {"R32_FLOAT", DXGI_FORMAT_R32_FLOAT},
    {"R16_UNORM", DXGI_FORMAT_R16_UNORM},
    // Controls: no comparison sampling, or no shader reads of their own.
    {"R16_FLOAT", DXGI_FORMAT_R16_FLOAT},
    {"R32_UINT", DXGI_FORMAT_R32_UINT},
    {"R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM},
    {"R11G11B10_FLOAT", DXGI_FORMAT_R11G11B10_FLOAT},
    {"BC1_UNORM", DXGI_FORMAT_BC1_UNORM},
    {"D32_FLOAT", DXGI_FORMAT_D32_FLOAT},
    {"D24_UNORM_S8_UINT", DXGI_FORMAT_D24_UNORM_S8_UINT},
    {"D32_FLOAT_S8X24_UINT", DXGI_FORMAT_D32_FLOAT_S8X24_UINT},
    {"D16_UNORM", DXGI_FORMAT_D16_UNORM},
    {"R32_TYPELESS", DXGI_FORMAT_R32_TYPELESS},
    {"R24G8_TYPELESS", DXGI_FORMAT_R24G8_TYPELESS},
    {"R32G8X24_TYPELESS", DXGI_FORMAT_R32G8X24_TYPELESS},
    {"R16_TYPELESS", DXGI_FORMAT_R16_TYPELESS},
};

HRESULT ask(ID3D12Device* device, DXGI_FORMAT format, UINT& view_bits) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT data{};
    data.Format = format;
    const HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &data, sizeof(data));
    view_bits = SUCCEEDED(hr) ? (static_cast<UINT>(data.Support1) & kViewBits) : 0;
    return hr;
}
}  // namespace

int main() {
    Ref<ID3D12Device> ours;
    if (FAILED(rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&ours.p))) || !ours.p) {
        std::printf("SKIP: no Vulkan device\n");
        return 77;
    }
    if (hostwine::host_is_wine()) {
        std::printf("SKIP: the host comparison is skipped under Wine\n");
        return 77;
    }
    char env[4] = {};
    const bool warp = GetEnvironmentVariableA("PROBE_WARP", env, sizeof(env)) == 1 && env[0] == '1';
    Ref<IDXGIFactory4> factory;
    Ref<IDXGIAdapter> adapter;
    Ref<ID3D12Device> host;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory.p))) ||
        (warp && FAILED(factory.p->EnumWarpAdapter(IID_PPV_ARGS(&adapter.p)))) ||
        FAILED(D3D12CreateDevice(adapter.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&host.p)))) {
        std::printf("SKIP: no host D3D12 device\n");
        return 77;
    }
    const char* host_name = warp ? "WARP" : "the default adapter";

    unsigned differ = 0;
    for (const Row& row : kRows) {
        UINT mine = 0, theirs = 0;
        const HRESULT my_hr = ask(ours.p, row.format, mine);
        const HRESULT host_hr = ask(host.p, row.format, theirs);
        const bool same = my_hr == host_hr && mine == theirs;
        std::printf("%-26s layer 0x%08lx view bits 0x%08x, %s 0x%08lx view bits 0x%08x%s\n", row.name,
                    static_cast<unsigned long>(my_hr), mine, host_name, static_cast<unsigned long>(host_hr), theirs,
                    same ? "" : "  DIFFERS");
        if (!same) ++differ;
    }
    std::printf("depth view format support against %s: %zu formats compared, %u differ\n", host_name,
                sizeof(kRows) / sizeof(kRows[0]), differ);
    return differ ? 1 : 0;
}
