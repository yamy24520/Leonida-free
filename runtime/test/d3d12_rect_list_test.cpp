// The console's rectangle and quad lists drawn through the D3D12-on-Vulkan layer (src/rect_list_geometry.h).
//
// Built like d3d12_translated_draw_test.cpp, from the same translated vs_m15 / ps_m15 (the VS fetches its vertices
// through a V#; the PS multiplies the colour by (0.5, 2, 1, 1) and adds the bias, 0 here), so the shaders are real
// translator output found by the SHA-256 of their FSSM programs.
//   frame 1, rectangle list: PSO topology type 19, IASetPrimitiveTopology(17), DrawInstanced(3). The three corners
//     v1 (-1, 1), v2 (1, 1), v3 (-1, -1) cover the top-left, top-right and bottom-left of the 64x64 target; the fourth
//     corner, v2 + v3 - v1 = (1, -1) with colour c2 + c3 - c1, is made by the layer. Every pixel must carry the plane
//     through the three corner colours; the bottom-right quadrant is only right when the fourth corner is.
//   frames 3 to 5, rectangle lists whose right angle is at the second vertex, at the third vertex, and a triangle with
//     no right angle at all. The layer takes the right-angle corner as the vertex opposite the longest screen edge, so
//     the first two must cover all 64x64 pixels on the colour plane too (the retail boot's full-screen draw has its
//     right angle at the second vertex); the third only has to draw something and not fail.
//   frame 2, quad list: PSO topology type TRIANGLE, IASetPrimitiveTopology(19), DrawInstanced(8): two quads (corners
//     in order around the quad), the top-left and bottom-right quadrants, each one colour. These pixels are exact.
// Pixels are read back and compared; the other two quadrants of frame 2 stay the clear colour. LEONIDA_VK_VALIDATION=1
// runs it under the validation layer.
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/gfx10_descriptors.h"
#include "../src/shader_library.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

#ifndef LEONIDA_TEST_M15_DIR
#error "LEONIDA_TEST_M15_DIR must name the folder build.py wrote (spirv/index.json, fssm/*.fssm)"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slots 9/10: handle in RAX

namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Vertex {  // 32 bytes, as vs_m15.asm reads it
    float position[4];
    float color[4];
};

using Pixel = std::array<float, 4>;
constexpr UINT target_extent = 64;
constexpr UINT pitch = target_extent * sizeof(Pixel);
constexpr D3D_PRIMITIVE_TOPOLOGY rect_list = D3D_PRIMITIVE_TOPOLOGY(17), quad_list = D3D_PRIMITIVE_TOPOLOGY(19);
const std::uint32_t rect_type_value = 19;  // not an enumerator of D3D12_PRIMITIVE_TOPOLOGY_TYPE

std::array<std::uint32_t, 4> make_vsharp(std::uint64_t base, std::uint32_t records) {
    leonida::gfx10::Buffer buffer;
    buffer.address = base;
    buffer.stride = 32;
    buffer.num_records = records;
    buffer.format = 77;  // GFX10_FORMAT_32_32_32_32_FLOAT
    buffer.oob_select = leonida::gfx10::OobSelect::structured;
    return leonida::gfx10::encode(buffer);
}

Vertex vertex(float x, float y, const float* c) { return Vertex{{x, y, 0.0f, 1.0f}, {c[0], c[1], c[2], c[3]}}; }

}  // namespace

