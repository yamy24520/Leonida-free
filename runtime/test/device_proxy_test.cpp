// Exercises the Xbox-ABI device proxy against a real WARP PC device, with no game executable.
//
// It creates the proxy through the registered d3d12_xs!D3D12XboxCreateDevice shim, then calls the measured Xbox
// slots exactly as the guest would (raw function pointers, not the C++ interface):
//   slot 14  CreateDescriptorHeap (returns a heap proxy)
//   slot 25/26 GetResourceAllocationInfo/GetCustomHeapProperties in the title's C form (return buffer first)
//   heap 9/10 GetCPU/GetGPUDescriptorHandleForHeapStart (Xbox: 8-byte handle in RAX; the PC heap uses the
//             hidden return pointer in RDX, so slot 14 wraps it)
//   slot 62  CreateComputePipelineState
//   slot 76  resource creation with an Xbox layout word (0x101)
//   slot 8/36 CreateCommandQueue/CreateFence (return queue and fence proxies that run GPU work)
// Slot 9 on the device is CreateCommandAllocator on the PC vtable and must still forward, which proves the handle
// getters were not wedged into the device slots. Console output also shows the proxy's first-call slot log.
#include <vector>
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <wrl/client.h>

#include "d3d12_trace.h"
#include "d3d12_xs_desc_clean.h"
#include "d3d12_xs_device.h"
#include "shims.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (!condition) ++g_failures;
}

using Microsoft::WRL::ComPtr;

using XboxCreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, const void*, const IID*, void**);
using XboxCreateResourceFn = HRESULT(WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                              const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                              const D3D12_CLEAR_VALUE*, REFIID, void**);
// Xbox slot 62: (desc, two words, iid, out), as XboxBvhBuilder.dll calls it (zeros for the two words).
using XboxCreateComputePsoFn = HRESULT(WINAPI*)(void*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, std::uint64_t,
                                                std::uint64_t, REFIID, void**);
using GetCpuHandleFn = D3D12_CPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*);
using GetGpuHandleFn = D3D12_GPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*);

D3D12_RESOURCE_DESC buffer_desc() {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 4096;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);  // measured Xbox tiled/buffer word
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

D3D12_RESOURCE_DESC texture_desc() {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 4;
    desc.Height = 4;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

D3D12_HEAP_PROPERTIES default_heap() {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    return heap;
}

bool load_compute_shader(ComPtr<ID3DBlob>& bytecode) {
    HMODULE compiler = LoadLibraryExA("d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(compiler != nullptr, "d3dcompiler_47.dll loads");
    auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    check(compile != nullptr, "d3dcompiler_47.dll exports D3DCompile");
    if (!compile) return false;

    static const char kSource[] =
        "[numthreads(1, 1, 1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) { }\n";
    ComPtr<ID3DBlob> errors;
    HRESULT hr = compile(kSource, sizeof(kSource) - 1, "device_proxy_test.hlsl", nullptr, nullptr, "main",
                         "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, bytecode.GetAddressOf(),
                         errors.GetAddressOf());
    if (FAILED(hr) && errors) {
        std::printf("shader compile error: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
    }
    check(SUCCEEDED(hr) && bytecode, "trivial compute shader compiles (cs_5_0)");
    return SUCCEEDED(hr) && bytecode;
}

// ---- Slot 55 (CreateDerivedPipelineState) ----

constexpr std::size_t kRecordBytes = 0x14c;

// One record: u32 tag, then the payload at +4 (d3d12_xs_pso_stream.h).
void put_record(unsigned char* record, std::uint32_t tag, const void* payload, std::size_t size) {
    std::memset(record, 0, kRecordBytes);
    std::memcpy(record, &tag, sizeof(tag));
    if (size) std::memcpy(record + 4, payload, size);
}

std::size_t align_up(std::size_t value, std::size_t align) { return (value + align - 1) / align * align; }

// A stream written in the console's layout: u32 type, payload at its alignment, next entry 8-aligned.
struct StreamBytes {
    std::vector<unsigned char> bytes;
    void add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const void* payload, std::size_t size, std::size_t align) {
        const std::size_t at = bytes.size();
        bytes.resize(at + 4);
        const std::uint32_t t = type;
        std::memcpy(bytes.data() + at, &t, sizeof(t));
        bytes.resize(align_up(at + 4, align), 0);
        const auto* p = static_cast<const unsigned char*>(payload);
        bytes.insert(bytes.end(), p, p + size);
        bytes.resize(align_up(bytes.size(), 8), 0);
    }
};

bool compile_hlsl(const char* source, const char* target, ComPtr<ID3DBlob>& bytecode) {
    HMODULE compiler = LoadLibraryExA("d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) return false;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = compile(source, std::strlen(source), "device_proxy_slot55.hlsl", nullptr, nullptr, "main",
                               target, D3DCOMPILE_ENABLE_STRICTNESS, 0, bytecode.GetAddressOf(),
                               errors.GetAddressOf());
    if (FAILED(hr) && errors) std::printf("shader compile error: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
    return SUCCEEDED(hr) && bytecode;
}

// Graphics stream with every state a derived record can patch: DEPTH_STENCIL1, RASTERIZER, BLEND, SAMPLE_MASK.
std::vector<unsigned char> graphics_stream(ID3D12RootSignature* root, ID3DBlob* vs, ID3DBlob* ps) {
    StreamBytes s;
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &root, sizeof(root), alignof(void*));
    const D3D12_SHADER_BYTECODE vs_code{vs->GetBufferPointer(), vs->GetBufferSize()};
    const D3D12_SHADER_BYTECODE ps_code{ps->GetBufferPointer(), ps->GetBufferSize()};
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, &vs_code, sizeof(vs_code), alignof(void*));
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, &ps_code, sizeof(ps_code), alignof(void*));
    D3D12_RT_FORMAT_ARRAY rt{};
    rt.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt.NumRenderTargets = 1;
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, &rt, sizeof(rt), 4);
    const DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, &dsv, sizeof(dsv), 4);
    const DXGI_SAMPLE_DESC sample{1, 0};
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, &sample, sizeof(sample), 4);
    const D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, &topology, sizeof(topology), 4);

    D3D12_BLEND_DESC blend{};
    for (auto& rtb : blend.RenderTarget) {
        rtb.SrcBlend = rtb.SrcBlendAlpha = D3D12_BLEND_ONE;
        rtb.DestBlend = rtb.DestBlendAlpha = D3D12_BLEND_ZERO;
        rtb.BlendOp = rtb.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rtb.LogicOp = D3D12_LOGIC_OP_NOOP;
        rtb.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, &blend, sizeof(blend), 4);
    const UINT mask = 0xffffffffu;
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, &mask, sizeof(mask), 4);
    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = D3D12_CULL_MODE_BACK;
    raster.DepthClipEnable = TRUE;
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, &raster, sizeof(raster), 4);
    D3D12_DEPTH_STENCIL_DESC1 ds{};
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ds.StencilReadMask = ds.StencilWriteMask = 0xff;
    for (D3D12_DEPTH_STENCILOP_DESC* op : {&ds.FrontFace, &ds.BackFace}) {
        op->StencilFailOp = op->StencilDepthFailOp = op->StencilPassOp = D3D12_STENCIL_OP_KEEP;
        op->StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    }
    s.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, &ds, sizeof(ds), 4);
    return s.bytes;
}

