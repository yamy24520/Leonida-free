// DXGI_FORMAT_A8_UNORM textures in 1D and 3D. The host makes them (a title's boot sizes a 128-texel 1D one through
// GetResourceAllocationInfo); the layer refused every A8_UNORM texture that was not 2D. They are R8_UNORM images, like
// the 2D ones, and their SRVs read (0, 0, 0, A).
//
// Exercises, for a 1D (128) and a 3D (8x8x4) texture:
//   1. CheckFeatureSupport advertises TEXTURE1D and TEXTURE3D for A8_UNORM
//   2. GetResourceAllocationInfo gives a size and CreateCommittedResource makes the texture
//   3. Bytes uploaded with CopyTextureRegion come back unchanged
//   4. The default SRV is a T# of the right image type, in R8_UNORM's GFX10 format, with the swizzle (0, 0, 0, X)
//   5. Negative controls: unordered access and depth-stencil are still refused in both dimensions
//
// Synthetic data only; no game files. Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation.
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include "../src/gfx10_descriptors.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

namespace g = leonida::gfx10;

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

D3D12_RESOURCE_DESC a8_desc(D3D12_RESOURCE_DIMENSION dimension, UINT64 width, UINT height, UINT16 depth,
                            D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = dimension;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = depth;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_A8_UNORM;
    d.SampleDesc = {1, 0};
    d.Flags = flags;
    return d;
}

D3D12_RESOURCE_DESC buf_desc(UINT64 bytes) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return b;
}

std::uint8_t texel(UINT x, UINT y, UINT z) { return std::uint8_t(1 + x + 7 * y + 61 * z); }

}  // namespace

