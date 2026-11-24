// The layer's DXGI swap chain against the host runtime's on the same calls: the order GetCurrentBackBufferIndex hands
// the back buffers out in over a run of presents and after ResizeBuffers, and the swap effect and buffer usage
// GetDesc/GetDesc1 give back from the creation description. The host side is the hardware adapter behind the layer's
// Vulkan device (hostadapter::reference), or WARP with PROBE_WARP=1.
//
// Host DXGI (RTX 5070 Ti and WARP, Windows 11) cycles the index 0, 1, ..., BufferCount-1 for a flip-model chain and
// starts again at 0 after ResizeBuffers; it reports the SwapEffect and BufferUsage the chain was created with
// (FLIP_SEQUENTIAL, RENDER_TARGET_OUTPUT | SHADER_INPUT, or a usage of 0).
// Exits 77 without an interactive window station, a Vulkan device or a host D3D12 device.
#include "vk_test_device.h"
#include "host_adapter.h"
#include "window_station.h"
#include "../shims/d3d12_com.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace {

using Results = std::map<std::string, std::string>;

std::string num(unsigned long long v) { return std::to_string(v); }
std::string hx(HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08lx", static_cast<unsigned long>(hr));
    return b;
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
HWND create_test_window(int w, int h) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LeonidaDxgiHostAnswersWnd";
    RegisterClassW(&wc);
    RECT rc{0, 0, w, h};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowExW(0, wc.lpszClassName, L"Leonida DXGI host answers", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                           CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance,
                           nullptr);
}

struct Side {
    IDXGIFactory4* factory = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    void release() {
        if (queue) queue->Release();
        if (factory) factory->Release();
        if (device) device->Release();
        *this = {};
    }
};

DXGI_SWAP_CHAIN_DESC1 chain_desc(DXGI_SWAP_EFFECT effect, UINT count) {
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = 128;
    d.Height = 128;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = count;
    d.SwapEffect = effect;
    return d;
}

// GetCurrentBackBufferIndex after creation and after each of 12 presents, then after ResizeBuffers and 6 more.
void index_order(const Side& s, Results& r, const char* name, const DXGI_SWAP_CHAIN_DESC1& desc) {
    HWND hwnd = create_test_window(128, 128);
    IDXGISwapChain1* sc1 = nullptr;
    IDXGISwapChain3* sc = nullptr;
    const HRESULT hr = s.factory->CreateSwapChainForHwnd(s.queue, hwnd, &desc, nullptr, nullptr, &sc1);
    std::string seq = hx(hr);
    if (SUCCEEDED(hr) && SUCCEEDED(sc1->QueryInterface(IID_PPV_ARGS(&sc)))) {
        seq += " ";
        seq += num(sc->GetCurrentBackBufferIndex());
        for (int i = 0; i < 12; ++i) {
            sc->Present(0, 0);
            seq += num(sc->GetCurrentBackBufferIndex());
        }
        seq += " resize " + hx(sc->ResizeBuffers(0, 96, 96, DXGI_FORMAT_UNKNOWN, 0)) + " ";
        seq += num(sc->GetCurrentBackBufferIndex());
        for (int i = 0; i < 6; ++i) {
            sc->Present(0, 0);
            seq += num(sc->GetCurrentBackBufferIndex());
        }
        sc->Release();
    }
    if (sc1) sc1->Release();
    DestroyWindow(hwnd);
    r[std::string("index order, ") + name] = seq;
}

// SwapEffect and BufferUsage as GetDesc1 and GetDesc report them.
void desc_round_trip(const Side& s, Results& r, const char* name, const DXGI_SWAP_CHAIN_DESC1& desc) {
    HWND hwnd = create_test_window(128, 128);
    IDXGISwapChain1* sc = nullptr;
    const HRESULT hr = s.factory->CreateSwapChainForHwnd(s.queue, hwnd, &desc, nullptr, nullptr, &sc);
    std::string got = hx(hr);
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 d1{};
        DXGI_SWAP_CHAIN_DESC d{};
        sc->GetDesc1(&d1);
        sc->GetDesc(&d);
        char b[128];
        std::snprintf(b, sizeof(b), " desc1 effect %d usage 0x%x, desc effect %d usage 0x%x", d1.SwapEffect,
                      d1.BufferUsage, d.SwapEffect, d.BufferUsage);
        got += b;
        sc->Release();
    }
    DestroyWindow(hwnd);
    r[std::string("desc, ") + name] = got;
}

