// Direct heap indexing (docs/d3d12_vulkan.md sections 3.4 and 3.5), compute (part 1) and graphics (part 2, see
// run_graphics). Part 1: a root signature with
// CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED makes the list write the bound CBV/SRV/UAV heap's base, the heap SetDescriptorHeaps
// bound, into the user SGPR right after the root arguments at each Dispatch and dispatch ExecuteIndirect. The shader is
// translator output (test/shaders/cs_heap_index.asm, hand-written RDNA2 in the shape of the game's heap-indexing
// shaders): it loads the V# at heap slot k (k a root constant, a run-time offset) and at heap slot 1 (a static offset)
// from that base and writes through both.
//
// One list, four dispatches, each of one 64-lane group:
//   1. Dispatch, heap A bound, k = 2: A[2] is buffer a2, A[1] buffer a1
//   2. Dispatch, heap B bound, k = 2: B[2] is buffer b2, B[1] buffer b1
//   3. Dispatch, heap A bound, k = 3: A[3] is buffer a3, A[1] buffer a1 again
//   4. ExecuteIndirect of one DISPATCH (1, 1, 1), heap B bound, k = 3: B[3] is buffer b3, B[1] buffer b1 again
// Every buffer starts as a sentinel and is compared word by word with the CPU reference. Negative control: dispatch 2
// differs from dispatch 1 only in the heap SetDescriptorHeaps bound, so its writes land in heap B's buffers only if
// the base the shader reads follows the binding; a base left at 0 or at the first heap would leave b2 and b1 at the
// sentinel (or write a2 and a1 again, which the reference checks too). The layouts of signatures without the flag,
// with the sampler flag and with both are checked through root_signature_layout.
// Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation (0 errors required).
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../src/shader_library.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#ifndef LEONIDA_TEST_HEAP_INDEX_DIR
#error "LEONIDA_TEST_HEAP_INDEX_DIR must name the folder build.py wrote (spirv/index.json, fssm/cs_heap_index.fssm)"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;

namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

constexpr UINT lanes = 64;
constexpr UINT64 bytes = lanes * 4;
constexpr std::uint32_t sentinel = 0x5e5e5e5eu;

enum Buf { a1, a2, a3, b1, b2, b3, buffer_count };

// A root signature of `constants` 32-bit constants with `flags`. The host serializer is given no flags (Windows 10's
// refuses the heap-indexing ones) and the flags word of the RTS0 1.0 header, its sixth dword, is written afterwards.
std::vector<std::uint8_t> root_signature(UINT flags, UINT constants = 1) {
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants = {0, 0, constants};
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    desc.Desc_1_0 = {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    std::vector<std::uint8_t> blob = serialize_root_signature(desc);
    if (blob.size() < 32 || std::memcmp(blob.data(), "DXBC", 4) != 0) return {};
    std::uint32_t parts = 0;
    std::memcpy(&parts, blob.data() + 28, 4);
    for (std::uint32_t i = 0; i < parts && 32 + 4 * (i + 1) <= blob.size(); ++i) {
        std::uint32_t at = 0;
        std::memcpy(&at, blob.data() + 32 + 4 * i, 4);
        if (std::uint64_t(at) + 8 + 24 > blob.size() || std::memcmp(blob.data() + at, "RTS0", 4) != 0) continue;
        std::memcpy(blob.data() + at + 8 + 20, &flags, 4);
        return blob;
    }
    return {};
}

bool create_device(vktest::Device& d, const std::filesystem::path& root_dir, Ref<ID3D12Device>& device) {
    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = d.instance_proc;
    info.instance = d.instance;
    info.physical = d.physical;
    info.device = d.device;
    info.queue = d.queue;
    info.queue_family = d.family;
    info.buffer_device_address = true;  // the V# holds the buffer's address
    info.bindless = d.bindless;
    info.bindless_partially_bound = d.partially_bound;
    info.image_view_min_lod = d.image_view_min_lod;
    info.shader_dir = (root_dir / "spirv").string();
    return SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&device.p)));
}

constexpr UINT res = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
constexpr UINT smp = D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
constexpr std::uint32_t none = rt::rootsig::kNoSgpr;

