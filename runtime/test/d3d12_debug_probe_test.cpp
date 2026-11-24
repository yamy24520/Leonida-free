// LEONIDA_PROBE, LEONIDA_PROBE_MODE, LEONIDA_PROBE_POS (src/debug_probe.h) and LEONIDA_DEBUG_CLEAR
// (shims/d3d12_com.cpp) through the Xbox-ABI proxy over the Vulkan device. First without a GPU: the settings parser
// and the patched words of the POSITION pass-through. Then on a 64x64 target cleared blue, with the variables set
// before each pipeline is made:
//   - no pipeline matches: the title VS (test/glsl/vs_draw.vert) moves its quad by the root constants to the
//     top-right quadrant and the title PS (ps_draw.frag) colours it red;
//   - MODE=ps on the PS name: the same quad, green (the built-in pixel shader);
//   - both stages (MODE unset) with FrontCounterClockwise, which makes the probe triangle back-facing: the whole target
//     green, so the probe's culling is off;
//   - MODE=vs on a PS with no inputs (fs_solid.frag): the whole target in that PS's colour;
//   - POS=ndc on the VS name: the quad in the top-left quadrant, where its POSITION puts it without the title VS's
//     offset; POSITION is the VS's input register 1, so this also checks the moved Location;
//   - LEONIDA_PROBE=all with POS=px:64x64 and a quad given in pixels: the bottom-right quadrant (pixel y is down);
//   - LEONIDA_DEBUG_CLEAR=1: an all-zero clear reads back opaque magenta, while the blue clears above stay blue.
// No game data. Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation.
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/debug_probe.h"
#include "../src/shader_library.h"

#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>

#if !defined(LEONIDA_TEST_VS_SPV) || !defined(LEONIDA_TEST_PS_SPV) || !defined(LEONIDA_TEST_FS_SOLID_SPV)
#error "LEONIDA_TEST_VS_SPV, LEONIDA_TEST_PS_SPV and LEONIDA_TEST_FS_SOLID_SPV must name the compiled test shaders"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)
#define CHECK(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return false; } \
} while (0)

using namespace d3d12test;
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slot 9: handle in RAX