void check_derived_states(ID3D12Device* device, void** proxy_vtbl, ID3D12RootSignature* root,
                          ID3D12PipelineState* compute_base, ID3D12PipelineState* native_base) {
    using DeriveFn = HRESULT(WINAPI*)(void*, void*, std::uint64_t, const void*, const IID*, void**);
    auto derive = reinterpret_cast<DeriveFn>(proxy_vtbl[55]);
    const IID iid = __uuidof(ID3D12PipelineState);
    unsigned char rec_tag5[kRecordBytes], rec_raster[kRecordBytes];
    unsigned char five[5 * kRecordBytes] = {};
    put_record(rec_tag5, 5, nullptr, 0);
    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = D3D12_FILL_MODE_WIREFRAME;
    raster.CullMode = D3D12_CULL_MODE_BACK;
    raster.DepthClipEnable = TRUE;
    put_record(rec_raster, 1, &raster, sizeof(raster));
    ComPtr<ID3D12PipelineState> out;

    // Refusals: a compute base has no graphics subobjects, a native compute PSO was never kept, and the arguments.
    out.Reset();
    check(derive(device, compute_base, 1, rec_tag5, &iid, reinterpret_cast<void**>(out.GetAddressOf())) == E_INVALIDARG
              && !out, "slot 55 refuses tag 5 (E_INVALIDARG, null output)");
    out.Reset();
    check(derive(device, compute_base, 1, rec_raster, &iid, reinterpret_cast<void**>(out.GetAddressOf())) == E_INVALIDARG
              && !out, "slot 55 refuses a rasterizer record on a base without RASTERIZER (E_INVALIDARG)");
    out.Reset();
    check(derive(device, native_base, 1, rec_raster, &iid, reinterpret_cast<void**>(out.GetAddressOf())) == E_FAIL
              && !out, "slot 55 refuses a base PSO that was never kept (null output, no derivation)");
    out.Reset();
    check(derive(device, compute_base, 1, nullptr, &iid, reinterpret_cast<void**>(out.GetAddressOf())) == E_POINTER,
          "slot 55 with null records is E_POINTER");
    out.Reset();
    check(derive(device, compute_base, 0, rec_raster, &iid, reinterpret_cast<void**>(out.GetAddressOf())) == E_INVALIDARG,
          "slot 55 with n = 0 is E_INVALIDARG");
    check(derive(device, compute_base, 5, five, &iid, reinterpret_cast<void**>(out.GetAddressOf())) == E_INVALIDARG,
          "slot 55 with n = 5 is E_INVALIDARG");
    check(derive(device, compute_base, 1, rec_raster, nullptr, reinterpret_cast<void**>(out.GetAddressOf())) == E_POINTER,
          "slot 55 with null riid is E_POINTER");

    // Positive: a graphics base through slot 86, then tags 0, 1 and 3 in one call, tag 2 alone, and tag 2 again on
    // the derived PSO (a derived PSO is a base too).
    ComPtr<ID3DBlob> vs, ps;
    if (!compile_hlsl("float4 main() : SV_Position { return float4(0, 0, 0, 1); }", "vs_5_0", vs) ||
        !compile_hlsl("float4 main() : SV_Target { return float4(0, 0, 0, 1); }", "ps_5_0", ps)) {
        check(false, "graphics shaders compile for the slot 55 positive test");
        return;
    }
    const std::vector<unsigned char> stream = graphics_stream(root, vs.Get(), ps.Get());
    D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{stream.size(), const_cast<unsigned char*>(stream.data())};
    using CreateStreamFn = HRESULT(WINAPI*)(void*, const D3D12_PIPELINE_STATE_STREAM_DESC*, const IID*, void**);
    ComPtr<ID3D12PipelineState> base;
    HRESULT hr = reinterpret_cast<CreateStreamFn>(proxy_vtbl[86])(device, &stream_desc, &iid,
                                                                  reinterpret_cast<void**>(base.GetAddressOf()));
    check(SUCCEEDED(hr) && base, "slot 86 creates the graphics base PSO (WARP accepts the stream)");
    if (!base) return;

    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ds.StencilReadMask = ds.StencilWriteMask = 0xff;
    for (D3D12_DEPTH_STENCILOP_DESC* op : {&ds.FrontFace, &ds.BackFace}) {
        op->StencilFailOp = op->StencilDepthFailOp = op->StencilPassOp = D3D12_STENCIL_OP_KEEP;
        op->StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    }
    unsigned char recs[3 * kRecordBytes];
    put_record(recs, 0, &ds, sizeof(ds));
    put_record(recs + kRecordBytes, 1, &raster, sizeof(raster));
    const UINT sample_mask = 0x1;
    put_record(recs + 2 * kRecordBytes, 3, &sample_mask, sizeof(sample_mask));
    ComPtr<ID3D12PipelineState> three;
    hr = derive(device, base.Get(), 3, recs, &iid, reinterpret_cast<void**>(three.GetAddressOf()));
    check(SUCCEEDED(hr) && three, "slot 55 derives a PSO from tags 0, 1 and 3 (n = 3) on the graphics base");

    D3D12_BLEND_DESC blend{};
    for (auto& rtb : blend.RenderTarget) {
        rtb.SrcBlend = rtb.SrcBlendAlpha = D3D12_BLEND_ONE;
        rtb.DestBlend = rtb.DestBlendAlpha = D3D12_BLEND_ZERO;
        rtb.BlendOp = rtb.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rtb.LogicOp = D3D12_LOGIC_OP_NOOP;
        rtb.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    // Four records (raster, depth-stencil, sample mask, blend): the largest count the console builds.
    unsigned char four_recs[4 * kRecordBytes];
    std::memcpy(four_recs, recs, 3 * kRecordBytes);
    put_record(four_recs + 3 * kRecordBytes, 2, &blend, sizeof(blend));
    ComPtr<ID3D12PipelineState> quad;
    hr = derive(device, base.Get(), 4, four_recs, &iid, reinterpret_cast<void**>(quad.GetAddressOf()));
    check(SUCCEEDED(hr) && quad, "slot 55 derives a PSO from four records (n = 4)");

    unsigned char rec_blend[kRecordBytes];
    put_record(rec_blend, 2, &blend, sizeof(blend));
    ComPtr<ID3D12PipelineState> blended;
    hr = derive(device, base.Get(), 1, rec_blend, &iid, reinterpret_cast<void**>(blended.GetAddressOf()));
    check(SUCCEEDED(hr) && blended, "slot 55 derives a PSO from tag 2 (blend), n = 1");

    ComPtr<ID3D12PipelineState> chained;
    hr = derive(device, three.Get(), 1, rec_blend, &iid, reinterpret_cast<void**>(chained.GetAddressOf()));
    check(SUCCEEDED(hr) && chained, "slot 55 derives from a derived PSO (its stream is kept too)");
}

}  // namespace