// The layouts a signature of one root constant gets at both bind points: compute from s4, graphics from s8, the heap
// bases right after, CBV/SRV/UAV first; no flag, no heap SGPR. A valid signature that spills at one bind point is
// still created and only that bind point is unusable.
int check_layouts(ID3D12Device* device) {
    const struct {
        UINT flags;
        std::uint32_t compute_res, compute_smp, graphics_res, graphics_smp, graphics_end;
    } layouts[] = {{0, none, none, none, none, 9},
                   {res, 5, none, 9, none, 10},
                   {smp, none, 5, none, 9, 10},
                   {res | smp, 5, 6, 9, 10, 11}};
    for (const auto& want : layouts) {
        const auto blob = root_signature(want.flags);
        EXPECT(!blob.empty());
        Ref<ID3D12RootSignature> signature;
        EXPECT(SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&signature.p))));
        const rt::d3d12::RootArgumentLayout* layout = rt::d3d12::root_signature_layout(signature.p);
        EXPECT(layout && layout->compute_ok && layout->graphics_ok && layout->flags == want.flags);
        EXPECT(layout->parameters.size() == 1 && layout->parameters[0].compute_offset == 4 &&
               layout->parameters[0].user_offset == 8);
        EXPECT(layout->compute_resource_heap_sgpr == want.compute_res);
        EXPECT(layout->compute_sampler_heap_sgpr == want.compute_smp);
        EXPECT(layout->graphics_resource_heap_sgpr == want.graphics_res);
        EXPECT(layout->graphics_sampler_heap_sgpr == want.graphics_smp);
        EXPECT(layout->graphics_end_sgpr == want.graphics_end);
    }
    // 25 constants fit compute (s4..s28) but not the graphics stages (s8..s32); 29 fit neither. Both are valid D3D12
    // signatures the console spills, so both are created.
    const struct {
        UINT constants;
        bool compute_ok, graphics_ok;
    } spills[] = {{24, true, true}, {25, true, false}, {28, true, false}, {29, false, false}};
    for (const auto& want : spills) {
        const auto blob = root_signature(0, want.constants);
        EXPECT(!blob.empty());
        Ref<ID3D12RootSignature> signature;
        EXPECT(SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&signature.p))));
        const rt::d3d12::RootArgumentLayout* layout = rt::d3d12::root_signature_layout(signature.p);
        EXPECT(layout && layout->compute_ok == want.compute_ok && layout->graphics_ok == want.graphics_ok);
    }
    return 0;
}

