// Xbox device slot 57 (d3d12_xs_native_pso.h) through the device proxy, over a mock device: no D3D12 runtime, no
// GPU, no game data. The descriptions, digest map and "containers" are built here by hand; the mock records what the
// proxy hands to ID3D12Device2::CreatePipelineState (PC slot 47), and slot 55 deriving from such a pipeline.
#include "../shims/d3d12_xs_device.h"
#include "../shims/d3d12_xs_native_pso.h"
#include "../shims/d3d12_xs_pso_stream.h"
#include "../src/native_pso.h"

#include <d3d12.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace np = leonida::native_pso;
namespace xs = rt::d3d12_xs;

int g_failures = 0;
#define EXPECT(c) \
    do { if (!(c)) { std::fprintf(stderr, "xs_native_pso_test line %d: %s\n", __LINE__, #c); ++g_failures; } } while (0)

// ---- Mock pipeline: IUnknown and ID3D12Object private data (slot 55 keeps its stream record there); one placed
// at an odd address for the even-pointer check ----

struct MockPipeline {
    void** vtable;
    long refs;
    GUID data_guid;
    std::vector<std::uint8_t> data;
    GUID interface_guid;
    IUnknown* object;
};

unsigned g_odd_releases = 0;
alignas(16) unsigned char g_odd_storage[32];

bool odd(const void* p) { return reinterpret_cast<std::uintptr_t>(p) & 1; }

HRESULT WINAPI pipe_qi(MockPipeline* self, const IID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (odd(self) || !(IsEqualIID(*riid, __uuidof(IUnknown)) || IsEqualIID(*riid, __uuidof(ID3D12Object)) ||
                       IsEqualIID(*riid, __uuidof(ID3D12PipelineState))))
        return E_NOINTERFACE;
    ++self->refs;
    *out = self;
    return S_OK;
}
ULONG WINAPI pipe_add_ref(MockPipeline* self) { return odd(self) ? 1 : ++self->refs; }
ULONG WINAPI pipe_release(MockPipeline* self) {
    if (odd(self)) return ++g_odd_releases, 0;
    return --self->refs;
}
HRESULT WINAPI pipe_get_private_data(MockPipeline* self, const GUID* guid, UINT* size, void* data) {
    if (!IsEqualGUID(*guid, self->data_guid) || self->data.empty()) return DXGI_ERROR_NOT_FOUND;
    if (!data || *size < self->data.size()) return E_INVALIDARG;
    *size = static_cast<UINT>(self->data.size());
    std::memcpy(data, self->data.data(), self->data.size());
    return S_OK;
}
HRESULT WINAPI pipe_set_private_data(MockPipeline* self, const GUID* guid, UINT size, const void* data) {
    self->data_guid = *guid;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    self->data.assign(bytes, bytes + size);
    return S_OK;
}
HRESULT WINAPI pipe_set_private_interface(MockPipeline* self, const GUID* guid, IUnknown* object) {
    if (object) object->AddRef();
    self->interface_guid = *guid;
    self->object = object;  // kept for the life of the test
    return S_OK;
}
HRESULT WINAPI pipe_set_name(MockPipeline*, const wchar_t*) { return S_OK; }
void* g_pipe_vtable[] = {reinterpret_cast<void*>(&pipe_qi),
                         reinterpret_cast<void*>(&pipe_add_ref),
                         reinterpret_cast<void*>(&pipe_release),
                         reinterpret_cast<void*>(&pipe_get_private_data),
                         reinterpret_cast<void*>(&pipe_set_private_data),
                         reinterpret_cast<void*>(&pipe_set_private_interface),
                         reinterpret_cast<void*>(&pipe_set_name)};

// The root signature slot 57 passes on: an IUnknown, since slot 86's kept stream holds a reference to it.
MockPipeline g_root_signature{g_pipe_vtable, 1, {}, {}, {}, nullptr};

// ---- Mock device: every pipeline must come through PC slot 47 (ID3D12Device2::CreatePipelineState) ----

struct Recorded {
    std::vector<std::uint8_t> vs, ps, gs, ms;
    const void* root = nullptr;
    UINT rt_count = 0;
    DXGI_FORMAT rtv[8]{};
    DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
    BOOL depth_enable = FALSE;
    D3D12_COMPARISON_FUNC depth_func{};
    D3D12_CULL_MODE cull{};
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology{};
    bool has_layout = false, has_topology = false;
    UINT samples = 0, sample_mask = 0;
    BOOL blend0 = TRUE;
    std::vector<std::string> names;
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
};

struct MockDevice {
    void** vtable;
    long refs;
    bool device2;
    HRESULT result;
    bool odd_pointer;
    unsigned graphics_calls, stream_calls;
    Recorded last;
    MockPipeline* last_pipeline;
};

std::vector<std::uint8_t> copy(const D3D12_SHADER_BYTECODE& b) {
    const auto* p = static_cast<const std::uint8_t*>(b.pShaderBytecode);
    return p ? std::vector<std::uint8_t>(p, p + b.BytecodeLength) : std::vector<std::uint8_t>{};
}

HRESULT hand_out(MockDevice* self, const IID* riid, void** out) {
    if (!out) return S_FALSE;
    *out = nullptr;
    if (FAILED(self->result)) return self->result;
    if (!riid || !IsEqualIID(*riid, __uuidof(ID3D12PipelineState))) return E_NOINTERFACE;
    if (self->odd_pointer) {
        void* object = g_odd_storage + 1;
        void** vtable = g_pipe_vtable;
        std::memcpy(object, &vtable, sizeof(vtable));
        *out = object;
    } else {
        self->last_pipeline = new MockPipeline{g_pipe_vtable, 1, {}, {}, {}, nullptr};
        *out = self->last_pipeline;
    }
    return S_OK;
}

HRESULT WINAPI dev_qi(MockDevice* self, const IID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    const bool known = IsEqualIID(*riid, __uuidof(IUnknown)) || IsEqualIID(*riid, __uuidof(ID3D12Object)) ||
                       IsEqualIID(*riid, __uuidof(ID3D12Device)) || IsEqualIID(*riid, __uuidof(ID3D12Device1)) ||
                       (self->device2 && IsEqualIID(*riid, __uuidof(ID3D12Device2)));
    if (!known) return E_NOINTERFACE;
    ++self->refs;
    *out = self;
    return S_OK;
}
ULONG WINAPI dev_add_ref(MockDevice* self) { return ++self->refs; }
ULONG WINAPI dev_release(MockDevice* self) { return --self->refs; }
HRESULT WINAPI dev_other(MockDevice*) { return S_OK; }

HRESULT WINAPI dev_graphics(MockDevice* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, const IID*, void** out) {
    ++self->graphics_calls;  // slot 57 must not take this path: slot 55 could not derive from its pipelines
    if (out) *out = nullptr;
    return E_FAIL;
}

template <typename P>
const P* payload(const std::uint8_t* base, std::size_t& at) {
    const std::size_t p = (at + 4 + alignof(P) - 1) & ~(alignof(P) - 1);
    at = (p + sizeof(P) + 7) & ~std::size_t(7);
    return reinterpret_cast<const P*>(base + p);
}

HRESULT WINAPI dev_stream(MockDevice* self, const D3D12_PIPELINE_STATE_STREAM_DESC* d, const IID* riid, void** out) {
    ++self->stream_calls;
    Recorded r;
    const auto* base = static_cast<const std::uint8_t*>(d->pPipelineStateSubobjectStream);
    for (std::size_t at = 0; at < d->SizeInBytes;) {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        std::memcpy(&type, base + at, sizeof(type));
        switch (type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: r.root = *payload<void*>(base, at); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: r.vs = copy(*payload<D3D12_SHADER_BYTECODE>(base, at)); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: r.gs = copy(*payload<D3D12_SHADER_BYTECODE>(base, at)); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: r.ms = copy(*payload<D3D12_SHADER_BYTECODE>(base, at)); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: r.ps = copy(*payload<D3D12_SHADER_BYTECODE>(base, at)); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
                r.blend0 = payload<D3D12_BLEND_DESC>(base, at)->RenderTarget[0].BlendEnable;
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: r.sample_mask = *payload<UINT>(base, at); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
                r.cull = payload<D3D12_RASTERIZER_DESC>(base, at)->CullMode;
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: {
                const auto* ds = payload<D3D12_DEPTH_STENCIL_DESC>(base, at);
                r.depth_enable = ds->DepthEnable;
                r.depth_func = ds->DepthFunc;
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: {
                const auto* layout = payload<D3D12_INPUT_LAYOUT_DESC>(base, at);
                r.has_layout = true;
                for (UINT i = 0; i < layout->NumElements; ++i) {
                    r.names.emplace_back(layout->pInputElementDescs[i].SemanticName);
                    r.elements.push_back(layout->pInputElementDescs[i]);
                }
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
                r.has_topology = true;
                r.topology = *payload<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(base, at);
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
                const auto* f = payload<D3D12_RT_FORMAT_ARRAY>(base, at);
                r.rt_count = f->NumRenderTargets;
                std::memcpy(r.rtv, f->RTFormats, sizeof(r.rtv));
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
                r.dsv = *payload<DXGI_FORMAT>(base, at);
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
                r.samples = payload<DXGI_SAMPLE_DESC>(base, at)->Count;
                break;
            default:
                std::fprintf(stderr, "xs_native_pso_test: unexpected subobject %d\n", static_cast<int>(type));
                ++g_failures;
                return E_INVALIDARG;
        }
    }
    self->last = std::move(r);
    return hand_out(self, riid, out);
}

void* g_dev_vtable[128];

MockDevice* new_device(bool device2 = true) {
    for (void*& slot : g_dev_vtable) slot = reinterpret_cast<void*>(&dev_other);
    g_dev_vtable[0] = reinterpret_cast<void*>(&dev_qi);
    g_dev_vtable[1] = reinterpret_cast<void*>(&dev_add_ref);
    g_dev_vtable[2] = reinterpret_cast<void*>(&dev_release);
    g_dev_vtable[10] = reinterpret_cast<void*>(&dev_graphics);
    g_dev_vtable[47] = reinterpret_cast<void*>(&dev_stream);
    auto* d = new MockDevice{};
    d->vtable = g_dev_vtable;
    d->refs = 1;
    d->device2 = device2;
    return d;
}

// ---- Hand-built inputs ----

void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    if (b.size() < at + 4) b.resize(at + 4);
    std::memcpy(b.data() + at, &v, 4);
}

std::vector<std::uint8_t> state_b() {
    std::vector<std::uint8_t> b(np::kStateBHeader, 0);
    b[0x10] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    b[0x11] = DXGI_FORMAT_R16G16_FLOAT;
    put32(b, 0x1c, 0xffffffffu);
    b[0x24] = DXGI_FORMAT_D32_FLOAT;
    b[0x25] = 2;
    b[0x26] = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    b[0x28] = 1;
    put32(b, 0x4c, 2);
    const struct { const char* name; std::uint32_t format, offset; } elements[] = {
        {"POSITION", DXGI_FORMAT_R32G32B32_FLOAT, 0}, {"TEXCOORD", DXGI_FORMAT_R32G32_FLOAT, 12}};
    for (const auto& e : elements) {
        const std::uint32_t length = static_cast<std::uint32_t>(std::strlen(e.name) + 1);
        put32(b, b.size(), length);
        b.insert(b.end(), e.name, e.name + length);
        for (const std::uint32_t v : {0u, e.format, 0u, e.offset, 0u, 0u}) put32(b, b.size(), v);
    }
    return b;
}

np::Digest digest(std::uint8_t seed) {
    np::Digest d;
    for (std::size_t i = 0; i < d.size(); ++i) d[i] = static_cast<std::uint8_t>(seed * 31 + i);
    return d;
}

std::vector<std::uint8_t> records(const np::Digest& d) {
    std::vector<std::uint8_t> b;
    for (const std::uint32_t v : {0x403u, 0x41u, 0u, 0x43u}) put32(b, b.size(), v);
    b.insert(b.end(), d.begin(), d.end());
    return b;
}

const np::Digest kVs = digest(1), kPs = digest(2), kMs = digest(3), kLost = digest(4), kUnknown = digest(5);
const std::vector<std::uint8_t> kVsBytes = {'D', 'X', 'B', 'C', 'v', 's'}, kPsBytes = {'D', 'X', 'B', 'C', 'p', 's'},
                                kMsBytes = {'D', 'X', 'B', 'C', 'm', 's'};
std::filesystem::path g_root, g_spirv;

std::filesystem::path test_shader_dir() { return g_spirv; }

void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary).write(text.data(), static_cast<std::streamsize>(text.size()));
}
void write(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    write(path, std::string(bytes.begin(), bytes.end()));
}

void make_tree() {
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH, temp);
    g_root = std::filesystem::path(temp) / (L"leonida_native_pso_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(g_root);
    g_spirv = g_root / L"spirv";
    std::filesystem::create_directories(g_spirv);
    std::filesystem::create_directories(g_root / L"dxbc");
    write(g_root / L"dxbc" / L"vs one.dxbc", kVsBytes);
    write(g_root / L"dxbc" / L"ps.dxbc", kPsBytes);
    write(g_root / L"dxbc" / L"ms.dxbc", kMsBytes);
    const auto line = [](const np::Digest& d, const char* stage, const char* path) {
        return np::hex(d) + "\t" + stage + "\t" + path + "\n";
    };
    const std::string map = std::string(np::HashMap::kHeader) + "\n" + line(kVs, "vs", "../dxbc/vs one.dxbc") +
                            line(kPs, "ps", "../dxbc/ps.dxbc") + line(kMs, "ms", "../dxbc/ms.dxbc") +
                            line(kLost, "ps", "../dxbc/gone.dxbc");
    write(g_spirv / L"native_hashes.txt", map);
}

struct Desc {
    std::vector<std::uint8_t> a = std::vector<std::uint8_t>(384, 0), b = state_b();
    std::vector<std::uint8_t> code[np::kStageBlocks], rec[np::kStageBlocks];
    np::GraphicsDesc desc{};
    Desc() {
        desc.root_signature = &g_root_signature;
        desc.state_a = {a.data(), a.size()};
        desc.state_b = {b.data(), b.size()};
    }
    Desc& stage(std::size_t k, const np::Digest& d) {
        code[k].assign(64, 0xbf);
        rec[k] = records(d);
        desc.stages[k] = {{code[k].data(), code[k].size()}, {rec[k].data(), rec[k].size()}};
        return *this;
    }
};

const IID kPipelineIid = __uuidof(ID3D12PipelineState);

HRESULT call57(void* device, const void* desc, const IID* riid, void** out) {
    using Fn = std::uint64_t(WINAPI*)(void*, const void*, const IID*, void**);
    const auto fn = reinterpret_cast<Fn>((*static_cast<void***>(device))[57]);
    return static_cast<HRESULT>(static_cast<std::uint32_t>(fn(device, desc, riid, out)));
}

void* proxy_over(MockDevice* mock) {
    void* device = nullptr;
    if (FAILED(xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(mock), __uuidof(ID3D12Device), &device)))
        return nullptr;
    return device;
}

void fresh(bool provider = true) {
    xs::reset_native_pso_state();
    xs::set_native_shader_directory_provider(provider ? &test_shader_dir : nullptr);
    SetEnvironmentVariableW(L"LEONIDA_STUB_PIPELINES", nullptr);
    SetEnvironmentVariableW(L"LEONIDA_NATIVE_PSO", nullptr);
    SetEnvironmentVariableW(L"LEONIDA_SHADERS", nullptr);
}

void test_vs_ps() {
    fresh();
    MockDevice* mock = new_device();
    void* device = proxy_over(mock);
    EXPECT(device && xs::device_slot_overridden(57));
    Desc d;
    d.stage(2, kPs).stage(0, kVs);
    void* out = reinterpret_cast<void*>(1);
    EXPECT(call57(device, &d.desc, &kPipelineIid, &out) == S_OK);
    EXPECT(out == mock->last_pipeline && mock->stream_calls == 1 && mock->graphics_calls == 0);
    const Recorded& r = mock->last;
    EXPECT(r.vs == kVsBytes && r.ps == kPsBytes && r.gs.empty());
    EXPECT(r.root == static_cast<const void*>(&g_root_signature));
    EXPECT(r.rt_count == 2 && r.rtv[0] == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && r.rtv[1] == DXGI_FORMAT_R16G16_FLOAT &&
           r.rtv[2] == DXGI_FORMAT_UNKNOWN);
    EXPECT(r.dsv == DXGI_FORMAT_D32_FLOAT && r.depth_enable && r.depth_func == D3D12_COMPARISON_FUNC_ALWAYS);
    EXPECT(r.cull == D3D12_CULL_MODE_NONE && !r.blend0 && r.samples == 1 && r.sample_mask == 0xffffffffu);
    EXPECT(r.has_layout && r.has_topology && r.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    EXPECT(r.names.size() == 2 && r.names[0] == "POSITION" && r.names[1] == "TEXCOORD");
    EXPECT(r.elements.size() == 2 && r.elements[1].Format == DXGI_FORMAT_R32G32_FLOAT &&
           r.elements[1].AlignedByteOffset == 12);
    const auto stats = xs::native_pso_stats();
    EXPECT(stats.calls == 1 && stats.created == 1 && stats.refused == 0 && stats.stubbed == 0);

    // The map is read once: a second pipeline does not need the provider again.
    xs::set_native_shader_directory_provider(nullptr);
    EXPECT(call57(device, &d.desc, &kPipelineIid, &out) == S_OK && mock->stream_calls == 2);
}

// A container is read from disk once: a later pipeline naming it gets the same bytes even if the file is gone.
void test_container_read_once() {
    fresh();
    MockDevice* mock = new_device();
    void* device = proxy_over(mock);
    Desc d;
    d.stage(0, kVs).stage(1, kPs);
    void* out = nullptr;
    EXPECT(call57(device, &d.desc, &kPipelineIid, &out) == S_OK && mock->last.vs == kVsBytes);

    const std::filesystem::path vs = g_root / L"dxbc" / L"vs one.dxbc", moved = g_root / L"dxbc" / L"vs one.moved";
    std::filesystem::rename(vs, moved);
    out = nullptr;
    const HRESULT again = call57(device, &d.desc, &kPipelineIid, &out);
    std::filesystem::rename(moved, vs);
    EXPECT(again == S_OK && mock->stream_calls == 2 && mock->last.vs == kVsBytes && mock->last.ps == kPsBytes);

    // A container that could not be read is not remembered as missing: once the file is there, it resolves.
    fresh();
    std::filesystem::rename(vs, moved);
    out = nullptr;
    EXPECT(call57(device, &d.desc, &kPipelineIid, &out) == E_NOTIMPL);
    std::filesystem::rename(moved, vs);
    EXPECT(call57(device, &d.desc, &kPipelineIid, &out) == S_OK && mock->last.vs == kVsBytes);
}

void test_mesh() {
    fresh();
    MockDevice* mock = new_device();
    void* device = proxy_over(mock);
    Desc d;
    d.stage(1, kPs).stage(6, kMs);
    void* out = nullptr;
    EXPECT(call57(device, &d.desc, &kPipelineIid, &out) == S_OK);
    EXPECT(out == mock->last_pipeline && mock->stream_calls == 1 && mock->graphics_calls == 0 &&
           !mock->last.has_layout && !mock->last.has_topology);
    EXPECT(mock->last.ms == kMsBytes && mock->last.ps == kPsBytes &&
           mock->last.root == static_cast<const void*>(&g_root_signature));
    EXPECT(mock->last.rt_count == 2 && mock->last.dsv == DXGI_FORMAT_D32_FLOAT && mock->last.samples == 1);
    EXPECT(mock->last.cull == D3D12_CULL_MODE_NONE && mock->last.depth_func == D3D12_COMPARISON_FUNC_ALWAYS);

    MockDevice* old = new_device(false);
    void* old_device = proxy_over(old);
    EXPECT(call57(old_device, &d.desc, &kPipelineIid, &out) == E_NOTIMPL && !out && old->stream_calls == 0);
}

// Slot 55 derives from a slot-57 pipeline: the stream slot 57 made is kept on it (#7759).
void test_derive_from_slot57() {
    fresh();
    MockDevice* mock = new_device();
    void* device = proxy_over(mock);
    Desc d;
    d.stage(0, kVs).stage(1, kPs);
    void* base = nullptr;
    EXPECT(call57(device, &d.desc, &kPipelineIid, &base) == S_OK && base == mock->last_pipeline);
    std::vector<std::uint8_t> record(xs::kDerivedRecordSize, 0);
    put32(record, 0, 3);  // tag 3: sample mask
    put32(record, xs::kDerivedRecordPayload, 0x0f);
    void* derived = nullptr;
    using Fn = std::uint64_t(WINAPI*)(void*, void*, std::uint64_t, const void*, const IID*, void**);
    const auto fn = reinterpret_cast<Fn>((*static_cast<void***>(device))[55]);
    EXPECT(static_cast<HRESULT>(static_cast<std::uint32_t>(fn(device, base, 1, record.data(), &kPipelineIid,
                                                              &derived))) == S_OK);
    EXPECT(derived && derived != base && mock->stream_calls == 2 && mock->graphics_calls == 0);
    EXPECT(mock->last.sample_mask == 0x0f && mock->last.vs == kVsBytes && mock->last.ps == kPsBytes);
    EXPECT(mock->last.names.size() == 2 && mock->last.names[1] == "TEXCOORD" && mock->last.rt_count == 2);
}

void test_unresolved() {
    fresh();
    MockDevice* mock = new_device();
    void* device = proxy_over(mock);
    void* out = reinterpret_cast<void*>(1);
    Desc unknown;
    unknown.stage(0, kVs).stage(1, kUnknown);
    EXPECT(call57(device, &unknown.desc, &kPipelineIid, &out) == E_NOTIMPL && !out && mock->stream_calls == 0);
    Desc lost;
    lost.stage(0, kVs).stage(1, kLost);
    EXPECT(call57(device, &lost.desc, &kPipelineIid, &out) == E_NOTIMPL && !out && mock->stream_calls == 0);
    EXPECT(xs::native_pso_stats().refused == 2);

    // #7417's switch: the device gets a pipeline whose only stage is an FSSM blob no index names.
    SetEnvironmentVariableW(L"LEONIDA_STUB_PIPELINES", L"1");
    EXPECT(call57(device, &unknown.desc, &kPipelineIid, &out) == S_OK && out == mock->last_pipeline);
    EXPECT(mock->stream_calls == 1 && mock->last.vs.size() == 16 && std::memcmp(mock->last.vs.data(), "FSSM", 4) == 0);
    EXPECT(mock->last.ps.empty() && mock->last.elements.empty() && mock->last.rt_count == 2);
    EXPECT(xs::native_pso_stats().stubbed == 1);
    // A device without #7417 refuses the stub: counted as refused, not stubbed.
    mock->result = E_INVALIDARG;
    EXPECT(call57(device, &unknown.desc, &kPipelineIid, &out) == E_INVALIDARG && !out && mock->stream_calls == 2);
    EXPECT(xs::native_pso_stats().stubbed == 1 && xs::native_pso_stats().refused == 3);
    mock->result = S_OK;
    SetEnvironmentVariableW(L"LEONIDA_STUB_PIPELINES", L"0");
    EXPECT(call57(device, &unknown.desc, &kPipelineIid, &out) == E_NOTIMPL && mock->stream_calls == 2);

    Desc good;
    good.stage(0, kVs).stage(1, kPs);
    for (const wchar_t* off : {L"0", L"off", L"FALSE", L"no"}) {
        SetEnvironmentVariableW(L"LEONIDA_NATIVE_PSO", off);
        EXPECT(call57(device, &good.desc, &kPipelineIid, &out) == E_NOTIMPL && !out && mock->stream_calls == 2);
    }
    SetEnvironmentVariableW(L"LEONIDA_NATIVE_PSO", L"1");
    EXPECT(call57(device, &good.desc, &kPipelineIid, &out) == S_OK && mock->stream_calls == 3);
}

void test_arguments_and_results() {
    fresh();
    MockDevice* mock = new_device();
    void* device = proxy_over(mock);
    Desc d;
    d.stage(0, kVs).stage(1, kPs);
    void* out = reinterpret_cast<void*>(1);
    EXPECT(call57(device, &d.desc, &kPipelineIid, nullptr) == E_POINTER);
    EXPECT(call57(device, &d.desc, nullptr, &out) == E_POINTER && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(call57(device, nullptr, &kPipelineIid, &out) == E_INVALIDARG && !out);
    Desc no_b = d;
    no_b.desc.state_b = {};
    EXPECT(call57(device, &no_b.desc, &kPipelineIid, &out) == E_INVALIDARG);
    Desc half = d;
    half.desc.stages[0].records = {};  // code without records: unresolvable, not malformed
    EXPECT(call57(device, &half.desc, &kPipelineIid, &out) == E_NOTIMPL && mock->stream_calls == 0);

    mock->result = E_INVALIDARG;
    EXPECT(call57(device, &d.desc, &kPipelineIid, &out) == E_INVALIDARG && !out && mock->stream_calls == 1);
    mock->result = S_OK;
    mock->odd_pointer = true;
    EXPECT(call57(device, &d.desc, &kPipelineIid, &out) == E_FAIL && !out && g_odd_releases == 1);
}

std::filesystem::path g_provided;
std::filesystem::path provided_dir() { return g_provided; }

void test_directory() {
    // No provider (a host D3D12 device): refused, even with LEONIDA_SHADERS naming a good folder.
    fresh(false);
    MockDevice* mock = new_device();
    void* device = proxy_over(mock);
    Desc d;
    d.stage(0, kVs).stage(1, kPs);
    void* out = nullptr;
    SetEnvironmentVariableW(L"LEONIDA_SHADERS", g_spirv.c_str());
    EXPECT(call57(device, &d.desc, &kPipelineIid, &out) == E_NOTIMPL && mock->stream_calls == 0);

    // A folder without native_hashes.txt, and an empty path: nothing resolves.
    for (const std::filesystem::path& dir : {g_root, std::filesystem::path()}) {
        fresh(false);
        g_provided = dir;
        xs::set_native_shader_directory_provider(&provided_dir);
        EXPECT(call57(device, &d.desc, &kPipelineIid, &out) == E_NOTIMPL && mock->stream_calls == 0);
    }
}

}  // namespace

int main() {
    make_tree();
    test_vs_ps();
    test_container_read_once();
    test_mesh();
    test_derive_from_slot57();
    test_unresolved();
    test_arguments_and_results();
    test_directory();
    xs::reset_native_pso_state();
    std::error_code ignored;
    std::filesystem::remove_all(g_root, ignored);
    if (g_failures) {
        std::fprintf(stderr, "xs_native_pso_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("xs_native_pso_test: ok\n");
    return 0;
}