namespace {

struct Vertex {
    float x, y;
    std::uint8_t r, g, b, a;
};

using Pixel = std::array<std::uint8_t, 4>;
constexpr Pixel kBlue{0, 0, 255, 255}, kRed{255, 0, 0, 255}, kGreen{0, 255, 0, 255}, kMagenta{255, 0, 255, 255};
constexpr Pixel kSolid{32, 160, 240, 255};  // fs_solid.frag
constexpr UINT kSize = 64;

int parser_checks() {
    using Position = leonida::DebugProbe::Position;
    EXPECT(!leonida::parse_debug_probe(nullptr, "vs", "px").enabled());
    EXPECT(!leonida::parse_debug_probe("", nullptr, nullptr).enabled());
    auto p = leonida::parse_debug_probe("draw", nullptr, nullptr);
    EXPECT(p.enabled() && p.vertex && p.pixel && p.position == Position::fullscreen && p.problem.empty());
    EXPECT(p.matches("vs_draw") && !p.matches("vs_tri") && !p.matches(""));
    EXPECT(leonida::parse_debug_probe("all", "", "").matches("anything"));
    p = leonida::parse_debug_probe("x", "vs", "ndc");
    EXPECT(p.vertex && !p.pixel && p.position == Position::ndc && p.problem.empty());
    p = leonida::parse_debug_probe("x", "ps", "px");
    EXPECT(!p.vertex && p.pixel && p.position == Position::pixels && p.width == 3840 && p.height == 2160);
    p = leonida::parse_debug_probe("x", "both", "px:1920x1080");
    EXPECT(p.vertex && p.pixel && p.position == Position::pixels && p.width == 1920 && p.height == 1080);
    p = leonida::parse_debug_probe("x", "neither", "px:12");
    EXPECT(p.vertex && p.pixel && p.position == Position::fullscreen && !p.problem.empty());
    EXPECT(!leonida::parse_debug_probe("x", nullptr, "px:0x5").problem.empty());
    EXPECT(!leonida::parse_debug_probe("x", nullptr, "pixels").problem.empty());

    // The pass-through's words: its one Location becomes the given register, its SpecId 0 to 3 constants the values.
    const auto words = leonida::debug_probe_position_words(5, {0.25f, -0.5f, -1.0f, 1.0f});
    EXPECT(!words.empty());
    std::uint32_t spec_ids[4] = {}, location = ~0u;
    std::array<float, 4> values{};
    unsigned found = 0;
    for (std::size_t i = 5; i < words.size(); i += words[i] >> 16) {
        const std::uint32_t op = words[i] & 0xffffu;
        if (op == 71 && words[i + 2] == 30) location = words[i + 3];
        if (op == 71 && words[i + 2] == 1) spec_ids[words[i + 3]] = words[i + 1];
        if (op != 50) continue;
        for (unsigned id = 0; id < 4; ++id) {
            if (spec_ids[id] != words[i + 2]) continue;
            values[id] = std::bit_cast<float>(words[i + 3]);
            ++found;
        }
    }
    EXPECT(location == 5 && found == 4);
    EXPECT(values == (std::array<float, 4>{0.25f, -0.5f, -1.0f, 1.0f}));
    EXPECT(leonida::debug_probe_vertex_words({}, 0).size() == std::size(leonida::debug_probe_fullscreen_vs));
    return 0;
}

void set_env(const char* name, const char* value) { _putenv_s(name, value ? value : ""); }  // "" removes it

unsigned mismatches(const std::vector<std::uint8_t>& pixels, const char* what,
                    const std::function<Pixel(UINT col, UINT row)>& want_at) {
    unsigned bad = 0;
    for (UINT row = 0; row < kSize; ++row) {
        for (UINT col = 0; col < kSize; ++col) {
            const Pixel want = want_at(col, row);
            const std::uint8_t* got = pixels.data() + row * 256 + col * 4;
            if (std::equal(want.begin(), want.end(), got)) continue;
            if (bad++ < 4)
                std::fprintf(stderr, "%s: pixel (%u, %u): got %u,%u,%u,%u want %u,%u,%u,%u\n", what, col, row, got[0],
                             got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
        }
    }
    return bad;
}

}  // namespace

int main() {
    if (const int failed = parser_checks()) return failed;

    vktest::Device d;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) {
        std::printf("SKIP: no usable Vulkan 1.3 device (the parser checks passed)\n");
        return 77;  // ctest's SKIP_RETURN_CODE
    }
    EXPECT(device_status == vktest::DeviceStatus::ready);