int run(vktest::Device& d, const std::filesystem::path& root_dir, unsigned& words_checked) {
    leonida::ShaderLibrary library;
    EXPECT(library.load((root_dir / "spirv").string()));
    const auto fssm = read_file(root_dir / "fssm" / "cs_heap_index.fssm");
    EXPECT(fssm.size() >= 0x150 && library.find("cs", leonida::sha256_hex(fssm)));
    const auto container = shader_container(fssm);
    Ref<ID3D12Device> device;
    EXPECT(create_device(d, root_dir, device));
    if (const int status = check_layouts(device.p)) return status;
    const auto root_blob = root_signature(res);
    Ref<ID3D12RootSignature> root;
    EXPECT(!root_blob.empty() &&
           SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.p;
    pso_desc.CS = {container.data(), container.size()};
    Ref<ID3D12PipelineState> pso;
    EXPECT(SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso.p))));

    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, upload_heap{D3D12_HEAP_TYPE_UPLOAD},
        readback_heap{D3D12_HEAP_TYPE_READBACK};
    Ref<ID3D12Resource> upload, readback, buf[buffer_count];
    const auto create = [&](Ref<ID3D12Resource>& out, const D3D12_HEAP_PROPERTIES& heap, UINT64 size,
                            D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
        const auto desc = buffer_desc(size, flags);
        return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                         IID_PPV_ARGS(&out.p)));
    };
    EXPECT(create(upload, upload_heap, bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ));
    EXPECT(create(readback, readback_heap, bytes * buffer_count, D3D12_RESOURCE_FLAG_NONE,
                  D3D12_RESOURCE_STATE_COPY_DEST));
    for (auto& b : buf)
        EXPECT(create(b, default_heap, bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_DEST));
    {
        std::uint8_t* mapped = nullptr;
        EXPECT(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))));
        for (UINT64 i = 0; i < bytes / 4; ++i) std::memcpy(mapped + i * 4, &sentinel, 4);
        upload->Unmap(0, nullptr);
    }

    // Two shader-visible heaps of four slots; slot 0 stays empty in both.
    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                               D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap_a, heap_b;
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap_a.p))));
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap_b.p))));
    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    EXPECT(step == 32);  // the shader steps 32 bytes a slot
    for (ID3D12DescriptorHeap* h : {heap_a.p, heap_b.p}) {
        const UINT64 gpu = h->GetGPUDescriptorHandleForHeapStart().ptr;
        EXPECT(gpu >= 0x10000 && gpu < (1ull << 32));  // a 32-bit pointer reaches it
    }
    EXPECT(heap_a->GetGPUDescriptorHandleForHeapStart().ptr != heap_b->GetGPUDescriptorHandleForHeapStart().ptr);
    const auto raw_view = [&](ID3D12DescriptorHeap* h, UINT index, Buf which) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer = {0, UINT(bytes / 4), 0, 0, D3D12_BUFFER_UAV_FLAG_RAW};
        D3D12_CPU_DESCRIPTOR_HANDLE at = h->GetCPUDescriptorHandleForHeapStart();
        at.ptr += SIZE_T(index) * step;
        device->CreateUnorderedAccessView(buf[which].p, nullptr, &view, at);
    };
    raw_view(heap_a.p, 1, a1);
    raw_view(heap_a.p, 2, a2);
    raw_view(heap_a.p, 3, a3);
    raw_view(heap_b.p, 1, b1);
    raw_view(heap_b.p, 2, b2);
    raw_view(heap_b.p, 3, b3);

    // The indirect arguments: one D3D12_DISPATCH_ARGUMENTS of (1, 1, 1), read from an upload buffer.
    const D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
    const D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &argument, 0};
    Ref<ID3D12CommandSignature> command_signature;
    EXPECT(SUCCEEDED(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&command_signature.p))));
    Ref<ID3D12Resource> arguments;
    EXPECT(create(arguments, upload_heap, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ));
    {
        D3D12_DISPATCH_ARGUMENTS* mapped = nullptr;
        EXPECT(SUCCEEDED(arguments->Map(0, nullptr, reinterpret_cast<void**>(&mapped))));
        *mapped = {1, 1, 1};
        arguments->Unmap(0, nullptr);
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                               IID_PPV_ARGS(&list.p))));
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

    constexpr auto uav_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_BARRIER barriers[buffer_count];
    for (auto& b : buf) list->CopyBufferRegion(b.p, 0, upload.p, 0, bytes);
    for (int i = 0; i < buffer_count; ++i) barriers[i] = transition(buf[i].p, D3D12_RESOURCE_STATE_COPY_DEST, uav_state);
    list->ResourceBarrier(buffer_count, barriers);
    list->SetComputeRootSignature(root.p);
    list->SetPipelineState(pso.p);
    const auto dispatch = [&](ID3D12DescriptorHeap* h, UINT k, bool indirect) {
        list->SetDescriptorHeaps(1, &h);
        list->SetComputeRoot32BitConstant(0, k, 0);
        if (indirect)
            list->ExecuteIndirect(command_signature.p, 1, arguments.p, 0, nullptr, 0);
        else
            list->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER all{};
        all.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;  // a1 and b1 are written twice
        list->ResourceBarrier(1, &all);
    };
    dispatch(heap_a.p, 2, false);
    dispatch(heap_b.p, 2, false);
    dispatch(heap_a.p, 3, false);
    dispatch(heap_b.p, 3, true);
    for (int i = 0; i < buffer_count; ++i) barriers[i] = transition(buf[i].p, uav_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(buffer_count, barriers);
    for (int i = 0; i < buffer_count; ++i) list->CopyBufferRegion(readback.p, bytes * i, buf[i].p, 0, bytes);
    EXPECT(list->Close() == S_OK);
    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);
    EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
    EXPECT(SUCCEEDED(queue->Signal(fence.p, 1)));
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    EXPECT(event != nullptr);
    EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, event)));
    if (WaitForSingleObject(event, 20000) != WAIT_OBJECT_0) {
        std::fprintf(stderr, "the fence never reached 1\n");
        return 1;
    }
    CloseHandle(event);

    std::vector<std::uint32_t> got(bytes / 4 * buffer_count);
    {
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
        std::memcpy(got.data(), mapped, got.size() * 4);
        readback->Unmap(0, nullptr);
    }
    // a1 and b1: the static slot; a2, b2: k = 2 through heaps A and B; a3, b3: k = 3 through heap A (Dispatch) and
    // heap B (ExecuteIndirect).
    const auto want = [](Buf which, UINT g) -> std::uint32_t {
        switch (which) {
            case a1:
            case b1:
                return 0xa5000000u + g;
            case a2:
            case b2:
                return 0x2000u + g;
            case a3:
            case b3:
                return 0x3000u + g;
            default:
                return sentinel;
        }
    };
    static const char* const names[buffer_count] = {"A[1]", "A[2]", "A[3]", "B[1]", "B[2]", "B[3]"};
    unsigned bad = 0;
    for (int i = 0; i < buffer_count; ++i)
        for (UINT g = 0; g < lanes; ++g) {
            const std::uint32_t have = got[i * lanes + g], expected = want(Buf(i), g);
            ++words_checked;
            if (have != expected && bad++ < 8)
                std::fprintf(stderr, "%s word %u: 0x%08x, expected 0x%08x\n", names[i], g, have, expected);
        }
    EXPECT(bad == 0);
    return 0;
}