int main(int argc, char** argv) {
    // --trace-d3d12, as gta6rt takes it: device_proxy_trace checks that the parameter words still print then.
    if (argc == 2 && std::strcmp(argv[1], "--trace-d3d12") == 0) rt::d3d12_trace::set_enabled(true);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("device_proxy_test: Xbox-ABI device proxy over WARP\n");

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    check(SUCCEEDED(hr) && factory, "DXGI factory created");
    ComPtr<IDXGIAdapter> warp;
    if (factory) hr = factory->EnumWarpAdapter(IID_PPV_ARGS(warp.GetAddressOf()));
    if (!factory) {
        std::printf("device_proxy_test: FAILED (no DXGI factory)\n");
        return 1;
    }
    if (!warp) {
        // Windows always has WARP; Wine doesn't. CTest reads exit code 77 as a skip (SKIP_RETURN_CODE).
        std::printf("SKIP device_proxy_test: no WARP adapter (0x%08lX)\n", static_cast<unsigned long>(hr));
        return 77;
    }
    check(true, "WARP adapter enumerated");

    auto create_device = reinterpret_cast<XboxCreateDeviceFn>(
        rt::shims::find("d3d12_xs.dll", "D3D12XboxCreateDevice"));
    check(create_device != nullptr, "shim registry exposes d3d12_xs!D3D12XboxCreateDevice");
    if (!create_device) return 1;

    std::uint32_t params[24] = {0x1234abcd};  // logged only, not decoded; word 0 is device_proxy_trace's marker
    const IID iid_device = __uuidof(ID3D12Device);
    void* created = nullptr;
    hr = create_device(warp.Get(), params, &iid_device, &created);
    check(SUCCEEDED(hr) && created != nullptr, "D3D12XboxCreateDevice returns a device");
    if (!created) {
        std::printf("device_proxy_test: FAILED (no device)\n");
        return 1;
    }

    ComPtr<ID3D12Device> device;
    device.Attach(static_cast<ID3D12Device*>(created));
    check(rt::d3d12_xs::is_device_proxy(device.Get()), "returned pointer is an Xbox-ABI proxy");

    ID3D12Device* real = rt::d3d12_xs::device_proxy_real_device(device.Get());
    check(real != nullptr, "proxy exposes its PC device");
    unsigned slots = rt::d3d12_xs::device_proxy_slot_count(device.Get());
    std::printf("proxy forwards %u PC vtable slots (ID3D12Device..Device14 max is 83)\n", slots);
    check(slots >= 44, "proxy forwards at least the base ID3D12Device vtable");
    check(rt::d3d12_xs::device_slot_overridden(9), "device slot 9 is CreateCommandAllocator (allocator proxies)");
    check(!rt::d3d12_xs::device_slot_overridden(10), "device slot 10 is forwarded (PC CreateGraphicsPipelineState)");
    check(rt::d3d12_xs::device_slot_overridden(14), "device slot 14 is the descriptor-heap override");
    check(rt::d3d12_xs::device_slot_overridden(62), "device slot 62 is the compute-PSO override");
    check(rt::d3d12_xs::device_slot_overridden(76), "device slot 76 is the resource override");
    check(rt::d3d12_xs::device_slot_overridden(86), "device slot 86 is the CreatePipelineState override");
    check(rt::d3d12_xs::device_slot_overridden(101), "device slot 101 is the CreateStateObject override");
    check(rt::d3d12_xs::device_slot_overridden(108), "device slot 108 is the GetResourceAllocationInfo2 override");
    check(rt::d3d12_xs::device_slot_overridden(67),
          "device slot 67 is the DeserializeComputePipelineStateX override (console native-code compute pipeline, not PC CreateProtectedResourceSession1)");

    void** proxy_vtbl = *reinterpret_cast<void***>(device.Get());
    void** real_vtbl = real ? *reinterpret_cast<void***>(real) : nullptr;
    check(proxy_vtbl != nullptr && proxy_vtbl[14] != real_vtbl[14], "slot 14 thunk differs from the PC entry");
    if (slots > 62) check(proxy_vtbl[62] != real_vtbl[62], "slot 62 thunk differs from PC CreateStateObject");
    if (slots > 67) check(proxy_vtbl[67] != real_vtbl[67], "slot 67 thunk differs from PC CreateProtectedResourceSession1");
    if (slots > 76) check(proxy_vtbl[76] != real_vtbl[76], "slot 76 thunk differs from PC CreateCommittedResource3");

    // The hit counters are for tests and count from the first read or reset, so that a title's calls write no
    // shared counter. The QueryInterface of the creation above was before that: its flag is set, its hit is not.
    check(rt::d3d12_xs::device_slot_first_call_seen(0), "slot 0 first-call flag is set by the creation");
    check(rt::d3d12_xs::device_slot_hits(0) == 0, "slot 0 has no hit before the first read of a counter");
    device->AddRef();
    device->Release();
    check(rt::d3d12_xs::device_slot_hits(1) == 1 && rt::d3d12_xs::device_slot_hits(2) == 1,
          "slots 1 and 2 count the calls after the first read");

    rt::d3d12_xs::reset_device_proxy_stats();

    // QI/refcount behaviour: device interfaces resolve to the same proxy, non-device IIDs do not.
    ID3D12Device* same = nullptr;
    hr = device->QueryInterface(__uuidof(ID3D12Device), reinterpret_cast<void**>(&same));
    check(SUCCEEDED(hr) && same == device.Get(), "QueryInterface(IID_ID3D12Device) returns the same proxy");
    if (same) same->Release();
    ComPtr<ID3D12Device1> device1;
    hr = device->QueryInterface(IID_PPV_ARGS(device1.GetAddressOf()));
    check(SUCCEEDED(hr) && device1 != nullptr, "QueryInterface(IID_ID3D12Device1) succeeds");
    ComPtr<ID3D12CommandQueue> not_a_device;
    hr = device->QueryInterface(IID_PPV_ARGS(not_a_device.GetAddressOf()));
    check(FAILED(hr) && !not_a_device, "QueryInterface rejects a non-device IID");
    check(rt::d3d12_xs::device_slot_hits(0) >= 3, "slot 0 hit counter moves");

    // Generic-forwarder coverage: slot 43 returns an 8-byte struct through the hidden pointer, and slot 30 has
    // two stack arguments. Both must reach the PC device unchanged (they are not measured overrides).
    const LUID proxy_luid = device->GetAdapterLuid();
    const LUID real_luid = real ? real->GetAdapterLuid() : LUID{};
    check(proxy_luid.LowPart == real_luid.LowPart && proxy_luid.HighPart == real_luid.HighPart,
          "slot 43 forwards GetAdapterLuid (struct return)");
    // Slot 25 returns a struct wider than 8 bytes. The console title calls it with the C ABI: the hidden
    // return pointer first (RCX), then the device (RDX), then the arguments, and the buffer comes back in RAX
    // (measured for slot 26 on the retail executable). Call slot 25 that way through a raw function pointer and compare
    // with the PC device's own answer.
    check(rt::d3d12_xs::device_slot_overridden(25), "device slot 25 is the C-ABI GetResourceAllocationInfo override");
    if (real) {
        using AllocInfoFn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT,
                                                                     const D3D12_RESOURCE_DESC*);
        auto get_alloc_info = reinterpret_cast<AllocInfoFn>(proxy_vtbl[25]);
        const D3D12_RESOURCE_DESC desc = buffer_desc();
        D3D12_RESOURCE_ALLOCATION_INFO got{~0ull, ~0ull};
        D3D12_RESOURCE_ALLOCATION_INFO* returned = get_alloc_info(&got, device.Get(), 0, 1, &desc);
        // The proxy cleans the console layout word and flag bits before the PC call (stop 16), so the expected answer is
        // the PC device's for the cleaned description (the raw 0x101 layout makes WARP answer UINT64_MAX).
        const D3D12_RESOURCE_DESC clean = rt::d3d12_xs::pc_resource_desc(desc);
        const D3D12_RESOURCE_ALLOCATION_INFO want = real->GetResourceAllocationInfo(0, 1, &clean);
        check(want.SizeInBytes != UINT64_MAX, "WARP accepts the normalized slot-25 buffer description");
        check(returned == &got, "slot 25 returns the caller's buffer in RAX");
        check(got.SizeInBytes == want.SizeInBytes && got.Alignment == want.Alignment,
              "slot 25 (C ABI) writes the PC device's GetResourceAllocationInfo into the caller's buffer");

        // The runtime's own code calls the proxy through d3d12.h (PC form: this, then the return buffer). Slot 25
        // still answers that form.
        const D3D12_RESOURCE_ALLOCATION_INFO pc_info = device->GetResourceAllocationInfo(0, 1, &desc);
        check(pc_info.SizeInBytes == want.SizeInBytes && pc_info.Alignment == want.Alignment,
              "slot 25 still answers the PC (C++) form");
    }

    D3D12_RESOURCE_DESC reserved_desc = buffer_desc();
    reserved_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> reserved;
    hr = device->CreateReservedResource(&reserved_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                        IID_PPV_ARGS(reserved.GetAddressOf()));
    check(SUCCEEDED(hr) && reserved != nullptr, "slot 30 forwards CreateReservedResource (stack arguments)");

    // Slot 26 the way the retail executable calls GetCustomHeapProperties: the buffer for the struct in RCX, the
    // device in RDX, for the three heap types it asks about. The answer is the PC device's own, and nothing but
    // the struct is written (the buffer sits in a pattern). On main this call read the PC vtable through the buffer.
    check(rt::d3d12_xs::device_slot_overridden(26), "device slot 26 takes the console's struct-return form");
    using XboxGetCustomHeapPropertiesFn =
        D3D12_HEAP_PROPERTIES*(WINAPI*)(D3D12_HEAP_PROPERTIES*, void*, UINT, D3D12_HEAP_TYPE);
    auto get_custom_heap = reinterpret_cast<XboxGetCustomHeapPropertiesFn>(proxy_vtbl[26]);
    const D3D12_HEAP_TYPE heap_types[] = {D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_TYPE_READBACK};
    bool console_returns_buffer = true, console_matches_pc = real != nullptr, console_writes_only_struct = true;
    for (const D3D12_HEAP_TYPE type : heap_types) {
        struct Padded {
            D3D12_HEAP_PROPERTIES value;
            std::uint8_t after[64];
        } padded;
        std::memset(&padded, 0xA5, sizeof(padded));
        console_returns_buffer = get_custom_heap(&padded.value, device.Get(), 1, type) == &padded.value &&
                                 console_returns_buffer;
        const D3D12_HEAP_PROPERTIES want = real ? real->GetCustomHeapProperties(1, type) : D3D12_HEAP_PROPERTIES{};
        console_matches_pc = console_matches_pc && std::memcmp(&padded.value, &want, sizeof(want)) == 0;
        for (const std::uint8_t byte : padded.after)
            console_writes_only_struct = console_writes_only_struct && byte == 0xA5;
    }
    check(console_returns_buffer, "slot 26 console form returns the buffer's address");
    check(console_matches_pc, "slot 26 console form writes the PC device's DEFAULT, UPLOAD and READBACK properties");
    check(console_writes_only_struct, "slot 26 console form writes nothing past the struct");
    check(rt::d3d12_xs::device_slot_hits(26) == 3, "slot 26 hit counter is 3");
    check(rt::d3d12_xs::is_device_proxy(device.Get()) && rt::d3d12_xs::device_proxy_real_device(device.Get()) == real,
          "the proxy is intact after the console-form calls");
    // A PC C++ call through the proxy (the proxy in RCX, the buffer in RDX) is still forwarded.
    const D3D12_HEAP_PROPERTIES pc_form = device->GetCustomHeapProperties(1, D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES pc_want = real ? real->GetCustomHeapProperties(1, D3D12_HEAP_TYPE_UPLOAD)
                                               : D3D12_HEAP_PROPERTIES{};
    check(real && std::memcmp(&pc_form, &pc_want, sizeof(pc_want)) == 0 && rt::d3d12_xs::device_slot_hits(26) == 4,
          "slot 26 PC form still forwards GetCustomHeapProperties");

    // Slot 14: descriptor heap through the proxy. The proxy wraps the real PC heap for the handle ABI.
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 8;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    const unsigned long refs_before_heap = rt::d3d12_xs::device_proxy_ref_count(device.Get());
    hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(heap.GetAddressOf()));
    check(SUCCEEDED(hr) && heap != nullptr, "slot 14 creates a descriptor heap");
    check(rt::d3d12_xs::device_slot_hits(14) == 1, "slot 14 hit counter is 1");
    check(heap && rt::d3d12_xs::device_proxy_ref_count(device.Get()) == refs_before_heap + 1,
          "heap holds one device proxy reference");

    if (heap) {
        check(rt::d3d12_xs::is_heap_proxy(heap.Get()), "slot 14 returns a heap proxy");
        ID3D12DescriptorHeap* real_heap = rt::d3d12_xs::heap_proxy_real_heap(heap.Get());
        check(real_heap != nullptr, "heap proxy exposes the PC heap");
        check(rt::d3d12_xs::heap_slot_overridden(9), "heap slot 9 is the CPU-handle override");
        check(rt::d3d12_xs::heap_slot_overridden(10), "heap slot 10 is the GPU-handle override");
        ID3D12DescriptorHeap* same_heap = nullptr;
        hr = heap->QueryInterface(__uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&same_heap));
        check(SUCCEEDED(hr) && same_heap == heap.Get(), "heap QueryInterface returns the same heap proxy");
        if (same_heap) same_heap->Release();

        // ID3D12DeviceChild::GetDevice must return the Xbox-ABI device, not the raw PC device, and AddRef it.
        check(rt::d3d12_xs::heap_slot_overridden(7), "heap slot 7 is the GetDevice override");
        ID3D12Device* heap_device = nullptr;
        const unsigned long refs_before_get = rt::d3d12_xs::device_proxy_ref_count(device.Get());
        hr = heap->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&heap_device));
        check(SUCCEEDED(hr) && heap_device == device.Get() && rt::d3d12_xs::is_device_proxy(heap_device),
              "heap GetDevice returns the device proxy");
        check(rt::d3d12_xs::device_proxy_ref_count(device.Get()) == refs_before_get + 1,
              "GetDevice AddRefs the device proxy");
        check(rt::d3d12_xs::heap_slot_hits(7) == 1, "heap slot 7 hit counter is 1");
        if (heap_device) heap_device->Release();
        check(rt::d3d12_xs::device_proxy_ref_count(device.Get()) == refs_before_get,
              "GetDevice release balances");

        // The Xbox call shape: `this` only, 8-byte handle in RAX. The proxy converts from the PC heap's
        // hidden return pointer (RDX). A PC C++ call through the proxy uses the PC convention, so it is not
        // used here; the real heap below shows that convention and the conversion is compared against it.
        void** proxy_heap_vtbl = *reinterpret_cast<void***>(heap.Get());
        auto get_cpu = reinterpret_cast<GetCpuHandleFn>(proxy_heap_vtbl[9]);
        auto get_gpu = reinterpret_cast<GetGpuHandleFn>(proxy_heap_vtbl[10]);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = get_cpu(heap.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = get_gpu(heap.Get());
        check(cpu.ptr != 0, "heap proxy slot 9 returns a CPU handle in RAX");
        check(gpu.ptr != 0, "heap proxy slot 10 returns a GPU handle in RAX");
        if (real_heap) {
            void** real_heap_vtbl = *reinterpret_cast<void***>(real_heap);
            check(proxy_heap_vtbl[9] != real_heap_vtbl[9], "heap slot 9 thunk differs from the PC heap entry");
            check(proxy_heap_vtbl[10] != real_heap_vtbl[10], "heap slot 10 thunk differs from the PC heap entry");
        }

        // PC reality check: the real heap takes a hidden return pointer in RDX. This is the deviation that
        // makes the wrapper necessary; a one-argument call on the real heap is invalid and is not made.
        if (real_heap) {
            void** real_heap_vtbl = *reinterpret_cast<void***>(real_heap);
            using GetCpuOutFn = void(WINAPI*)(ID3D12DescriptorHeap*, D3D12_CPU_DESCRIPTOR_HANDLE*);
            using GetGpuOutFn = void(WINAPI*)(ID3D12DescriptorHeap*, D3D12_GPU_DESCRIPTOR_HANDLE*);
            constexpr SIZE_T kSentinel = 0x1111111111111111ull;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu_pc{};
            D3D12_GPU_DESCRIPTOR_HANDLE gpu_pc{};
            cpu_pc.ptr = kSentinel;
            gpu_pc.ptr = kSentinel;
            reinterpret_cast<GetCpuOutFn>(real_heap_vtbl[9])(real_heap, &cpu_pc);
            reinterpret_cast<GetGpuOutFn>(real_heap_vtbl[10])(real_heap, &gpu_pc);
            check(cpu_pc.ptr != kSentinel, "PC heap slot 9 writes through the RDX return pointer");
            check(gpu_pc.ptr != kSentinel, "PC heap slot 10 writes through the RDX return pointer");
            check(cpu_pc.ptr == cpu.ptr, "proxy CPU handle equals the PC heap's hidden-pointer result");
            check(gpu_pc.ptr == gpu.ptr, "proxy GPU handle equals the PC heap's hidden-pointer result");
        }

        check(rt::d3d12_xs::heap_slot_overridden(8), "heap slot 8 is the console-ABI GetDesc override");
        {
            using ConsoleHeapGetDescFn = D3D12_DESCRIPTOR_HEAP_DESC*(WINAPI*)(D3D12_DESCRIPTOR_HEAP_DESC*, void*);
            auto console_get_desc = reinterpret_cast<ConsoleHeapGetDescFn>(proxy_heap_vtbl[8]);
            struct PaddedHeapDesc {
                D3D12_DESCRIPTOR_HEAP_DESC value;
                std::uint8_t after[64];
            } padded;
            std::memset(&padded, 0xA5, sizeof(padded));
            const std::uint64_t hits_before = rt::d3d12_xs::heap_slot_hits(8);
            const D3D12_DESCRIPTOR_HEAP_DESC* returned = console_get_desc(&padded.value, heap.Get());
            const D3D12_DESCRIPTOR_HEAP_DESC want = real_heap ? real_heap->GetDesc() : D3D12_DESCRIPTOR_HEAP_DESC{};
            bool canary_intact = true;
            for (const std::uint8_t byte : padded.after) canary_intact = canary_intact && byte == 0xA5;
            check(returned == &padded.value, "heap slot 8 console form returns the buffer address");
            check(real_heap && std::memcmp(&padded.value, &want, sizeof(want)) == 0 &&
                      padded.value.NumDescriptors == heap_desc.NumDescriptors,
                  "heap slot 8 console form matches real heap GetDesc");
            check(canary_intact, "heap slot 8 console form writes nothing past the struct");
            check(rt::d3d12_xs::heap_slot_hits(8) == hits_before + 1, "heap slot 8 console form increments hit counter");
            check(rt::d3d12_xs::is_heap_proxy(heap.Get()) && rt::d3d12_xs::heap_proxy_real_heap(heap.Get()) == real_heap,
                  "heap proxy stays intact after console-form GetDesc");
        }
        const std::uint64_t heap_desc_hits_before_pc = rt::d3d12_xs::heap_slot_hits(8);
        const D3D12_DESCRIPTOR_HEAP_DESC got_desc = heap->GetDesc();
        check(got_desc.NumDescriptors == heap_desc.NumDescriptors &&
                  rt::d3d12_xs::heap_slot_hits(8) == heap_desc_hits_before_pc + 1,
              "heap slot 8 PC form still forwards GetDesc");
        check(rt::d3d12_xs::heap_slot_hits(9) >= 1, "heap slot 9 hit counter moves");
        check(rt::d3d12_xs::heap_slot_hits(10) >= 1, "heap slot 10 hit counter moves");
        check(rt::d3d12_xs::heap_slot_first_call_seen(9), "heap slot 9 first-call flag is set");
    }

    // Slot 9 on the device must remain CreateCommandAllocator, not a handle getter.
    ComPtr<ID3D12CommandAllocator> allocator;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()));
    check(SUCCEEDED(hr) && allocator != nullptr, "device slot 9 forwards CreateCommandAllocator");
    // A PC runtime allocator is not wrapped: the PC runtime's lists could not resolve an allocator proxy.
    check(!rt::d3d12_xs::is_allocator_proxy(allocator.Get()), "slot 9 hands out the PC allocator unwrapped");
    check(rt::d3d12_xs::device_slot_hits(9) == 1, "slot 9 hit counter is 1");

    // Slot 76: buffer resource with the measured Xbox layout word 0x101; PC committed resources take ROW_MAJOR.
    const IID iid_resource = __uuidof(ID3D12Resource);
    auto create_resource = reinterpret_cast<XboxCreateResourceFn>(proxy_vtbl[76]);
    const D3D12_HEAP_PROPERTIES heap_properties = default_heap();
    D3D12_RESOURCE_DESC xbox_buffer = buffer_desc();
    ComPtr<ID3D12Resource> buffer;
    hr = create_resource(device.Get(), &heap_properties, D3D12_HEAP_FLAG_NONE, &xbox_buffer,
                         D3D12_RESOURCE_STATE_COMMON, nullptr, iid_resource,
                         reinterpret_cast<void**>(buffer.GetAddressOf()));
    check(SUCCEEDED(hr) && buffer != nullptr, "slot 76 creates a committed buffer");
    if (buffer) {
        const D3D12_RESOURCE_DESC got = buffer->GetDesc();
        check(got.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR, "slot 76 maps 0x101 to ROW_MAJOR for a buffer");
        check(got.Width == 4096, "slot 76 preserves the resource width");
    }

    // The same Xbox word on a texture maps to UNKNOWN, which is what PC committed textures require.
    D3D12_RESOURCE_DESC xbox_texture = texture_desc();
    ComPtr<ID3D12Resource> texture;
    hr = create_resource(device.Get(), &heap_properties, D3D12_HEAP_FLAG_NONE, &xbox_texture,
                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, iid_resource,
                         reinterpret_cast<void**>(texture.GetAddressOf()));
    check(SUCCEEDED(hr) && texture != nullptr, "slot 76 creates a committed texture");
    if (texture) {
        check(texture->GetDesc().Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN,
              "slot 76 maps 0x101 to UNKNOWN for a texture");
    }
    check(rt::d3d12_xs::device_slot_hits(76) == 2, "slot 76 hit counter is 2");
    check(rt::d3d12_xs::device_slot_first_call_seen(76), "slot 76 first-call flag is set");

    // Xbox device slots from the public HLE. On the Vulkan device the PC entries at 78/79 are
    // CreateReservedResource2/CreateSampler2 and 80 is past the 80-slot table, so success here can only come from
    // the routes: frame events return S_OK and a fresh token, CreateCommandQueueX returns a queue proxy and
    // CreateCommandListX (desc, allocator, initialState, riid, out; no node mask) reaches PC CreateCommandList.
    {
        using SetFrameIntervalFn = HRESULT(WINAPI*)(void*, void*, UINT, UINT, UINT);
        using ScheduleFrameEventFn = HRESULT(WINAPI*)(void*, UINT, UINT, void*, UINT);
        using WaitFrameEventFn = HRESULT(WINAPI*)(void*, UINT, UINT, void*, UINT, UINT64*);
        using CreateQueueXFn = HRESULT(WINAPI*)(void*, const D3D12_COMMAND_QUEUE_DESC*, const IID*, void**);
        using CreateListXFn = HRESULT(WINAPI*)(void*, const void*, ID3D12CommandAllocator*, ID3D12PipelineState*,
                                               const IID*, void**);

        hr = reinterpret_cast<SetFrameIntervalFn>(proxy_vtbl[78])(device.Get(), nullptr, 16667, 1, 0);
        check(hr == S_OK, "slot 78 SetFrameIntervalX returns S_OK");
        hr = reinterpret_cast<ScheduleFrameEventFn>(proxy_vtbl[79])(device.Get(), 0, 0, nullptr, 0);
        check(hr == S_OK, "slot 79 ScheduleFrameEventX returns S_OK");

        std::uint64_t token = 0, token2 = 0;
        hr = reinterpret_cast<WaitFrameEventFn>(proxy_vtbl[80])(device.Get(), 0, INFINITE, nullptr, 0, &token);
        check(hr == S_OK && token != 0, "slot 80 WaitFrameEventX returns a non-zero token");
        hr = reinterpret_cast<WaitFrameEventFn>(proxy_vtbl[80])(device.Get(), 0, 0, nullptr, 0, &token2);
        check(hr == S_OK && token2 != 0 && token2 != token, "slot 80 returns a fresh token on the next wait");
        hr = reinterpret_cast<WaitFrameEventFn>(proxy_vtbl[80])(device.Get(), 0, 0, nullptr, 0, nullptr);
        check(hr == E_POINTER, "slot 80 refuses a null token with E_POINTER");
        check(rt::d3d12_xs::device_slot_hits(78) == 1 && rt::d3d12_xs::device_slot_hits(79) == 1 &&
                  rt::d3d12_xs::device_slot_hits(80) == 3,
              "slots 78/79/80 hit counters are 1/1/3");

        // The console description is 32 bytes: the PC prefix, then four Xbox words (the proxy logs them).
        struct { D3D12_COMMAND_QUEUE_DESC pc; std::uint32_t extra[4]; } xbox_queue{};
        xbox_queue.pc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        const IID iid_queue = __uuidof(ID3D12CommandQueue);
        ComPtr<ID3D12CommandQueue> xbox_queue_proxy;
        hr = reinterpret_cast<CreateQueueXFn>(proxy_vtbl[48])(device.Get(), reinterpret_cast<const D3D12_COMMAND_QUEUE_DESC*>(&xbox_queue), &iid_queue,
                                                               reinterpret_cast<void**>(xbox_queue_proxy.GetAddressOf()));
        check(hr == S_OK && rt::d3d12_xs::is_queue_proxy(xbox_queue_proxy.Get()),
              "slot 48 CreateCommandQueueX returns a queue proxy");
        check(rt::d3d12_xs::device_slot_hits(48) == 1, "slot 48 hit counter is 1");

        ComPtr<ID3D12CommandAllocator> xbox_allocator;
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(xbox_allocator.GetAddressOf()));
        struct XboxCommandListDesc {
            UINT Type;
            UINT NodeMask;
        } list_desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 7};  // the node mask is ignored by the console call
        const IID iid_list = __uuidof(ID3D12GraphicsCommandList);
        ComPtr<ID3D12GraphicsCommandList> xbox_list;
        hr = SUCCEEDED(hr) ? reinterpret_cast<CreateListXFn>(proxy_vtbl[73])(
                                 device.Get(), &list_desc, xbox_allocator.Get(), nullptr, &iid_list,
                                 reinterpret_cast<void**>(xbox_list.GetAddressOf()))
                           : hr;
        check(hr == S_OK && xbox_list != nullptr, "slot 73 CreateCommandListX returns a command list");
        check(rt::d3d12_xs::device_slot_hits(73) == 1, "slot 73 hit counter is 1");
    }

    // Slot 62: an empty root signature and a trivial compute shader through the Xbox signature. The PC entry at
    // slot 62 is CreateStateObject, so success here proves the override reached CreateComputePipelineState.
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> root_blob;
    ComPtr<ID3DBlob> root_error;
    hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, root_blob.GetAddressOf(),
                                     root_error.GetAddressOf());
    check(SUCCEEDED(hr) && root_blob != nullptr, "empty root signature serializes");
    ComPtr<ID3D12RootSignature> root_signature;
    if (root_blob) {
        hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(root_signature.GetAddressOf()));
    }
    check(SUCCEEDED(hr) && root_signature != nullptr, "device slot 16 forwards CreateRootSignature");

    ComPtr<ID3DBlob> bytecode;
    if (load_compute_shader(bytecode) && root_signature) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root_signature.Get();
        pso_desc.CS.pShaderBytecode = bytecode->GetBufferPointer();
        pso_desc.CS.BytecodeLength = bytecode->GetBufferSize();
        pso_desc.NodeMask = 0;
        pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

        auto create_pso = reinterpret_cast<XboxCreateComputePsoFn>(proxy_vtbl[62]);
        const IID iid_pso = __uuidof(ID3D12PipelineState);
        ComPtr<ID3D12PipelineState> pso;
        hr = create_pso(device.Get(), &pso_desc, 0, 0, iid_pso, reinterpret_cast<void**>(pso.GetAddressOf()));
        check(SUCCEEDED(hr) && pso != nullptr, "slot 62 creates a compute PSO via CreateComputePipelineState");
        check(rt::d3d12_xs::device_slot_hits(62) == 1, "slot 62 hit counter is 1");
        check(rt::d3d12_xs::device_slot_first_call_seen(62), "slot 62 first-call flag is set");

        // Slot 67: Xbox native compute pipeline creation. Maps the console descriptor (+0x00 root signature,
        // +0x28 kind 6 code blob, +0x30 code size) to D3D12_COMPUTE_PIPELINE_STATE_DESC and calls CreateComputePipelineState.
        struct XboxNativeComputeDesc {
            ID3D12RootSignature* root_signature;
            const void* kind0;
            std::uint64_t kind0_size;
            const void* kind1;
            std::uint64_t kind1_size;
            const void* kind6;
            std::uint64_t kind6_size;
            const void* kind7;
            std::uint64_t kind7_size;
        } xbox_desc{};
        xbox_desc.root_signature = root_signature.Get();
        xbox_desc.kind6 = bytecode->GetBufferPointer();
        xbox_desc.kind6_size = bytecode->GetBufferSize();

        using XboxCreateNativeComputePsoFn = HRESULT(WINAPI*)(void*, const void*, const IID*, void**);
        auto create_native_pso = reinterpret_cast<XboxCreateNativeComputePsoFn>(proxy_vtbl[67]);
        ComPtr<ID3D12PipelineState> pso67;
        hr = create_native_pso(device.Get(), &xbox_desc, &iid_pso, reinterpret_cast<void**>(pso67.GetAddressOf()));
        check(SUCCEEDED(hr) && pso67 != nullptr, "slot 67 creates a compute PSO via CreateComputePipelineState");
        check(rt::d3d12_xs::device_slot_hits(67) == 1, "slot 67 hit counter is 1");
        check(rt::d3d12_xs::device_slot_first_call_seen(67), "slot 67 first-call flag is set");

        // Slot 86: CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**), forwarded to PC
        // ID3D12Device2::CreatePipelineState (PC slot 47).
        if (slots > 47) {
            struct alignas(void*) ComputeStream {
                alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE root_type =
                    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
                ID3D12RootSignature* root_sig = nullptr;
                alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE cs_type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
                D3D12_SHADER_BYTECODE cs{};
            } stream;
            stream.root_sig = root_signature.Get();
            stream.cs.pShaderBytecode = bytecode->GetBufferPointer();
            stream.cs.BytecodeLength = bytecode->GetBufferSize();
            D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
            using XboxCreatePipelineStateFn =
                HRESULT(WINAPI*)(void*, const D3D12_PIPELINE_STATE_STREAM_DESC*, const IID*, void**);
            auto create_stream_pso = reinterpret_cast<XboxCreatePipelineStateFn>(proxy_vtbl[86]);
            ComPtr<ID3D12PipelineState> pso86;
            hr = create_stream_pso(device.Get(), &stream_desc, &iid_pso,
                                   reinterpret_cast<void**>(pso86.GetAddressOf()));
            check(SUCCEEDED(hr) && pso86 != nullptr,
                  "slot 86 creates a PSO via PC ID3D12Device2::CreatePipelineState (slot 47)");
            check(rt::d3d12_xs::device_slot_hits(86) == 1, "slot 86 hit counter is 1");
            check(rt::d3d12_xs::device_slot_first_call_seen(86), "slot 86 first-call flag is set");
            check_derived_states(device.Get(), proxy_vtbl, root_signature.Get(), pso86.Get(), pso67.Get());
        }
    }

    // Slot 101: CreateStateObject(const D3D12_STATE_OBJECT_DESC*, REFIID, void**), forwarded to PC
    // ID3D12Device5::CreateStateObject (PC slot 62).
    if (slots > 62) {
        using XboxCreateStateObjectFn = HRESULT(WINAPI*)(void*, const D3D12_STATE_OBJECT_DESC*, const IID*, void**);
        auto create_state_object = reinterpret_cast<XboxCreateStateObjectFn>(proxy_vtbl[101]);
        D3D12_STATE_OBJECT_DESC empty_so{};
        empty_so.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        void* so_out = reinterpret_cast<void*>(0x1111);
        hr = create_state_object(device.Get(), &empty_so, nullptr, &so_out);
        check(hr == E_POINTER && so_out == nullptr, "slot 101 nulls its output for a null IID");
        check(rt::d3d12_xs::device_slot_hits(101) == 1, "slot 101 hit counter is 1");
    }

    // Slot 108: GetResourceAllocationInfo2(ret, this, visible_mask, count, descs, info1) with C-ABI struct return,
    // 56-byte D3D12_RESOURCE_DESC entries, and Xbox layout/flag words, compared against direct PC
    // ID3D12Device8::GetResourceAllocationInfo2 (PC slot 68).
    if (slots > 68) {
        ComPtr<ID3D12Device8> real_device8;
        if (real && SUCCEEDED(real->QueryInterface(IID_PPV_ARGS(real_device8.GetAddressOf()))) && real_device8) {
            D3D12_RESOURCE_DESC xbox_descs[2]{};
            xbox_descs[0].Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            xbox_descs[0].Width = 64;
            xbox_descs[0].Height = 64;
            xbox_descs[0].DepthOrArraySize = 1;
            xbox_descs[0].MipLevels = 1;
            xbox_descs[0].Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            xbox_descs[0].SampleDesc.Count = 1;
            xbox_descs[0].Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x105);
            xbox_descs[0].Flags = static_cast<D3D12_RESOURCE_FLAGS>(
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | 0x300000u);
            xbox_descs[1].Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            xbox_descs[1].Width = 4096;
            xbox_descs[1].Height = 1;
            xbox_descs[1].DepthOrArraySize = 1;
            xbox_descs[1].MipLevels = 1;
            xbox_descs[1].SampleDesc.Count = 1;
            xbox_descs[1].Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
            xbox_descs[1].Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x200000u);

            D3D12_RESOURCE_DESC1 pc_descs[2]{};
            std::memcpy(&pc_descs[0], &xbox_descs[0], sizeof(D3D12_RESOURCE_DESC));
            pc_descs[0].SamplerFeedbackMipRegion = {};
            pc_descs[0].Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            pc_descs[0].Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            std::memcpy(&pc_descs[1], &xbox_descs[1], sizeof(D3D12_RESOURCE_DESC));
            pc_descs[1].SamplerFeedbackMipRegion = {};
            pc_descs[1].Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            pc_descs[1].Flags = D3D12_RESOURCE_FLAG_NONE;

            D3D12_RESOURCE_ALLOCATION_INFO1 pc_info1[2]{};
            const D3D12_RESOURCE_ALLOCATION_INFO pc_info =
                real_device8->GetResourceAllocationInfo2(1, 2, pc_descs, pc_info1);

            using XboxGetAllocInfo2CAbiFn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(
                D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT, const D3D12_RESOURCE_DESC*,
                D3D12_RESOURCE_ALLOCATION_INFO1*);
            auto get_alloc_info2 = reinterpret_cast<XboxGetAllocInfo2CAbiFn>(proxy_vtbl[108]);
            D3D12_RESOURCE_ALLOCATION_INFO xbox_info{};
            D3D12_RESOURCE_ALLOCATION_INFO1 xbox_info1[2]{};
            D3D12_RESOURCE_ALLOCATION_INFO* ret_ptr =
                get_alloc_info2(&xbox_info, device.Get(), 1, 2, xbox_descs, xbox_info1);
            check(ret_ptr == &xbox_info && xbox_info.SizeInBytes == pc_info.SizeInBytes &&
                      xbox_info.Alignment == pc_info.Alignment && xbox_info1[0].SizeInBytes == pc_info1[0].SizeInBytes &&
                      xbox_info1[1].SizeInBytes == pc_info1[1].SizeInBytes,
                  "slot 108 C-ABI GetResourceAllocationInfo2 with 56-byte Xbox descs matches PC Device8");
            check(rt::d3d12_xs::device_slot_hits(108) == 1, "slot 108 hit counter is 1");
        }
    }

    // _COM_Outptr_ (d3d12.h): a failed create leaves NULL in the output. WARP accepts invalid heap descriptions,
    // so the PC-failure case for slot 14 is in d3d12_vulkan_device_test; these are the proxy's own early returns.
    {
        void* proxy_out = reinterpret_cast<void*>(0x1111);
        using Slot14Fn = HRESULT(WINAPI*)(void*, const D3D12_DESCRIPTOR_HEAP_DESC*, const IID*, void**);
        hr = reinterpret_cast<Slot14Fn>(proxy_vtbl[14])(device.Get(), &heap_desc, nullptr, &proxy_out);
        check(FAILED(hr) && proxy_out == nullptr, "slot 14 nulls its output for a null IID");
        using Slot62Fn = HRESULT(WINAPI*)(void*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, std::uint64_t,
                                          std::uint64_t, const IID*, void**);
        proxy_out = reinterpret_cast<void*>(0x1111);
        D3D12_COMPUTE_PIPELINE_STATE_DESC empty_pso{};
        hr = reinterpret_cast<Slot62Fn>(proxy_vtbl[62])(device.Get(), &empty_pso, 0, 0, nullptr, &proxy_out);
        check(FAILED(hr) && proxy_out == nullptr, "slot 62 nulls its output for a null IID");
        using Slot67Fn = HRESULT(WINAPI*)(void*, const void*, const IID*, void**);
        proxy_out = reinterpret_cast<void*>(0x1111);
        const IID iid_pso = __uuidof(ID3D12PipelineState);
        hr = reinterpret_cast<Slot67Fn>(proxy_vtbl[67])(device.Get(), nullptr, &iid_pso, &proxy_out);
        check(FAILED(hr) && proxy_out == nullptr, "slot 67 nulls its output for a null description");
        proxy_out = reinterpret_cast<void*>(0x1111);
        hr = reinterpret_cast<Slot67Fn>(proxy_vtbl[67])(device.Get(), &root_desc, nullptr, &proxy_out);
        check(FAILED(hr) && proxy_out == nullptr, "slot 67 nulls its output for a null IID");
        using Slot76Fn = HRESULT(WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                          const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                          const D3D12_CLEAR_VALUE*, const IID*, void**);
        proxy_out = reinterpret_cast<void*>(0x1111);
        hr = reinterpret_cast<Slot76Fn>(proxy_vtbl[76])(device.Get(), &heap_properties, D3D12_HEAP_FLAG_NONE,
                                                         nullptr, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         &iid_resource, &proxy_out);
        check(FAILED(hr) && proxy_out == nullptr, "slot 76 nulls its output for a null description");
    }

    // Slots 8 and 36 on the device the shim made: a title gets queue and fence proxies, and they run GPU work.
    {
        using CreateQueueFn = HRESULT(WINAPI*)(void*, const D3D12_COMMAND_QUEUE_DESC*, const IID*, void**);
        using CreateFenceFn = HRESULT(WINAPI*)(void*, UINT64, D3D12_FENCE_FLAGS, const IID*, void**);
        void** vtable = *reinterpret_cast<void***>(device.Get());
        const D3D12_COMMAND_QUEUE_DESC queue_desc{};
        const IID iid_queue = __uuidof(ID3D12CommandQueue);
        const IID iid_fence = __uuidof(ID3D12Fence);
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Fence> fence;
        hr = reinterpret_cast<CreateQueueFn>(vtable[8])(device.Get(), &queue_desc, &iid_queue,
                                                        reinterpret_cast<void**>(queue.GetAddressOf()));
        check(SUCCEEDED(hr) && rt::d3d12_xs::is_queue_proxy(queue.Get()), "slot 8 returns a queue proxy");
        hr = reinterpret_cast<CreateFenceFn>(vtable[36])(device.Get(), 0, D3D12_FENCE_FLAG_NONE, &iid_fence,
                                                         reinterpret_cast<void**>(fence.GetAddressOf()));
        check(SUCCEEDED(hr) && rt::d3d12_xs::is_fence_proxy(fence.Get()), "slot 36 returns a fence proxy");
        if (queue && fence) {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            check(event && SUCCEEDED(queue->Signal(fence.Get(), 1)) &&
                      SUCCEEDED(fence->SetEventOnCompletion(1, event)) && WaitForSingleObject(event, 5000) == 0,
                  "the queue proxy signals the fence proxy on the GPU");
            if (event) CloseHandle(event);
            ComPtr<ID3D12Device> owner;
            check(SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(owner.GetAddressOf()))) && owner.Get() == device.Get(),
                  "the queue proxy's GetDevice is the device the shim returned");
            ID3D12CommandQueue* real_queue = rt::d3d12_xs::queue_proxy_real_queue(queue.Get());
            void** queue_vtbl = *reinterpret_cast<void***>(queue.Get());
            using ConsoleQueueGetDescFn = D3D12_COMMAND_QUEUE_DESC*(WINAPI*)(D3D12_COMMAND_QUEUE_DESC*, void*);
            struct PaddedQueueDesc {
                D3D12_COMMAND_QUEUE_DESC value;
                std::uint8_t after[64];
            } padded;
            std::memset(&padded, 0xA5, sizeof(padded));
            const std::uint64_t qhits_before = rt::d3d12_xs::queue_slot_hits(18);
            const D3D12_COMMAND_QUEUE_DESC* returned =
                reinterpret_cast<ConsoleQueueGetDescFn>(queue_vtbl[18])(&padded.value, queue.Get());
            const D3D12_COMMAND_QUEUE_DESC want = real_queue ? real_queue->GetDesc() : D3D12_COMMAND_QUEUE_DESC{};
            bool canary_intact = true;
            for (const std::uint8_t byte : padded.after) canary_intact = canary_intact && byte == 0xA5;
            check(returned == &padded.value && real_queue && std::memcmp(&padded.value, &want, sizeof(want)) == 0 &&
                      canary_intact && rt::d3d12_xs::queue_slot_hits(18) == qhits_before + 1 &&
                      rt::d3d12_xs::is_queue_proxy(queue.Get()) &&
                      rt::d3d12_xs::queue_proxy_real_queue(queue.Get()) == real_queue,
                  "queue slot 18 console form matches real queue GetDesc");
            const D3D12_COMMAND_QUEUE_DESC pc_queue_desc = queue->GetDesc();
            check(real_queue && std::memcmp(&pc_queue_desc, &want, sizeof(want)) == 0 &&
                      rt::d3d12_xs::queue_slot_hits(18) == qhits_before + 2,
                  "queue slot 18 PC form still forwards GetDesc");
        }
    }

    // Title-exported __XMemAllocationHooks__ backing for CreateDescriptorHeap (slot 14) + CPU handle translation
    // against the real WARP D3D12 runtime.
    {
        struct HookState {
            alignas(64) std::uint8_t backing[4096]{};
            std::size_t alloc_size = 0;
            std::uint64_t alloc_attr = 0;
            void* freed_ptr = nullptr;
            std::uint64_t freed_attr = 0;
            int alloc_count = 0;
            int free_count = 0;
        };
        static HookState hook_state;
        hook_state = {};
        struct HookCallbacks {
            static void* __stdcall alloc(std::size_t size, std::uint64_t attr) {
                ++hook_state.alloc_count;
                hook_state.alloc_size = size;
                hook_state.alloc_attr = attr;
                if ((attr & 0x3FFFull) == 4 && size <= sizeof(hook_state.backing)) return hook_state.backing;
                return nullptr;
            }
            static void __stdcall free(void* ptr, std::uint64_t attr) {
                ++hook_state.free_count;
                hook_state.freed_ptr = ptr;
                hook_state.freed_attr = attr;
            }
        };
        const rt::shims::XMemAllocationHooks title_hooks{&HookCallbacks::alloc, &HookCallbacks::free};
        rt::shims::add_xmem_allocation_hooks(&title_hooks, &title_hooks);

        D3D12_DESCRIPTOR_HEAP_DESC hooked_desc{};
        hooked_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hooked_desc.NumDescriptors = 4;
        hooked_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const UINT inc = device->GetDescriptorHandleIncrementSize(hooked_desc.Type);
        ComPtr<ID3D12DescriptorHeap> hooked_heap;
        hr = device->CreateDescriptorHeap(&hooked_desc, IID_PPV_ARGS(hooked_heap.GetAddressOf()));
        check(SUCCEEDED(hr) && hooked_heap != nullptr, "hooked slot 14 creates a descriptor heap");
        check(hook_state.alloc_count == 1 && hook_state.alloc_size == hooked_desc.NumDescriptors * inc &&
                  hook_state.alloc_attr == rt::d3d12_xs::kXmemDescriptorHeapAttributes,
              "slot 14 invokes __XMemAllocationHooks__ with heap byte size and D3D descriptor-heap attributes");
        if (hooked_heap) {
            void** hooked_vtbl = *reinterpret_cast<void***>(hooked_heap.Get());
            const auto hooked_cpu = reinterpret_cast<GetCpuHandleFn>(hooked_vtbl[9])(hooked_heap.Get());
            const auto hooked_gpu = reinterpret_cast<GetGpuHandleFn>(hooked_vtbl[10])(hooked_heap.Get());
            const auto expected_base = reinterpret_cast<SIZE_T>(hook_state.backing);
            check(hooked_cpu.ptr == expected_base,
                  "hooked heap slot 9 returns the title-supplied CPU descriptor base");
            check(hooked_gpu.ptr != 0, "hooked heap slot 10 returns a valid GPU descriptor start");

            // Create a CBV at slot 1 of the title-backed heap; WARP validates the translated real handle.
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
            cbv.BufferLocation = 0;
            cbv.SizeInBytes = 256;
            device->CreateConstantBufferView(&cbv, D3D12_CPU_DESCRIPTOR_HANDLE{expected_base + inc});
            check(true, "CreateConstantBufferView translates a title-backed CPU descriptor handle on WARP");
        }
        hooked_heap.Reset();
        check(hook_state.free_count == 1 && hook_state.freed_ptr == hook_state.backing &&
                  hook_state.freed_attr == rt::d3d12_xs::kXmemDescriptorHeapAttributes,
              "releasing the hooked heap frees its backing through __XMemAllocationHooks__");

        // End-to-end console command list clear + readback with title-backed RTV and DSV heaps:
        // verifies CPU handle translation on console slots 50/51/52 and FLOAT Depth (XMM3) on slot 51.
        {
            struct DualHeapState {
                alignas(64) std::uint8_t rtv_backing[1024]{};
                alignas(64) std::uint8_t dsv_backing[1024]{};
                int next_slice = 0;
            };
            static DualHeapState dual_state;
            dual_state = {};
            struct DualHooks {
                static void* __stdcall alloc(std::size_t size, std::uint64_t attr) {
                    if ((attr & 0x3FFFull) != 4 || size > 1024) return nullptr;
                    return dual_state.next_slice++ == 0 ? dual_state.rtv_backing : dual_state.dsv_backing;
                }
                static void __stdcall free(void*, std::uint64_t) {}
            };
            const rt::shims::XMemAllocationHooks dual_hooks{&DualHooks::alloc, &DualHooks::free};
            rt::shims::remove_xmem_allocation_hooks(&title_hooks);
            rt::shims::add_xmem_allocation_hooks(&dual_hooks, &dual_hooks);

            D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            D3D12_DESCRIPTOR_HEAP_DESC dsv_hd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
            hr = device->CreateDescriptorHeap(&rtv_hd, IID_PPV_ARGS(rtv_heap.GetAddressOf()));
            HRESULT hr_dsv = device->CreateDescriptorHeap(&dsv_hd, IID_PPV_ARGS(dsv_heap.GetAddressOf()));
            check(SUCCEEDED(hr) && SUCCEEDED(hr_dsv) && rtv_heap && dsv_heap,
                  "hooked RTV and DSV heaps create through slot 14");

            const D3D12_HEAP_PROPERTIES default_hp = default_heap();
            D3D12_HEAP_PROPERTIES readback_hp = default_heap();
            readback_hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rt_desc{};
            rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rt_desc.Width = 4;
            rt_desc.Height = 4;
            rt_desc.DepthOrArraySize = 1;
            rt_desc.MipLevels = 1;
            rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rt_desc.SampleDesc = {1, 0};
            rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_RESOURCE_DESC ds_desc = rt_desc;
            ds_desc.Format = DXGI_FORMAT_D32_FLOAT;
            ds_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            D3D12_RESOURCE_DESC rb_desc = buffer_desc();
            rb_desc.Width = 4096;
            rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            ComPtr<ID3D12Resource> rt_tex, ds_tex, rb_buf;
            hr = device->CreateCommittedResource(&default_hp, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                 IID_PPV_ARGS(rt_tex.GetAddressOf()));
            HRESULT hr2 = device->CreateCommittedResource(&default_hp, D3D12_HEAP_FLAG_NONE, &ds_desc,
                                                          D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
                                                          IID_PPV_ARGS(ds_tex.GetAddressOf()));
            HRESULT hr3 = device->CreateCommittedResource(&readback_hp, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                          IID_PPV_ARGS(rb_buf.GetAddressOf()));
            check(SUCCEEDED(hr) && SUCCEEDED(hr2) && SUCCEEDED(hr3),
                  "RT, DS and readback resources create for console list clear test");

            if (rtv_heap && dsv_heap && rt_tex && ds_tex && rb_buf) {
                const auto rtv_cpu = reinterpret_cast<GetCpuHandleFn>(
                    (*reinterpret_cast<void***>(rtv_heap.Get()))[9])(rtv_heap.Get());
                const auto dsv_cpu = reinterpret_cast<GetCpuHandleFn>(
                    (*reinterpret_cast<void***>(dsv_heap.Get()))[9])(dsv_heap.Get());
                const UINT rtv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                const UINT dsv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
                const D3D12_CPU_DESCRIPTOR_HANDLE rtv_h{rtv_cpu.ptr + rtv_inc};
                const D3D12_CPU_DESCRIPTOR_HANDLE dsv_h{dsv_cpu.ptr + dsv_inc};
                check(rtv_cpu.ptr == reinterpret_cast<SIZE_T>(dual_state.rtv_backing) &&
                          dsv_cpu.ptr == reinterpret_cast<SIZE_T>(dual_state.dsv_backing),
                      "RTV and DSV heaps report their title-backed CPU descriptor bases");

                device->CreateRenderTargetView(rt_tex.Get(), nullptr, rtv_h);
                device->CreateDepthStencilView(ds_tex.Get(), nullptr, dsv_h);

                using CreateQueueXFn = HRESULT(WINAPI*)(void*, const D3D12_COMMAND_QUEUE_DESC*, const IID*, void**);
                using CreateListXFn = HRESULT(WINAPI*)(void*, const void*, ID3D12CommandAllocator*,
                                                       ID3D12PipelineState*, const IID*, void**);
                using CloseFn = HRESULT(WINAPI*)(void*);
                using BarrierFn = void(WINAPI*)(void*, UINT, const D3D12_RESOURCE_BARRIER*);
                using CopyTexFn = void(WINAPI*)(void*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                                                const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
                using OmSetRtFn = void(WINAPI*)(void*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                                const D3D12_CPU_DESCRIPTOR_HANDLE*);
                using ClearDsvFn = void(WINAPI*)(void*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8,
                                                 UINT, const D3D12_RECT*);
                using ClearRtvFn = void(WINAPI*)(void*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT,
                                                 const D3D12_RECT*);

                const D3D12_COMMAND_QUEUE_DESC qd{};
                const IID iid_q = __uuidof(ID3D12CommandQueue);
                const IID iid_l = __uuidof(ID3D12GraphicsCommandList);
                ComPtr<ID3D12CommandQueue> clear_q;
                ComPtr<ID3D12CommandAllocator> clear_alloc;
                ComPtr<ID3D12GraphicsCommandList> clear_list;
                ComPtr<ID3D12Fence> clear_fence;
                hr = reinterpret_cast<CreateQueueXFn>(proxy_vtbl[48])(
                    device.Get(), &qd, &iid_q, reinterpret_cast<void**>(clear_q.GetAddressOf()));
                HRESULT hra = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                             IID_PPV_ARGS(clear_alloc.GetAddressOf()));
                struct {
                    UINT Type;
                    UINT NodeMask;
                } ld{D3D12_COMMAND_LIST_TYPE_DIRECT, 0};
                HRESULT hrl = SUCCEEDED(hra) ? reinterpret_cast<CreateListXFn>(proxy_vtbl[73])(
                                                   device.Get(), &ld, clear_alloc.Get(), nullptr, &iid_l,
                                                   reinterpret_cast<void**>(clear_list.GetAddressOf()))
                                             : hra;
                HRESULT hrf = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(clear_fence.GetAddressOf()));
                check(SUCCEEDED(hr) && SUCCEEDED(hra) && SUCCEEDED(hrl) && SUCCEEDED(hrf) &&
                          rt::d3d12_xs::is_list_proxy(clear_list.Get()),
                      "console list proxy created for hooked RTV/DSV clear");

                if (clear_q && clear_list && clear_fence) {
                    void** lv = *reinterpret_cast<void***>(clear_list.Get());
                    const FLOAT rgba[4] = {1.0f, 0.0f, 0.5f, 1.0f};
                    reinterpret_cast<OmSetRtFn>(lv[50])(clear_list.Get(), 1, &rtv_h, FALSE, &dsv_h);
                    reinterpret_cast<ClearRtvFn>(lv[52])(clear_list.Get(), rtv_h, rgba, 0, nullptr);
                    reinterpret_cast<ClearDsvFn>(lv[51])(clear_list.Get(), dsv_h, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 0,
                                                         nullptr);

                    D3D12_RESOURCE_BARRIER b[2]{};
                    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b[0].Transition = {rt_tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
                    b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b[1].Transition = {ds_tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                       D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE};
                    reinterpret_cast<BarrierFn>(lv[26])(clear_list.Get(), 2, b);

                    D3D12_TEXTURE_COPY_LOCATION dst_rt{}, src_rt{}, dst_ds{}, src_ds{};
                    dst_rt.pResource = rb_buf.Get();
                    dst_rt.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst_rt.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 256}};
                    src_rt.pResource = rt_tex.Get();
                    src_rt.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dst_ds.pResource = rb_buf.Get();
                    dst_ds.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst_ds.PlacedFootprint = {1536, {DXGI_FORMAT_R32_TYPELESS, 4, 4, 1, 256}};
                    src_ds.pResource = ds_tex.Get();
                    src_ds.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    reinterpret_cast<CopyTexFn>(lv[16])(clear_list.Get(), &dst_rt, 0, 0, 0, &src_rt, nullptr);
                    reinterpret_cast<CopyTexFn>(lv[16])(clear_list.Get(), &dst_ds, 0, 0, 0, &src_ds, nullptr);

                    const HRESULT hr_close = reinterpret_cast<CloseFn>(lv[9])(clear_list.Get());
                    check(hr_close == S_OK, "console list Close succeeds after title-backed RTV/DSV clears");

                    ID3D12CommandList* lists[] = {clear_list.Get()};
                    clear_q->ExecuteCommandLists(1, lists);
                    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    const bool done = ev && SUCCEEDED(clear_q->Signal(clear_fence.Get(), 1)) &&
                                      SUCCEEDED(clear_fence->SetEventOnCompletion(1, ev)) &&
                                      WaitForSingleObject(ev, 5000) == 0;
                    if (ev) CloseHandle(ev);
                    check(done, "console list clear commands complete on the GPU");

                    void* mapped = nullptr;
                    const D3D12_RANGE range{0, 4096};
                    if (done && SUCCEEDED(rb_buf->Map(0, &range, &mapped)) && mapped) {
                        const auto* bytes = static_cast<const std::uint8_t*>(mapped);
                        const bool rt_ok = bytes[0] == 255 && bytes[1] == 0 && (bytes[2] == 127 || bytes[2] == 128) &&
                                           bytes[3] == 255;
                        float depth_got = -1.0f;
                        std::memcpy(&depth_got, bytes + 1536, sizeof(depth_got));
                        const bool ds_ok = std::fabs(depth_got - 0.25f) < 1e-6f;
                        check(rt_ok, "console slot 52 ClearRenderTargetView writes expected RGBA texel via hooked heap");
                        check(ds_ok, "console slot 51 ClearDepthStencilView preserves FLOAT depth 0.25 via XMM3");
                        const D3D12_RANGE empty{0, 0};
                        rb_buf->Unmap(0, &empty);
                    }
                }
            }
            rtv_heap.Reset();
            dsv_heap.Reset();
            rt::shims::remove_xmem_allocation_hooks(&dual_hooks);
        }
    }

    // Hold a reference to the PC device so the final check can prove the proxy's Release balances its own.
    if (real) real->AddRef();
    // Drop every device child before the proxy so the PC device is not released first.
    root_signature.Reset();
    reserved.Reset();
    texture.Reset();
    buffer.Reset();
    allocator.Reset();
    device1.Reset();
    const unsigned long refs_before_heap_release = rt::d3d12_xs::device_proxy_ref_count(device.Get());
    heap.Reset();
    check(rt::d3d12_xs::device_proxy_ref_count(device.Get()) + 1 == refs_before_heap_release,
          "heap release drops its device reference");
    device.Reset();
    if (real) {
        check(real->GetNodeCount() > 0, "PC device survives the proxy's final Release");
        real->Release();
    }

    std::printf("device_proxy_test: %s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
