// A buffer the title places at its own VA (Xbox device slot 133, rt::d3d12::create_buffer_at_host_address) is read
// by a shader written to the translator's address contract (heap_base + console VA, docs/d3d12_vulkan.md section
// 3.3), through the two kinds of address the layer writes for it:
//   - a root view (SetComputeRootUnorderedAccessView with the title's VA): read 256 bytes past its base and written
//     at its base, then copied to a READBACK buffer;
//   - a buffer V# (CreateShaderResourceView of the title's buffer), the way translated vertex shaders fetch their
//     vertices: read and written to an arena buffer.
// The title's VA is a VirtualAlloc address, far past the 4 GiB arena. Both page kinds are covered: read-write pages
// (imported with VK_EXT_external_memory_host where the driver takes them) and write-combined pages (refused by the
// import, so a shadow copy in the arena, the retail case on an RTX 5070 Ti). The data is written after the buffer
// exists, so the shadow's copy before the submission is what the GPU reads.
// Then the title's own memory outside any slot-133 buffer (a constant ring, #8862 section 3.5): a V# the CPU writes
// into a bound table itself, a created CBV and a root view.
// Negative controls: a second title buffer with other data must not be what the first one's V# reads; an arena
// buffer's V# keeps its VA; the title's range is forgotten when the buffer is released; a raw arena V# and a
// T#-shaped slot in a scanned table keep their bytes; a reserved, uncommitted address is left alone. The shader is test/glsl/cs_user_data.comp. No game data. LEONIDA_VK_VALIDATION=1 for the
// Khronos validation layer.
#include "shader_fixture_path.h"
#include "test_temp_dir.h"
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/gfx10_descriptors.h"
#include "../src/shader_library.h"
#include "../src/title_va_alias.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#ifndef LEONIDA_TEST_CS_SPV
#error "LEONIDA_TEST_CS_SPV must name the compiled test shader"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slots 9/10: handle in RAX
using XboxComputePipelineFn = HRESULT(WINAPI*)(void* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, std::uint64_t,
                                               std::uint64_t, const IID*, void**);
using SerializeFn = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);

template<class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T* operator->() const { return p; }
};

void* slot(void* object, unsigned index) { return (*reinterpret_cast<void***>(object))[index]; }

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i)));
}

std::vector<std::uint8_t> zlib_stored(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out = {0x78, 0x01, 0x01};
    const auto length = std::uint16_t(data.size());
    out.insert(out.end(), {std::uint8_t(length), std::uint8_t(length >> 8), std::uint8_t(~length),
                           std::uint8_t(~length >> 8)});
    out.insert(out.end(), data.begin(), data.end());
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t x : data) {
        a = (a + x) % 65521;
        b = (b + a) % 65521;
    }
    const std::uint32_t adler = (b << 16) | a;
    out.insert(out.end(), {std::uint8_t(adler >> 24), std::uint8_t(adler >> 16), std::uint8_t(adler >> 8),
                           std::uint8_t(adler)});
    return out;
}

// The game's shader container shape (DXBC > DXIL > XDAT 0x0D > XBCH > zlib > FSSM), as d3d12_compute_test builds it.
std::vector<std::uint8_t> shader_container(const std::vector<std::uint8_t>& fssm) {
    const auto stream = zlib_stored(fssm);
    std::vector<std::uint8_t> xbch = {'X', 'B', 'C', 'H'};
    for (const std::uint32_t v : {1u, std::uint32_t(stream.size()), 0u, std::uint32_t(fssm.size()), 0u}) put32(xbch, v);
    xbch.insert(xbch.end(), stream.begin(), stream.end());
    std::vector<std::uint8_t> dxil(16, 0);
    dxil.insert(dxil.end(), {'X', 'D', 'A', 'T'});
    put32(dxil, 1);
    put32(dxil, 12);
    put32(dxil, 0x0d);
    put32(dxil, 16 + 12 + 12);
    put32(dxil, std::uint32_t(xbch.size()));
    dxil.insert(dxil.end(), xbch.begin(), xbch.end());
    std::vector<std::uint8_t> out = {'D', 'X', 'B', 'C'};
    out.resize(28, 0);
    put32(out, 1);
    put32(out, 36);
    out.insert(out.end(), {'D', 'X', 'I', 'L'});
    put32(out, std::uint32_t(dxil.size()));
    out.insert(out.end(), dxil.begin(), dxil.end());
    return out;
}