// Part 2, graphics: the same signature (one root constant, CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED) on a graphics PSO of
// vs_m2.asm (a quad from the vertex index) and ps_heap_index.asm, which reads k at s8 and the heap base at s9, loads
// the raw buffer view at heap slot k and writes its four floats to every pixel of an RGBA32F target. Three draws,
// each copied out before the next: heap C with k = 2, heap D with k = 2, heap C with k = 3, each slot a view of a
// different four floats. Negative control as in part 1: the second draw differs from the first only in the heap bound.
int run_graphics(vktest::Device& d, const std::filesystem::path& root_dir, unsigned& texels_checked) {
    leonida::ShaderLibrary library;
    EXPECT(library.load((root_dir / "spirv").string()));
    const auto vs_program = read_file(root_dir / "fssm" / "vs_m2.fssm");
    const auto ps_program = read_file(root_dir / "fssm" / "ps_heap_index.fssm");
    EXPECT(library.find("vs", leonida::sha256_hex(vs_program)) && library.find("ps", leonida::sha256_hex(ps_program)));
    const auto vs_container = shader_container(vs_program), ps_container = shader_container(ps_program);
    Ref<ID3D12Device> device;
    EXPECT(create_device(d, root_dir, device));
    const auto root_blob = root_signature(res);
    Ref<ID3D12RootSignature> root;
    EXPECT(!root_blob.empty() &&
           SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));

    constexpr UINT size = 8;
    constexpr DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.p;
    pso_desc.VS = {vs_container.data(), vs_container.size()};
    pso_desc.PS = {ps_container.data(), ps_container.size()};
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = 0xffffffff;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = format;
    pso_desc.SampleDesc.Count = 1;
    Ref<ID3D12PipelineState> pso;
    EXPECT(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso.p))));

    // The four floats of each draw; the upload buffer also holds the six indices of the quad, at byte 256.
    constexpr float colors[3][4] = {{0.25f, 0.5f, 0.75f, 1.0f}, {2.0f, -1.0f, 0.125f, 0.5f}, {3.0f, 4.0f, 5.0f, 6.0f}};
    constexpr std::uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
    constexpr UINT64 index_offset = 256, pitch = 256, slice = pitch * size;
    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, upload_heap{D3D12_HEAP_TYPE_UPLOAD},
        readback_heap{D3D12_HEAP_TYPE_READBACK};
    Ref<ID3D12Resource> upload, data, readback, target;
    const auto buffer = [&](Ref<ID3D12Resource>& out, const D3D12_HEAP_PROPERTIES& heap, UINT64 bytes_,
                            D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
        const auto desc = buffer_desc(bytes_, flags);
        return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                         IID_PPV_ARGS(&out.p)));
    };
    EXPECT(buffer(upload, upload_heap, 512, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ));
    EXPECT(buffer(data, default_heap, sizeof(colors), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_COPY_DEST));
    EXPECT(buffer(readback, readback_heap, slice * 3, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST));
    {
        std::uint8_t* mapped = nullptr;
        EXPECT(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))));
        std::memcpy(mapped, colors, sizeof(colors));
        std::memcpy(mapped + index_offset, indices, sizeof(indices));
        upload->Unmap(0, nullptr);
    }
    D3D12_RESOURCE_DESC target_desc{};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = size;
    target_desc.Height = size;
    target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1;
    target_desc.Format = format;
    target_desc.SampleDesc.Count = 1;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                                     D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                     IID_PPV_ARGS(&target.p))));

    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                               D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap_c, heap_d, rtv_heap;
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap_c.p))));
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap_d.p))));
    const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap.p))));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.p, nullptr, rtv);
    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto raw_view = [&](ID3D12DescriptorHeap* h, UINT index, UINT color) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer = {UINT64(color) * 4, 4, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW};
        D3D12_CPU_DESCRIPTOR_HANDLE at = h->GetCPUDescriptorHandleForHeapStart();
        at.ptr += SIZE_T(index) * step;
        device->CreateUnorderedAccessView(data.p, nullptr, &view, at);
    };
    raw_view(heap_c.p, 2, 0);
    raw_view(heap_d.p, 2, 1);
    raw_view(heap_c.p, 3, 2);

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pso.p,
                                               IID_PPV_ARGS(&list.p))));
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

    list->CopyBufferRegion(data.p, 0, upload.p, 0, sizeof(colors));
    D3D12_RESOURCE_BARRIER to_read = transition(data.p, D3D12_RESOURCE_STATE_COPY_DEST,
                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &to_read);
    list->SetGraphicsRootSignature(root.p);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_INDEX_BUFFER_VIEW ibv{upload->GetGPUVirtualAddress() + index_offset, sizeof(indices),
                                      DXGI_FORMAT_R16_UINT};
    list->IASetIndexBuffer(&ibv);
    const D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const FLOAT clear[4] = {-7.0f, -7.0f, -7.0f, -7.0f};  // a pixel the draw missed shows
    const auto draw = [&](ID3D12DescriptorHeap* h, UINT k, UINT n) {
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->SetDescriptorHeaps(1, &h);
        list->SetGraphicsRoot32BitConstant(0, k, 0);
        list->DrawIndexedInstanced(6, 1, 0, 0, 0);
        D3D12_RESOURCE_BARRIER b = transition(target.p, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                              D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.p;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = {slice * n, {format, size, size, 1, UINT(pitch)}};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = target.p;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
    };
    draw(heap_c.p, 2, 0);
    draw(heap_d.p, 2, 1);
    draw(heap_c.p, 3, 2);
    EXPECT(list->Close() == S_OK);
    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);
    EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
    EXPECT(SUCCEEDED(queue->Signal(fence.p, 1)));
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    EXPECT(event != nullptr);
    EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, event)));
    if (WaitForSingleObject(event, 20000) != WAIT_OBJECT_0) {
        std::fprintf(stderr, "the fence never reached 1\n");
        return 1;
    }
    CloseHandle(event);

    std::vector<std::uint8_t> got(slice * 3);
    {
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
        std::memcpy(got.data(), mapped, got.size());
        readback->Unmap(0, nullptr);
    }
    static const char* const names[3] = {"C[2]", "D[2]", "C[3]"};
    unsigned bad = 0;
    for (UINT n = 0; n < 3; ++n)
        for (UINT y = 0; y < size; ++y)
            for (UINT x = 0; x < size; ++x) {
                float texel[4];
                std::memcpy(texel, got.data() + slice * n + pitch * y + 16 * x, sizeof(texel));
                ++texels_checked;
                if (std::memcmp(texel, colors[n], sizeof(texel)) != 0 && bad++ < 8)
                    std::fprintf(stderr, "%s pixel (%u, %u): %g %g %g %g, expected %g %g %g %g\n", names[n], x, y,
                                 texel[0], texel[1], texel[2], texel[3], colors[n][0], colors[n][1], colors[n][2],
                                 colors[n][3]);
            }
    EXPECT(bad == 0);
    return 0;
}

}  // namespace

int main() {
    vktest::Device d;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) return 77;   // no Vulkan GPU: ctest skips
    if (device_status != vktest::DeviceStatus::ready) return 1;
    unsigned words = 0;
    if (const int status = run(d, LEONIDA_TEST_HEAP_INDEX_DIR, words)) return status;
    unsigned texels = 0;
    if (const int status = run_graphics(d, LEONIDA_TEST_HEAP_INDEX_DIR, texels)) return status;
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("d3d12_heap_index: %u words match through heap-indexed V#s (two heaps, run-time and static slots, "
                "Dispatch and ExecuteIndirect), and %u texels drawn through them by a pixel shader\n",
                words, texels);
    return 0;
}