int main() {
    vktest::Device d;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) return 77;  // no Vulkan GPU: ctest skips
    EXPECT(device_status == vktest::DeviceStatus::ready);
    if (!d.enabled.geometryShader) {
        std::printf("SKIP: no geometryShader on this GPU\n");
        return 77;
    }
    const std::filesystem::path root_dir = LEONIDA_TEST_M15_DIR;
    const auto vs_program = read_file(root_dir / "fssm" / "vs_m15.fssm");
    const auto ps_program = read_file(root_dir / "fssm" / "ps_m15.fssm");
    EXPECT(vs_program.size() >= 4 && ps_program.size() == 0x110);
    const auto vs_container = shader_container(vs_program), ps_container = shader_container(ps_program);

    // Frame 1: the rectangle's three corners. Vertices 0..2. Frame 2: two quads, vertices 3..10.
    const float ca[4] = {0.0f, 0.0f, 0.0f, 1.0f}, cb[4] = {1.0f, 0.0f, 0.5f, 1.0f}, cc[4] = {0.0f, 0.5f, 1.0f, 1.0f};
    const float q1[4] = {0.25f, 0.5f, 0.75f, 1.0f}, q2[4] = {1.0f, 0.0f, 0.5f, 1.0f};
    const Vertex vertices[20] = {vertex(-1, 1, ca), vertex(1, 1, cb), vertex(-1, -1, cc),
                                 vertex(-1, 1, q1), vertex(0, 1, q1), vertex(0, 0, q1), vertex(-1, 0, q1),
                                 vertex(0, 0, q2), vertex(1, 0, q2), vertex(1, -1, q2), vertex(0, -1, q2),
                                 // vertices 11..13: right angle at the second vertex; 14..16: at the third; 17..19: none
                                 vertex(-1, 1, ca), vertex(1, 1, cb), vertex(1, -1, cc),
                                 vertex(-1, 1, ca), vertex(1, -1, cb), vertex(-1, -1, cc),
                                 vertex(-1, 1, ca), vertex(0.5f, 0.75f, cb), vertex(-0.25f, -1, cc)};
    static_assert(sizeof(Vertex) == 32 && sizeof(vertices) == 640);
    const Vertex* const corner_cases[2] = {&vertices[11], &vertices[14]};

    std::vector<Pixel> rect_pixels(target_extent * target_extent), quad_pixels(target_extent * target_extent),
        second_pixels(target_extent * target_extent), third_pixels(target_extent * target_extent),
        skew_pixels(target_extent * target_extent);
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        info.features = d.enabled;  // geometryShader: the rectangle stage needs it
        info.shader_dir = (root_dir / "spirv").string();
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&device.p))));
        rt::d3d12::set_device_outer(inner, device.p);

        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = {1, &range};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = {0, 0, 4};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        const auto root_blob = serialize_root_signature(root_desc);
        EXPECT(!root_blob.empty());
        Ref<ID3D12RootSignature> root;
        EXPECT(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root.p;
        pso_desc.VS = {vs_container.data(), vs_container.size()};
        pso_desc.PS = {ps_container.data(), ps_container.size()};
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.SampleMask = 0xffffffff;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        pso_desc.SampleDesc.Count = 1;
        Ref<ID3D12PipelineState> rect_pso, quad_pso;
        std::memcpy(&pso_desc.PrimitiveTopologyType, &rect_type_value, sizeof(rect_type_value));  // 19: rectangles
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&rect_pso.p))));
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&quad_pso.p))));

        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        Ref<ID3D12Resource> vertex_buffer, upload, readback, target;
        const auto vb_desc = buffer_desc(sizeof(vertices));
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&vertex_buffer.p))));
        const auto upload_desc = buffer_desc(1024);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&upload.p))));
        const auto readback_desc = buffer_desc(pitch * target_extent);
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback.p))));
        D3D12_RESOURCE_DESC rt_desc{};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = rt_desc.Height = target_extent;
        rt_desc.DepthOrArraySize = 1;
        rt_desc.MipLevels = 1;
        rt_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        rt_desc.SampleDesc.Count = 1;
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&target.p))));
        std::uint8_t* up = nullptr;
        EXPECT(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&up))));
        std::memcpy(up, vertices, sizeof(vertices));

        // V#s over the one vertex buffer: slot 0 the rectangle's three corners, slot 1 the eight quad corners, slots 2 to 4
        // the three more rectangles.
        const D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5,
                                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        Ref<ID3D12DescriptorHeap> srv_heap;
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&srv_heap.p))));
        const std::uint64_t cpu = reinterpret_cast<XboxHeapHandleFn>(slot(srv_heap.p, 9))(srv_heap.p);
        const std::uint64_t gpu = reinterpret_cast<XboxHeapHandleFn>(slot(srv_heap.p, 10))(srv_heap.p);
        const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        EXPECT(cpu && gpu >= 0x10000 && gpu < (1ull << 32) && step >= 16);
        const std::uint64_t buffer_va = vertex_buffer->GetGPUVirtualAddress();
        EXPECT(buffer_va >= 0x10000);
        const std::array<std::uint32_t, 4> vsharps[5] = {make_vsharp(buffer_va, 3),
                                                         make_vsharp(buffer_va + 3 * sizeof(Vertex), 8),
                                                         make_vsharp(buffer_va + 11 * sizeof(Vertex), 3),
                                                         make_vsharp(buffer_va + 14 * sizeof(Vertex), 3),
                                                         make_vsharp(buffer_va + 17 * sizeof(Vertex), 3)};
        for (unsigned i = 0; i < 5; ++i)
            std::memcpy(reinterpret_cast<void*>(cpu + std::uint64_t(i) * step), vsharps[i].data(), 16);

        const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        Ref<ID3D12DescriptorHeap> rtv_heap;
        EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap.p))));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv{
            SIZE_T(reinterpret_cast<XboxHeapHandleFn>(slot(rtv_heap.p, 9))(rtv_heap.p))};
        device->CreateRenderTargetView(target.p, nullptr, rtv);

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        Ref<ID3D12Fence> fence;
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, rect_pso.p,
                                                   IID_PPV_ARGS(&list.p))));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

        list->CopyBufferRegion(vertex_buffer.p, 0, upload.p, 0, sizeof(vertices));
        D3D12_RESOURCE_BARRIER barriers[2] = {
            transition(vertex_buffer.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(target.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)};
        list->ResourceBarrier(2, barriers);

        const auto render_frame = [&](UINT64 frame, ID3D12PipelineState* pso, D3D_PRIMITIVE_TOPOLOGY topology,
                                      unsigned vsharp_slot, UINT vertex_count, std::vector<Pixel>& out) -> int {
            if (frame > 1) {
                EXPECT(SUCCEEDED(allocator->Reset()));
                EXPECT(SUCCEEDED(list->Reset(allocator.p, pso)));
                auto to_target = transition(target.p, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET);
                list->ResourceBarrier(1, &to_target);
            } else {
                list->SetPipelineState(pso);
            }
            const FLOAT clear[4] = {0.5f, 0.125f, 0.25f, 0.5f};
            list->ClearRenderTargetView(rtv, clear, 0, nullptr);
            list->SetGraphicsRootSignature(root.p);
            ID3D12DescriptorHeap* heaps[] = {srv_heap.p};
            list->SetDescriptorHeaps(1, heaps);
            list->IASetPrimitiveTopology(topology);
            const D3D12_VIEWPORT viewport{0, 0, float(target_extent), float(target_extent), 0, 1};
            const D3D12_RECT scissor{0, 0, LONG(target_extent), LONG(target_extent)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            const float constants[4] = {0, 0, 0, 0};  // no offset, no bias
            list->SetGraphicsRootDescriptorTable(0, D3D12_GPU_DESCRIPTOR_HANDLE{gpu + std::uint64_t(vsharp_slot) * step});
            list->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
            list->DrawInstanced(vertex_count, 1, 0, 0);
            D3D12_RESOURCE_BARRIER to_copy = transition(target.p, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                        D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &to_copy);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = readback.p;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = {0, {DXGI_FORMAT_R32G32B32A32_FLOAT, target_extent, target_extent, 1, pitch}};
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = target.p;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            EXPECT(list->Close() == S_OK);
            ID3D12CommandList* lists[] = {list.p};
            queue->ExecuteCommandLists(1, lists);
            EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
            EXPECT(SUCCEEDED(queue->Signal(fence.p, frame)));
            EXPECT(SUCCEEDED(fence->SetEventOnCompletion(frame, nullptr)));
            void* mapped = nullptr;
            EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
            std::memcpy(out.data(), mapped, out.size() * sizeof(Pixel));
            readback->Unmap(0, nullptr);
            return 0;
        };
        EXPECT(render_frame(1, rect_pso.p, rect_list, 0, 3, rect_pixels) == 0);
        EXPECT(render_frame(2, quad_pso.p, quad_list, 1, 8, quad_pixels) == 0);
        EXPECT(render_frame(3, rect_pso.p, rect_list, 2, 3, second_pixels) == 0);
        EXPECT(render_frame(4, rect_pso.p, rect_list, 3, 3, third_pixels) == 0);
        EXPECT(render_frame(5, rect_pso.p, rect_list, 4, 3, skew_pixels) == 0);
        for (auto* r : {&vertex_buffer, &upload, &readback, &target}) r->reset();
        rect_pso.reset();
        quad_pso.reset();
        root.reset();
        srv_heap.reset();
        rtv_heap.reset();
        list.reset();
        allocator.reset();
        queue.reset();
        fence.reset();
        EXPECT(rt::d3d12_xs::device_proxy_ref_count(device.p) == 1);
    }

    // Rectangle: the plane through the three corner colours, at each pixel centre, through the PS's scale.
    const float scale[4] = {0.5f, 2.0f, 1.0f, 1.0f};
    unsigned covered = 0, off = 0;
    float worst = 0.0f;
    for (UINT row = 0; row < target_extent; ++row) {
        for (UINT col = 0; col < target_extent; ++col) {
            const float u = (float(col) + 0.5f) / target_extent, v = (float(row) + 0.5f) / target_extent;
            const Pixel& got = rect_pixels[row * target_extent + col];
            bool pixel_ok = true;
            for (int c = 0; c < 4; ++c) {
                const float want = (ca[c] + u * (cb[c] - ca[c]) + v * (cc[c] - ca[c])) * scale[c];
                const float error = std::fabs(got[c] - want);
                worst = std::fmax(worst, error);
                if (!(error <= 2e-5f)) pixel_ok = false;
            }
            covered += pixel_ok;
            if (!pixel_ok && off++ < 8)
                std::fprintf(stderr, "rect pixel (%u, %u): got %g,%g,%g,%g\n", col, row, got[0], got[1], got[2], got[3]);
        }
    }
    // The four quadrant centres on their own, so a wrong fourth corner is named by quadrant.
    const char* names[4] = {"top left", "top right", "bottom left", "bottom right"};
    for (int quadrant = 0; quadrant < 4; ++quadrant) {
        const UINT col = (quadrant & 1 ? 48 : 16), row = (quadrant & 2 ? 48 : 16);
        const Pixel& got = rect_pixels[row * target_extent + col];
        const float u = (float(col) + 0.5f) / target_extent, v = (float(row) + 0.5f) / target_extent;
        for (int c = 0; c < 4; ++c) {
            const float want = (ca[c] + u * (cb[c] - ca[c]) + v * (cc[c] - ca[c])) * scale[c];
            if (!(std::fabs(got[c] - want) <= 2e-5f)) {
                std::fprintf(stderr, "rect %s quadrant: channel %d got %g want %g\n", names[quadrant], c, got[c], want);
                return 1;
            }
        }
    }
    EXPECT(covered == target_extent * target_extent);

    // Right angle at the second and at the third vertex: the plane through the three drawn corners, everywhere.
    const std::vector<Pixel>* const corner_pixels[2] = {&second_pixels, &third_pixels};
    const char* const corner_names[2] = {"second", "third"};
    unsigned corner_covered[2] = {0, 0};
    float corner_worst = 0.0f;
    for (int which = 0; which < 2; ++which) {
        const Vertex* t = corner_cases[which];
        const float x0 = t[0].position[0], y0 = t[0].position[1];
        const float ax = t[1].position[0] - x0, ay = t[1].position[1] - y0;
        const float bx = t[2].position[0] - x0, by = t[2].position[1] - y0;
        const float det = ax * by - ay * bx;
        for (UINT row = 0; row < target_extent; ++row) {
            for (UINT col = 0; col < target_extent; ++col) {
                const float px = 2.0f * (float(col) + 0.5f) / target_extent - 1.0f - x0;
                const float py = 1.0f - 2.0f * (float(row) + 0.5f) / target_extent - y0;
                const float s = (px * by - py * bx) / det, r = (ax * py - ay * px) / det;
                const Pixel& got = (*corner_pixels[which])[row * target_extent + col];
                bool pixel_ok = true;
                for (int c = 0; c < 4; ++c) {
                    const float want = (t[0].color[c] + s * (t[1].color[c] - t[0].color[c]) +
                                        r * (t[2].color[c] - t[0].color[c])) * scale[c];
                    const float error = std::fabs(got[c] - want);
                    corner_worst = std::fmax(corner_worst, error);
                    if (!(error <= 2e-5f)) pixel_ok = false;
                }
                corner_covered[which] += pixel_ok;
            }
        }
        if (corner_covered[which] != target_extent * target_extent) {
            std::fprintf(stderr, "rect list with the right angle at the %s vertex: %u of %u pixels on the plane\n",
                         corner_names[which], corner_covered[which], target_extent * target_extent);
            return 1;
        }
    }
    // Not a right triangle: any consistent corner is fine, but every corner choice makes a parallelogram that holds the
    // drawn triangle, so each pixel well inside the triangle must be drawn (not the clear colour).
    const Pixel clear_pixel = {0.5f, 0.125f, 0.25f, 0.5f};
    unsigned skew_drawn = 0, skew_inside = 0, skew_missing = 0;
    {
        const Vertex* t = &vertices[17];
        const float x0 = t[0].position[0], y0 = t[0].position[1];
        const float ax = t[1].position[0] - x0, ay = t[1].position[1] - y0;
        const float bx = t[2].position[0] - x0, by = t[2].position[1] - y0;
        const float det = ax * by - ay * bx;
        for (UINT row = 0; row < target_extent; ++row) {
            for (UINT col = 0; col < target_extent; ++col) {
                const float px = 2.0f * (float(col) + 0.5f) / target_extent - 1.0f - x0;
                const float py = 1.0f - 2.0f * (float(row) + 0.5f) / target_extent - y0;
                const float s = (px * by - py * bx) / det, r = (ax * py - ay * px) / det;
                const bool drawn = std::memcmp(&skew_pixels[row * target_extent + col], &clear_pixel, sizeof(Pixel)) != 0;
                skew_drawn += drawn;
                if (s > 0.02f && r > 0.02f && s + r < 0.98f) {
                    ++skew_inside;
                    skew_missing += !drawn;
                }
            }
        }
    }
    EXPECT(skew_inside > 1000 && skew_missing == 0);

    // Quad list: whole-bit comparison; two quadrants one colour each, the rest the clear colour.
    const Pixel clear_color = {0.5f, 0.125f, 0.25f, 0.5f};
    const Pixel top_left = {q1[0] * scale[0], q1[1] * scale[1], q1[2], q1[3]};
    const Pixel bottom_right = {q2[0] * scale[0], q2[1] * scale[1], q2[2], q2[3]};
    unsigned quad_wrong = 0;
    for (UINT row = 0; row < target_extent; ++row) {
        for (UINT col = 0; col < target_extent; ++col) {
            const bool top = row < target_extent / 2, left = col < target_extent / 2;
            const Pixel& want = top && left ? top_left : (!top && !left ? bottom_right : clear_color);
            const Pixel& got = quad_pixels[row * target_extent + col];
            if (std::memcmp(&got, &want, sizeof(Pixel)) == 0) continue;
            if (quad_wrong++ < 8)
                std::fprintf(stderr, "quad pixel (%u, %u): got %g,%g,%g,%g want %g,%g,%g,%g\n", col, row, got[0], got[1],
                             got[2], got[3], want[0], want[1], want[2], want[3]);
        }
    }
    EXPECT(!quad_wrong);
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("rect list: %u of %u pixels on the colour plane (worst error %g), all four quadrants; right angle at "
                "the second vertex %u and at the third %u of %u (worst error %g); no right angle: %u pixels drawn; "
                "quad list: %u of %u pixels exact\n",
                covered, target_extent * target_extent, double(worst), corner_covered[0], corner_covered[1],
                target_extent * target_extent, double(corner_worst), skew_drawn,
                target_extent * target_extent - quad_wrong, target_extent * target_extent);
    return 0;
}