std::vector<std::uint8_t> serialize(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc) {
    const HMODULE d3d12 = LoadLibraryExA("d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto fn = d3d12 ? reinterpret_cast<SerializeFn>(
                                reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature")))
                          : nullptr;
    std::vector<std::uint8_t> out;
    ID3DBlob* blob = nullptr;
    if (fn && SUCCEEDED(fn(&desc, &blob, nullptr)) && blob) {
        const auto* p = static_cast<const std::uint8_t*>(blob->GetBufferPointer());
        out.assign(p, p + blob->GetBufferSize());
        blob->Release();
    }
    return out;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

// The 48-bit base address of the 16-byte GFX10 buffer V# at `cpu` (dword 0, and dword 1 bits 0-15).
std::uint64_t vsharp_base(std::uint64_t cpu) {
    std::uint32_t w[2];
    std::memcpy(w, reinterpret_cast<const void*>(cpu), sizeof(w));
    return std::uint64_t(w[0]) | (std::uint64_t(w[1] & 0xFFFFu) << 32);
}

D3D12_SHADER_RESOURCE_VIEW_DESC raw_srv(UINT first_word, UINT words) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.FirstElement = first_word;
    desc.Buffer.NumElements = words;
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    return desc;
}

constexpr UINT kElements = 64, kBytes = kElements * 4;
constexpr UINT64 kPages = 64 * 1024;
constexpr UINT kSrvWord = 256;   // the V# reads title words 256..319
constexpr UINT kRootAdd = 7;     // root view mode: title word i = title word 64 + i + 7

struct Context {
    ID3D12Device* inner = nullptr;  // the Vulkan device (slot 133's backend takes it)
    ID3D12Device* device = nullptr;  // the Xbox-ABI proxy over it
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fence_value = 0;
    std::uint64_t arena = 0;
};

// One kind of title pages: `protect` is PAGE_READWRITE, or with PAGE_WRITECOMBINE.
int check_kind(Context& c, const char* kind, DWORD protect, int& checks) {
    void* pages = VirtualAlloc(nullptr, kPages, MEM_RESERVE | MEM_COMMIT, protect);
    void* other_pages = VirtualAlloc(nullptr, kPages, MEM_RESERVE | MEM_COMMIT, protect);
    EXPECT(pages && other_pages);
    const auto title_va = reinterpret_cast<std::uint64_t>(pages);
    EXPECT(title_va >= c.arena);  // outside the arena: heap_base + title_va names nothing of the layer's
    const D3D12_RESOURCE_DESC desc = buffer_desc(kPages);
    Ref<ID3D12Resource> title, other;
    EXPECT(SUCCEEDED(rt::d3d12::create_buffer_at_host_address(c.inner, pages, desc, 0xFFFFFFFFu,
                                                              IID_PPV_ARGS(&title.p))));
    EXPECT(SUCCEEDED(rt::d3d12::create_buffer_at_host_address(c.inner, other_pages, desc, 0xFFFFFFFFu,
                                                              IID_PPV_ARGS(&other.p))));
    EXPECT(title->GetGPUVirtualAddress() == title_va);  // the title keeps its own VA
    const std::uint64_t shader_va = leonida::title_va_aliases().shader_va(title_va);
    EXPECT(shader_va != title_va);
    const bool arena_copy = shader_va < c.arena;
    const bool vsharp_fits = (shader_va >> 48) == 0;

    // The data, written after the buffers exist.
    auto* words = static_cast<std::uint32_t*>(pages);
    auto* other_words = static_cast<std::uint32_t*>(other_pages);
    for (UINT i = 0; i < kPages / 4; ++i) {
        words[i] = 0xA0000000u + i * 3u;
        other_words[i] = 0xB0000000u + i * 5u;
    }

    // Descriptors: the title buffer's V# (and the other buffer's), and the negative controls.
    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
                                               D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(c.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p))));
    const std::uint64_t cpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 9))(heap.p);
    const std::uint64_t gpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 10))(heap.p);
    const UINT inc = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_SHADER_RESOURCE_VIEW_DESC srv = raw_srv(kSrvWord, kElements);
    c.device->CreateShaderResourceView(title.p, &srv, D3D12_CPU_DESCRIPTOR_HANDLE{cpu});
    c.device->CreateShaderResourceView(other.p, &srv, D3D12_CPU_DESCRIPTOR_HANDLE{cpu + inc});
    EXPECT(vsharp_base(cpu) == ((shader_va + kSrvWord * 4) & 0xFFFFFFFFFFFFull));

    // An arena buffer for the V# read's result, a READBACK buffer for both results.
    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, readback_heap{D3D12_HEAP_TYPE_READBACK};
    Ref<ID3D12Resource> out, readback;
    const auto uav = buffer_desc(kBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&out.p))));
    const auto readback_desc = buffer_desc(3 * kBytes);
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&readback.p))));
    // An arena buffer's V# keeps its VA: identity outside title-placed buffers.
    const D3D12_CONSTANT_BUFFER_VIEW_DESC arena_cbv{out->GetGPUVirtualAddress(), 256};
    c.device->CreateConstantBufferView(&arena_cbv, D3D12_CPU_DESCRIPTOR_HANDLE{cpu + 3 * inc});
    EXPECT(vsharp_base(cpu + 3 * inc) == out->GetGPUVirtualAddress());

    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, c.pso,
                                                 IID_PPV_ARGS(&list.p))));
    ID3D12DescriptorHeap* heaps[] = {heap.p};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(c.root);
    // Through the V# (buffer SRV mode, s5 = 0xFFFFFFFF), into the arena buffer. Skipped when the buffer's copy is
    // not within a V#'s 48 bits of heap_base (an import the driver put below the arena), which the layer logs.
    if (vsharp_fits) {
        list->SetComputeRoot32BitConstant(0, 0, 0);
        list->SetComputeRoot32BitConstant(0, 0xFFFFFFFFu, 1);
        list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
        list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu});
        list->Dispatch(1, 1, 1);
    }
    // Through the root view: the title's own VA, read at +256 bytes and written at +0.
    list->SetComputeRoot32BitConstant(0, kRootAdd, 0);
    list->SetComputeRoot32BitConstant(0, 0xFFFFFFFEu, 1);
    list->SetComputeRootUnorderedAccessView(1, title_va);
    list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &barrier);
    D3D12_RESOURCE_BARRIER to_copy{};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = out.p;
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &to_copy);
    list->CopyBufferRegion(readback.p, 0, out.p, 0, kBytes);
    list->CopyBufferRegion(readback.p, kBytes, title.p, 0, kBytes);
    EXPECT(list->Close() == S_OK);
    ID3D12CommandList* lists[] = {list.p};
    c.queue->ExecuteCommandLists(1, lists);
    EXPECT(rt::d3d12::last_execute_result(c.queue) == leonida::ReplayResult::success);
    EXPECT(SUCCEEDED(c.queue->Signal(c.fence, ++c.fence_value)));
    EXPECT(SUCCEEDED(c.fence->SetEventOnCompletion(c.fence_value, nullptr)));

    void* mapped = nullptr;
    EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
    const auto* got = static_cast<const std::uint32_t*>(mapped);
    UINT srv_ok = 0, srv_other = 0, root_ok = 0;
    for (UINT i = 0; i < kElements; ++i) {
        if (got[i] == words[kSrvWord + i]) ++srv_ok;
        if (got[i] == other_words[kSrvWord + i]) ++srv_other;
        if (got[kElements + i] == words[64 + i] + kRootAdd) ++root_ok;
    }
    readback->Unmap(0, nullptr);
    std::printf("%s pages: GPU copy %s (shader VA %s); V# read %s, %u/%u words matched, %u/%u read the other "
                "buffer's (negative control); root view read+write %u/%u matched\n",
                kind, arena_copy ? "in the arena" : "with its own device address",
                arena_copy ? "an arena offset" : "device address - heap_base",
                vsharp_fits ? "run" : "skipped (out of a V#'s 48 bits)", vsharp_fits ? srv_ok : 0, kElements,
                vsharp_fits ? srv_other : 0, kElements, root_ok, kElements);
    if (vsharp_fits) {
        EXPECT(srv_ok == kElements);
        EXPECT(srv_other == 0);
    }
    EXPECT(root_ok == kElements);

    // After release, the buffer's range is gone from the table: its old copy is not named any more.
    title.reset();
    EXPECT(leonida::title_va_aliases().shader_va(title_va) == title_va);
    checks += 2;
    other.reset();
    heap.reset();
    VirtualFree(pages, 0, MEM_RELEASE);
    VirtualFree(other_pages, 0, MEM_RELEASE);
    return 0;
}