void run(const Side& s, Results& r) {
    index_order(s, r, "FLIP_DISCARD x2", chain_desc(DXGI_SWAP_EFFECT_FLIP_DISCARD, 2));
    index_order(s, r, "FLIP_DISCARD x3", chain_desc(DXGI_SWAP_EFFECT_FLIP_DISCARD, 3));
    index_order(s, r, "FLIP_SEQUENTIAL x3", chain_desc(DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 3));
    index_order(s, r, "FLIP_DISCARD x4", chain_desc(DXGI_SWAP_EFFECT_FLIP_DISCARD, 4));

    DXGI_SWAP_CHAIN_DESC1 d = chain_desc(DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 2);
    desc_round_trip(s, r, "FLIP_SEQUENTIAL", d);
    d = chain_desc(DXGI_SWAP_EFFECT_FLIP_DISCARD, 2);
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    desc_round_trip(s, r, "usage RENDER_TARGET_OUTPUT | SHADER_INPUT", d);
    d.BufferUsage = 0;
    desc_round_trip(s, r, "usage 0", d);
}

bool make_host(bool warp, const vktest::Device& vk, Side& s, std::string& name) {
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&s.factory)))) return false;
    IDXGIAdapter* adapter = nullptr;
    if (!hostadapter::reference(s.factory, vk, warp, &adapter, name)) return false;
    const HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s.device));
    if (adapter) adapter->Release();
    if (FAILED(hr)) return false;
    const D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    return SUCCEEDED(s.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&s.queue)));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (!has_interactive_window_station()) {
        std::printf("SKIP: the process has no interactive window station\n");
        return 77;
    }
    vktest::Device vk;
    if (!vk.create(true)) {
        std::printf("SKIP: no Vulkan device\n");
        return 77;
    }
    char probe_warp[8] = {};
    const bool warp = GetEnvironmentVariableA("PROBE_WARP", probe_warp, sizeof(probe_warp)) > 0 &&
                      std::strcmp(probe_warp, "0") != 0;
    Side host;
    std::string host_name;
    if (!make_host(warp, vk, host, host_name)) {
        std::printf("SKIP: no host D3D12 device (%s)\n", warp ? "WARP" : "hardware");
        host.release();
        return 77;
    }

    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = vk.instance_proc;
    info.instance = vk.instance;
    info.physical = vk.physical;
    info.device = vk.device;
    info.queue = vk.queue;
    info.queue_family = vk.family;
    Side ours;
    const D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    if (FAILED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&ours.device))) ||
        FAILED(rt::d3d12::create_dxgi_factory(ours.device, IID_PPV_ARGS(&ours.factory))) ||
        FAILED(ours.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&ours.queue)))) {
        std::fprintf(stderr, "FAIL: layer device, factory or queue\n");
        return 1;
    }

    Results layer, reference;
    run(ours, layer);
    run(host, reference);
    int mismatches = 0;
    for (const auto& [key, value] : reference) {
        const std::string& got = layer[key];
        const bool same = got == value;
        mismatches += !same;
        std::printf("%s %s\n  layer: %s\n  host:  %s\n", same ? "ok  " : "DIFF", key.c_str(), got.c_str(),
                    value.c_str());
    }
    ours.release();
    host.release();
    if (vk.validation_errors()) {
        std::fprintf(stderr, "FAIL: %u validation errors\n", vk.validation_errors());
        return 1;
    }
    std::printf("dxgi_host_answers against %s: %d of %zu rows differ\n", host_name.c_str(), mismatches,
                reference.size());
    return mismatches ? 1 : 0;
}