    const auto vs_program = fssm_program({0x56530001, 0xbf810000});
    const auto ps_program = fssm_program({0x50530001, 0xbf810000});
    const auto solid_program = fssm_program({0x50530003, 0xbf810000});
    const auto vs_container = shader_container(vs_program), ps_container = shader_container(ps_program),
               solid_container = shader_container(solid_program);
    const auto library = std::filesystem::temp_directory_path() / "leonida_debug_probe_test";
    std::filesystem::create_directories(library);
    const auto copy = std::filesystem::copy_options::overwrite_existing;
    std::filesystem::copy_file(LEONIDA_TEST_VS_SPV, library / "vs_draw.spv", copy);
    std::filesystem::copy_file(LEONIDA_TEST_PS_SPV, library / "ps_draw.spv", copy);
    std::filesystem::copy_file(LEONIDA_TEST_FS_SOLID_SPV, library / "ps_solid.spv", copy);
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"vs_draw\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << leonida::sha256_hex(vs_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"input_signature\": ["
                 "{\"semantic\": \"COLOR\", \"semantic_index\": 0, \"register\": 0, \"mask\": 15, \"rw_mask\": 15, "
                 "\"system_value\": 0, \"component_type\": \"float\"}, "
                 "{\"semantic\": \"POSITION\", \"semantic_index\": 0, \"register\": 1, \"mask\": 3, \"rw_mask\": 3, "
                 "\"system_value\": 0, \"component_type\": \"float\"}]},\n"
                 " \"ps_draw\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << leonida::sha256_hex(ps_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"outputs\": [0]},\n"
                 " \"ps_solid\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << leonida::sha256_hex(solid_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"outputs\": [0]}}";
    }

    // Each run's variables, pipeline and draw; the pixels it read back.
    struct Run {
        const char* name;
        const char* probe;
        const char* mode;
        const char* pos;
        const std::vector<std::uint8_t>* ps;
        bool counter_clockwise = false;
        UINT first_vertex = 0;  // 0: the NDC quad; 4: the pixel quad
        bool zero_clear = false;
        std::vector<std::uint8_t> pixels = std::vector<std::uint8_t>(kSize * 256);
    };
    Run runs[] = {
        {"no match", "nomatch", nullptr, nullptr, &ps_container},
        {"ps only", "ps_draw", "ps", nullptr, &ps_container},
        {"both", "ps_draw", nullptr, nullptr, &ps_container, true},
        {"vs only", "ps_solid", "vs", nullptr, &solid_container},
        {"ndc", "vs_draw", "both", "ndc", &ps_container},
        {"pixels", "all", nullptr, "px:64x64", &ps_container, false, 4},
        {"debug clear", nullptr, nullptr, nullptr, &ps_container, false, 0, true},
    };
    set_env("LEONIDA_DEBUG_CLEAR", "1");  // read when each command list is made
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        info.features = d.enabled;
        info.subgroup = d.subgroup;
        info.shader_dir = library.string();
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&device.p))));
        rt::d3d12::set_device_outer(inner, device.p);

        // [0] two constants (s0, s1: the title VS's clip-space offset), [1] a root CBV (s2:s3: the title PS's tint).
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 2};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor = {1, 0};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        const auto root_blob = serialize_root_signature(root_desc);
        EXPECT(!root_blob.empty());
        Ref<ID3D12RootSignature> root;
        EXPECT(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));

        // The top-left quadrant as NDC, then the bottom-right quadrant as pixels of the 64x64 target; red.
        const Vertex vertices[8] = {{-1, 1, 255, 0, 0, 255},  {0, 1, 255, 0, 0, 255},  {-1, 0, 255, 0, 0, 255},
                                    {0, 0, 255, 0, 0, 255},   {32, 32, 255, 0, 0, 255}, {64, 32, 255, 0, 0, 255},
                                    {32, 64, 255, 0, 0, 255}, {64, 64, 255, 0, 0, 255}};
        const std::uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
        const float tint[3] = {1, 0, 1};  // red stays red

        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        Ref<ID3D12Resource> upload, readback, target;
        const auto upload_desc = buffer_desc(4096);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&upload.p))));
        const auto readback_desc = buffer_desc(kSize * 256);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback.p))));
        D3D12_RESOURCE_DESC rt_desc{};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = rt_desc.Height = kSize;
        rt_desc.DepthOrArraySize = 1;
        rt_desc.MipLevels = 1;
        rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rt_desc.SampleDesc.Count = 1;
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                         IID_PPV_ARGS(&target.p))));
        std::uint8_t* up = nullptr;
        EXPECT(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&up))));
        std::memcpy(up, vertices, sizeof(vertices));
        std::memcpy(up + 256, indices, sizeof(indices));
        std::memcpy(up + 512, tint, sizeof(tint));
        const D3D12_GPU_VIRTUAL_ADDRESS upload_va = upload->GetGPUVirtualAddress();

        const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                                                   0};
        Ref<ID3D12DescriptorHeap> rtv_heap;
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv{
            SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(rtv_heap.p, 9))(rtv_heap.p))};
        device->CreateRenderTargetView(target.p, nullptr, rtv);

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12Fence> fence;
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

        const D3D12_INPUT_ELEMENT_DESC elements[2] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        UINT64 fence_value = 0;
        const auto draw = [&](Run& run) -> bool {
            set_env("LEONIDA_PROBE", run.probe);
            set_env("LEONIDA_PROBE_MODE", run.mode);
            set_env("LEONIDA_PROBE_POS", run.pos);
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
            pso_desc.pRootSignature = root.p;
            pso_desc.VS = {vs_container.data(), vs_container.size()};
            pso_desc.PS = {run.ps->data(), run.ps->size()};
            pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pso_desc.SampleMask = 0xffffffff;
            pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
            pso_desc.RasterizerState.FrontCounterClockwise = run.counter_clockwise;
            pso_desc.RasterizerState.DepthClipEnable = TRUE;
            pso_desc.InputLayout = {elements, 2};
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso_desc.NumRenderTargets = 1;
            pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pso_desc.SampleDesc.Count = 1;
            Ref<ID3D12PipelineState> pso;
            CHECK(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso.p))));
            Ref<ID3D12CommandAllocator> allocator;
            Ref<ID3D12GraphicsCommandList> list;
            CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                           IID_PPV_ARGS(&allocator.p))));
            CHECK(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pso.p,
                                                      IID_PPV_ARGS(&list.p))));
            const FLOAT blue[4] = {0, 0, 1, 1}, zero[4] = {};
            list->ClearRenderTargetView(rtv, run.zero_clear ? zero : blue, 0, nullptr);
            if (!run.zero_clear) {
                list->SetGraphicsRootSignature(root.p);
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                const D3D12_VERTEX_BUFFER_VIEW vbv{upload_va, sizeof(vertices), sizeof(Vertex)};
                list->IASetVertexBuffers(0, 1, &vbv);
                const D3D12_INDEX_BUFFER_VIEW ibv{upload_va + 256, sizeof(indices), DXGI_FORMAT_R16_UINT};
                list->IASetIndexBuffer(&ibv);
                const D3D12_VIEWPORT viewport{0, 0, float(kSize), float(kSize), 0, 1};
                const D3D12_RECT scissor{0, 0, LONG(kSize), LONG(kSize)};
                list->RSSetViewports(1, &viewport);
                list->RSSetScissorRects(1, &scissor);
                list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                const float offset[2] = {1, 0};  // the title VS moves the quad to the top-right quadrant
                list->SetGraphicsRoot32BitConstants(0, 2, offset, 0);
                list->SetGraphicsRootConstantBufferView(1, upload_va + 512);
                list->DrawIndexedInstanced(6, 1, 0, INT(run.first_vertex), 0);
            }
            D3D12_RESOURCE_BARRIER barrier = transition(target.p, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                        D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = readback.p;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, 256}};
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = target.p;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier = transition(target.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &barrier);
            CHECK(list->Close() == S_OK);
            ID3D12CommandList* lists[] = {list.p};
            queue->ExecuteCommandLists(1, lists);
            CHECK(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
            CHECK(SUCCEEDED(queue->Signal(fence.p, ++fence_value)));
            CHECK(SUCCEEDED(fence->SetEventOnCompletion(fence_value, nullptr)));
            void* mapped = nullptr;
            CHECK(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
            std::memcpy(run.pixels.data(), mapped, run.pixels.size());
            readback->Unmap(0, nullptr);
            return true;
        };
        for (Run& run : runs) {
            if (!draw(run)) {
                std::fprintf(stderr, "run '%s' failed\n", run.name);
                return 1;
            }
        }
        for (const char* name : {"LEONIDA_PROBE", "LEONIDA_PROBE_MODE", "LEONIDA_PROBE_POS", "LEONIDA_DEBUG_CLEAR"})
            set_env(name, nullptr);

        for (auto* r : {&upload, &readback, &target}) r->reset();
        root.reset();
        rtv_heap.reset();
        queue.reset();
        fence.reset();
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);
    }
    std::filesystem::remove_all(library);

    const auto quadrant = [](bool top, bool left, Pixel inside) {
        return [=](UINT col, UINT row) {
            return (row < kSize / 2) == top && (col < kSize / 2) == left ? inside : kBlue;
        };
    };
    const auto everywhere = [](Pixel colour) { return [=](UINT, UINT) { return colour; }; };
    unsigned bad = 0;
    bad += mismatches(runs[0].pixels, runs[0].name, quadrant(true, false, kRed));
    bad += mismatches(runs[1].pixels, runs[1].name, quadrant(true, false, kGreen));
    bad += mismatches(runs[2].pixels, runs[2].name, everywhere(kGreen));
    bad += mismatches(runs[3].pixels, runs[3].name, everywhere(kSolid));
    bad += mismatches(runs[4].pixels, runs[4].name, quadrant(true, true, kGreen));
    bad += mismatches(runs[5].pixels, runs[5].name, quadrant(false, false, kGreen));
    bad += mismatches(runs[6].pixels, runs[6].name, everywhere(kMagenta));
    EXPECT(!bad);
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("debug probe: parser and patched words checked; on the GPU an unmatched pipeline drew its own quad, "
                "the probe pixel shader coloured it, the probe vertex shader covered the target (culling off), with "
                "the title PS too, POSITION passed through as NDC and as pixels, and an all-zero clear turned magenta; "
                "all "
                "%u pixels of %zu runs matched\n",
                kSize * kSize, std::size(runs));
    return 0;
}