// The title's own memory outside any slot-133 buffer: a VirtualAlloc range standing for its per-frame constant ring
// (#8862 section 3.5). Three ways a shader is given an address in it, each read back through an arena buffer:
//   - a buffer V# the CPU writes into a shader-visible heap itself, as the title does (rewritten before the submission);
//   - a V# made by CreateConstantBufferView (translated when written);
//   - a root view (translated when set).
// Negative controls: a raw V# whose base is an arena buffer and a T#-shaped slot whose base is the ring keep their
// bytes; committed memory that is not the title's, and a title range with no committed page, are left alone.
int check_title_memory(Context& c, int& checks) {
    constexpr UINT64 ring_bytes = 256 * 1024;
    auto* ring = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, ring_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    EXPECT(ring);
    const auto ring_va = reinterpret_cast<std::uint64_t>(ring);
    EXPECT(ring_va >= c.arena);
    auto& aliases = leonida::title_va_aliases();
    aliases.add_title_memory(ring_va, ring_bytes);  // as the xmem shims record what they hand the title
    const std::uint64_t table_before = aliases.translated(leonida::TitleVaAliases::table_view);
    const std::uint64_t created_before = aliases.translated(leonida::TitleVaAliases::created_view);
    const std::uint64_t root_before = aliases.translated(leonida::TitleVaAliases::root_view);

    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, readback_heap{D3D12_HEAP_TYPE_READBACK};
    const auto uav = buffer_desc(kBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Ref<ID3D12Resource> out[3], readback;
    for (auto& o : out)
        EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav,
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                           IID_PPV_ARGS(&o.p))));
    const auto readback_desc = buffer_desc(3 * kBytes);
    EXPECT(SUCCEEDED(c.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&readback.p))));

    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
                                               D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> heap;
    EXPECT(SUCCEEDED(c.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.p))));
    const std::uint64_t cpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 9))(heap.p);
    const std::uint64_t gpu = reinterpret_cast<XboxHeapHandleFn>(slot(heap.p, 10))(heap.p);
    const UINT inc = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto raw_vsharp = [](std::uint64_t address) {
        leonida::gfx10::Buffer b{};
        b.address = address;
        b.num_records = kBytes;
        b.format = leonida::gfx10::format_from_vulkan(VK_FORMAT_R32_UINT);
        b.oob_select = leonida::gfx10::OobSelect::raw;
        return leonida::gfx10::encode(b);
    };
    // Slot 0: a V# the CPU writes itself, its base in the ring. Slot 1: CreateConstantBufferView of the ring.
    // Slot 2: a raw 64-bit VA (out[2]) for the root-view mode. Slots 3 and 4: the negative controls.
    const auto v0 = raw_vsharp(ring_va + 0x1000);
    std::memcpy(reinterpret_cast<void*>(cpu), v0.data(), sizeof(v0));
    const D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{ring_va + 0x2000, 256};
    c.device->CreateConstantBufferView(&cbv, D3D12_CPU_DESCRIPTOR_HANDLE{cpu + inc});
    EXPECT(vsharp_base(cpu + inc) < c.arena);  // translated when written
    const std::uint64_t out2 = out[2]->GetGPUVirtualAddress();
    std::memcpy(reinterpret_cast<void*>(cpu + 2 * inc), &out2, sizeof(out2));
    const auto arena_vsharp = raw_vsharp(out[0]->GetGPUVirtualAddress());
    std::memcpy(reinterpret_cast<void*>(cpu + 3 * inc), arena_vsharp.data(), sizeof(arena_vsharp));
    auto image_like = raw_vsharp(ring_va + 0x4000);
    image_like[3] |= 0x8u << 28;  // TYPE: an image
    std::memcpy(reinterpret_cast<void*>(cpu + 4 * inc), image_like.data(), sizeof(image_like));
    std::uint8_t before3[32], before4[32];
    std::memcpy(before3, reinterpret_cast<const void*>(cpu + 3 * inc), 32);
    std::memcpy(before4, reinterpret_cast<const void*>(cpu + 4 * inc), 32);

    // The ring's data, written before the submission (after the descriptors, as a title fills its ring per frame).
    for (UINT i = 0; i < ring_bytes / 4; ++i) ring[i] = 0xC0000000u + i * 7u;

    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    EXPECT(SUCCEEDED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, c.pso,
                                                 IID_PPV_ARGS(&list.p))));
    ID3D12DescriptorHeap* heaps[] = {heap.p};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(c.root);
    // The CPU-written V# (buffer SRV mode).
    list->SetComputeRoot32BitConstant(0, 0, 0);
    list->SetComputeRoot32BitConstant(0, 0xFFFFFFFFu, 1);
    list->SetComputeRootUnorderedAccessView(1, out[0]->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu});
    list->Dispatch(1, 1, 1);
    // The created CBV (CBV mode).
    list->SetComputeRoot32BitConstant(0, 0, 1);
    list->SetComputeRootUnorderedAccessView(1, out[1]->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu + inc});
    list->Dispatch(1, 1, 1);
    // A root view into the ring, read 256 bytes past its base, written through slot 2's raw VA.
    list->SetComputeRoot32BitConstant(0, kRootAdd, 0);
    list->SetComputeRoot32BitConstant(0, 0xFFFFFFFDu, 1);
    list->SetComputeRootUnorderedAccessView(1, ring_va + 0x3000);
    list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu + 2 * inc});
    list->Dispatch(1, 1, 1);
    // The negative-control slots are bound too, so the scan reads them.
    list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu + 3 * inc});
    list->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu + 4 * inc});
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &barrier);
    for (UINT k = 0; k < 3; ++k) {
        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource = out[k].p;
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &to_copy);
        list->CopyBufferRegion(readback.p, k * kBytes, out[k].p, 0, kBytes);
    }
    EXPECT(list->Close() == S_OK);
    ID3D12CommandList* lists[] = {list.p};
    c.queue->ExecuteCommandLists(1, lists);
    EXPECT(rt::d3d12::last_execute_result(c.queue) == leonida::ReplayResult::success);
    EXPECT(SUCCEEDED(c.queue->Signal(c.fence, ++c.fence_value)));
    EXPECT(SUCCEEDED(c.fence->SetEventOnCompletion(c.fence_value, nullptr)));

    void* mapped = nullptr;
    EXPECT(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
    const auto* got = static_cast<const std::uint32_t*>(mapped);
    UINT raw_ok = 0, cbv_ok = 0, root_ok = 0;
    for (UINT i = 0; i < kElements; ++i) {
        if (got[i] == ring[0x1000 / 4 + i]) ++raw_ok;
        if (got[kElements + i] == ring[0x2000 / 4 + i]) ++cbv_ok;
        if (got[2 * kElements + i] == ring[0x3000 / 4 + 64 + i] + kRootAdd) ++root_ok;
    }
    readback->Unmap(0, nullptr);
    const bool rewritten = vsharp_base(cpu) < c.arena;
    const bool arena_kept = !std::memcmp(before3, reinterpret_cast<const void*>(cpu + 3 * inc), 32);
    const bool image_kept = !std::memcmp(before4, reinterpret_cast<const void*>(cpu + 4 * inc), 32);
    std::printf("title memory (a constant ring): CPU-written table V# %u/%u (rewritten before the submission: %s), "
                "created CBV %u/%u, root view %u/%u; negative controls: arena V# kept %s, T#-shaped slot kept %s; "
                "translated: %llu table V#, %llu created, %llu root\n",
                raw_ok, kElements, rewritten ? "yes" : "no", cbv_ok, kElements, root_ok, kElements,
                arena_kept ? "yes" : "no", image_kept ? "yes" : "no",
                static_cast<unsigned long long>(aliases.translated(leonida::TitleVaAliases::table_view) - table_before),
                static_cast<unsigned long long>(aliases.translated(leonida::TitleVaAliases::created_view) -
                                                created_before),
                static_cast<unsigned long long>(aliases.translated(leonida::TitleVaAliases::root_view) - root_before));
    EXPECT(raw_ok == kElements && cbv_ok == kElements && root_ok == kElements);
    EXPECT(rewritten && arena_kept && image_kept);
    // Left alone and counted: committed memory that is not the title's (the runtime's own, say), and a title range
    // with no committed page.
    const std::uint64_t untranslated_before = aliases.untranslated(leonida::TitleVaAliases::created_view);
    void* host = VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void* gone = VirtualAlloc(nullptr, 65536, MEM_RESERVE, PAGE_READWRITE);
    const auto host_va = reinterpret_cast<std::uint64_t>(host), gone_va = reinterpret_cast<std::uint64_t>(gone);
    aliases.add_title_memory(gone_va, 65536);
    const D3D12_CONSTANT_BUFFER_VIEW_DESC not_title{host_va, 256}, reserved_only{gone_va, 256};
    c.device->CreateConstantBufferView(&not_title, D3D12_CPU_DESCRIPTOR_HANDLE{cpu + 5 * inc});
    c.device->CreateConstantBufferView(&reserved_only, D3D12_CPU_DESCRIPTOR_HANDLE{cpu + 6 * inc});
    EXPECT(vsharp_base(cpu + 5 * inc) == (host_va & 0xFFFFFFFFFFFFull));
    EXPECT(vsharp_base(cpu + 6 * inc) == (gone_va & 0xFFFFFFFFFFFFull));
    EXPECT(aliases.untranslated(leonida::TitleVaAliases::created_view) - untranslated_before == 2);
    aliases.remove_title_memory(gone_va);
    aliases.remove_title_memory(ring_va);
    VirtualFree(host, 0, MEM_RELEASE);
    VirtualFree(gone, 0, MEM_RELEASE);
    checks += 4;
    return 0;
}

}  // namespace