int main() {
    vktest::Device d;
    const auto status = d.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    if (status != vktest::DeviceStatus::ready) return 1;

    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = d.instance_proc;
    info.instance = d.instance;
    info.physical = d.physical;
    info.device = d.device;
    info.queue = d.queue;
    info.queue_family = d.family;
    info.bindless = d.bindless;
    info.bindless_partially_bound = d.partially_bound;
    info.image_view_min_lod = d.image_view_min_lod;
    info.host_image_copy = d.host_image_copy;
    Ref<ID3D12Device> device;
    EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&device.p))));

    // 1. Format support names both dimensions, as the host's does.
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_A8_UNORM};
    EXPECT(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))));
    EXPECT((support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE1D) && (support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) &&
           (support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE3D));

    Ref<ID3D12CommandQueue> queue;
    const D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue.p))));
    Ref<ID3D12CommandAllocator> alloc;
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc.p))));
    Ref<ID3D12Fence> fence;
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
    Ref<ID3D12DescriptorHeap> cpu_heap;
    const D3D12_DESCRIPTOR_HEAP_DESC cpu_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&cpu_desc, IID_PPV_ARGS(&cpu_heap.p))));
    const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    EXPECT(step >= sizeof(g::ImageWords));

    const D3D12_HEAP_PROPERTIES def_heap{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_HEAP_PROPERTIES up_heap{D3D12_HEAP_TYPE_UPLOAD};
    const D3D12_HEAP_PROPERTIES rb_heap{D3D12_HEAP_TYPE_READBACK};
    const std::uint32_t r8_code = g::format_from_vulkan(VK_FORMAT_R8_UNORM);
    EXPECT(r8_code != 0);

    struct Case {
        const char* name;
        D3D12_RESOURCE_DESC desc;
        g::ImageType type;
    };
    const Case cases[] = {
        {"1D", a8_desc(D3D12_RESOURCE_DIMENSION_TEXTURE1D, 128, 1, 1), g::ImageType::image_1d},
        {"3D", a8_desc(D3D12_RESOURCE_DIMENSION_TEXTURE3D, 8, 8, 4), g::ImageType::image_3d},
    };
    UINT64 fence_value = 0;
    for (UINT c = 0; c < 2; ++c) {
        const Case& k = cases[c];
        const UINT width = UINT(k.desc.Width), height = k.desc.Height;
        const UINT depth = k.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? k.desc.DepthOrArraySize : 1u;

        // 5. Negative controls first: the flags A8_UNORM never takes stay refused in this dimension.
        for (auto flag : {D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL}) {
            const auto refused = a8_desc(k.desc.Dimension, k.desc.Width, k.desc.Height, k.desc.DepthOrArraySize, flag);
            EXPECT(device->GetResourceAllocationInfo(0, 1, &refused).SizeInBytes == UINT64_MAX);
        }

        // 2. A size and a texture.
        const auto alloc_info = device->GetResourceAllocationInfo(0, 1, &k.desc);
        EXPECT(alloc_info.SizeInBytes != UINT64_MAX && alloc_info.SizeInBytes >= UINT64(width) * height * depth);
        Ref<ID3D12Resource> tex;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&def_heap, D3D12_HEAP_FLAG_NONE, &k.desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&tex.p))));

        // 3. Upload, copy back, compare.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        device->GetCopyableFootprints(&k.desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        EXPECT(fp.Footprint.Format == DXGI_FORMAT_A8_UNORM && fp.Footprint.Width == width &&
               fp.Footprint.Height == height && fp.Footprint.Depth == depth && total != UINT64_MAX);
        const auto staging_desc = buf_desc(total);
        Ref<ID3D12Resource> upload, readback;
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&up_heap, D3D12_HEAP_FLAG_NONE, &staging_desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&upload.p))));
        EXPECT(SUCCEEDED(device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &staging_desc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback.p))));
        const UINT pitch = fp.Footprint.RowPitch;
        void* mapped = nullptr;
        EXPECT(SUCCEEDED(upload->Map(0, nullptr, &mapped)));
        std::memset(mapped, 0, size_t(total));
        for (UINT z = 0; z < depth; ++z)
            for (UINT y = 0; y < height; ++y)
                for (UINT x = 0; x < width; ++x)
                    static_cast<std::uint8_t*>(mapped)[size_t(z) * pitch * height + size_t(y) * pitch + x] =
                        texel(x, y, z);
        upload->Unmap(0, nullptr);

        Ref<ID3D12GraphicsCommandList> list;
        EXPECT(SUCCEEDED(alloc->Reset()));
        EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.p, nullptr,
                                                   IID_PPV_ARGS(&list.p))));
        D3D12_TEXTURE_COPY_LOCATION placed_up{upload.p, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        placed_up.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION sub{tex.p, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        sub.SubresourceIndex = 0;
        list->CopyTextureRegion(&sub, 0, 0, 0, &placed_up, nullptr);
        const auto to_src = transition(tex.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_src);
        D3D12_TEXTURE_COPY_LOCATION placed_rb{readback.p, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        placed_rb.PlacedFootprint = fp;
        list->CopyTextureRegion(&placed_rb, 0, 0, 0, &sub, nullptr);
        EXPECT(SUCCEEDED(list->Close()));
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, ++fence_value)));
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        EXPECT(event != nullptr);
        EXPECT(SUCCEEDED(fence->SetEventOnCompletion(fence_value, event)));
        const DWORD waited = WaitForSingleObject(event, 10000);
        CloseHandle(event);
        EXPECT(waited == WAIT_OBJECT_0);

        void* back = nullptr;
        const D3D12_RANGE read{0, size_t(total)};
        EXPECT(SUCCEEDED(readback->Map(0, &read, &back)));
        UINT mismatches = 0;
        for (UINT z = 0; z < depth; ++z)
            for (UINT y = 0; y < height; ++y)
                for (UINT x = 0; x < width; ++x)
                    mismatches += static_cast<const std::uint8_t*>(back)[size_t(z) * pitch * height +
                                                                         size_t(y) * pitch + x] != texel(x, y, z);
        readback->Unmap(0, nullptr);
        if (mismatches) std::fprintf(stderr, "%s: %u of %u texels differ\n", k.name, mismatches, width * height * depth);
        EXPECT(mismatches == 0);

        // 4. The default SRV: the image type, R8_UNORM's format and the alpha-only swizzle.
        D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(c) * step;
        std::memset(reinterpret_cast<void*>(handle.ptr), 0x77, step);
        device->CreateShaderResourceView(tex.p, nullptr, handle);
        g::ImageWords words{};
        std::memcpy(words.data(), reinterpret_cast<const void*>(handle.ptr), sizeof(words));
        g::Image image{};
        EXPECT(g::is_image(words) && g::decode(words, image));
        EXPECT(image.type == k.type && image.width == width && image.format == r8_code);
        EXPECT(k.type != g::ImageType::image_3d || (image.height == height && image.depth == depth));
        const g::Swizzle alpha_only{g::select_zero, g::select_zero, g::select_zero, g::select_x};
        EXPECT(image.swizzle == alpha_only);
        std::printf("A8_UNORM %s: sized %llu B, %u texels round-trip, SRV reads (0, 0, 0, A)\n", k.name,
                    static_cast<unsigned long long>(alloc_info.SizeInBytes), width * height * depth);
    }
    std::printf("d3d12_a8_dimensions: PASS\n");
    return 0;
}