int main() {
    vktest::Device d;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) {
        std::fprintf(stderr, "SKIP: no usable Vulkan 1.3 device\n");
        return 77;
    }
    EXPECT(device_status == vktest::DeviceStatus::ready);

    std::vector<std::uint8_t> fssm = {'F', 'S', 'S', 'M'};
    put32(fssm, 6000);
    for (std::uint32_t i = 0; i < 14; ++i) put32(fssm, 0xbf810000u + i);
    const auto container = shader_container(fssm);
    const testfiles::ScratchDir scratch("leonida_title_va_test");
    const auto& library = scratch.path();
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(LEONIDA_TEST_CS_SPV), library / "cs_user_data.spv"));
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"cs_user_data\": {\"stage\": \"cs\", \"fssm_sha256\": \"" << leonida::sha256_hex(fssm)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"local_size\": [64, 1, 1], "
                 "\"uses_code_base\": true, \"wave64\": false}}";
    }

    int checks = 0;
    {
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        info.subgroup = d.subgroup;
        info.shader_dir = library.string();
        ID3D12Device* inner = nullptr;  // owned by the proxy, as in d3d12_compute_test
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&device.p))));
        rt::d3d12::set_device_outer(inner, device.p);

        // [0] two constants (s4, s5), [1] a root UAV (s6:s7), [2] a one-descriptor table (s8): the console compute
        // layout cs_user_data.comp reads.
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1, 0, 0};
        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 2};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor = {0, 0};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {1, &range};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        const auto root_blob = serialize(root_desc);
        EXPECT(!root_blob.empty());
        Ref<ID3D12RootSignature> root;
        EXPECT(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root.p;
        pso_desc.CS = {container.data(), container.size()};
        const IID pso_iid = __uuidof(ID3D12PipelineState);
        Ref<ID3D12PipelineState> pso;
        EXPECT(SUCCEEDED(reinterpret_cast<XboxComputePipelineFn>(slot(device.p, 62))(
            device.p, &pso_desc, 0, 0, &pso_iid, reinterpret_cast<void**>(&pso.p))));

        const D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Ref<ID3D12CommandQueue> queue;
        Ref<ID3D12Fence> fence;
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));

        Context c;
        c.inner = inner;
        c.device = device.p;
        c.root = root.p;
        c.pso = pso.p;
        c.queue = queue.p;
        c.fence = fence.p;
        c.arena = rt::d3d12::device_arena_size(device.p);
        EXPECT(c.arena > 0);
        if (check_kind(c, "read-write", PAGE_READWRITE, checks)) return 1;
        if (check_kind(c, "write-combined", PAGE_READWRITE | PAGE_WRITECOMBINE, checks)) return 1;
        if (check_title_memory(c, checks)) return 1;
    }
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("title-placed buffers: a translated shader reached both page kinds through a root view and a buffer "
                "V#; %d descriptor-level negative controls held\n",
                checks);
    return 0;
}
