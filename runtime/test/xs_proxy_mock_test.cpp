// Xbox-ABI queue and fence proxies (d3d12_xs_device.h) over synthetic COM objects, with no D3D12 runtime and no GPU:
//   - devices that stop at ID3D12Device, Device1, Device3 and Device9: the slots of a missing interface (45/46,
//     50, 75) are refused without reaching the device, the others get the real objects behind the proxies;
//   - fences with and without ID3D12Fence1: 11 or 12 public slots, QueryInterface and slot 11 to match;
//   - fence slot 12: the address of a completed value that follows the real fence, from a CPU Signal through the
//     proxy and from a signal the proxy never sees, up to device removal, with the descriptor's code pointer unused;
//   - creation that fails, that returns S_OK with no object, a null IID or output, and an interface the real object
//     has but the proxy does not model;
//   - OpenSharedHandle of a fence (as ID3D12Fence, IUnknown, Fence1), of a non-fence object, of an invalid handle,
//     and with a null output or IID, with the references of every object it touches;
//   - allocation failures in the proxy (operator new, the arrays it unwraps), injected in this executable only;
//   - device slot 26 called the console's way (the struct's buffer in RCX, the device in RDX) and the PC way;
//   - --fail-virtualalloc / --fail-virtualprotect / --fail-veh: the guard region's setup failing, injected by
//     patching this executable's own import table before the first proxy table is built (one process each).
// Every device, queue, fence and resource here is a mock that records the calls it gets. No game data, no Xbox DLLs.
#include "../shims/d3d12_xs_device.h"

#include <d3d12.h>
#include <windows.h>

#include <algorithm>

#include <crtdbg.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>
#include <utility>

namespace {

const char* g_label = "xs_proxy_mock";

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "%s line %d: %s\n", g_label, __LINE__, #condition); return 1; } \
} while (0)

// ---- Allocation failures: this executable's global operator new fails the next g_fail_news calls ----

int g_fail_news = 0;     // single-threaded: armed right before the call it is meant for
int g_failed_news = 0;   // allocations refused so far

}  // namespace

// The annotations are vcruntime_new.h's, so /analyze sees the same contract as for the CRT's own operator new.
_NODISCARD _Ret_notnull_ _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new(std::size_t size) {
    if (g_fail_news > 0) {
        --g_fail_news;
        ++g_failed_news;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
_NODISCARD _Ret_maybenull_ _Success_(return != NULL) _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}
void __CRTDECL operator delete(void* p) noexcept { std::free(p); }
void __CRTDECL operator delete(void* p, std::size_t) noexcept { std::free(p); }
void __CRTDECL operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

namespace xs = rt::d3d12_xs;

// ---- Mocks ----

enum class Kind { device, queue, fence, resource, allocator, command_list, heap, list };

constexpr unsigned kMockSlots = 128;
constexpr std::uint64_t kFenceHandle = 0x1000, kResourceHandle = 0x2000;  // the handles the mock device opens
constexpr std::uint64_t kCompletedValue = 7;                            // what a mock fence's slot 8 returns
constexpr UINT kHeapMarker = 0x26262626;  // the VisibleNodeMask a mock device's slot 26 answers
constexpr SIZE_T kMockHeapCpuStart = 0x55550000ull;
constexpr UINT64 kMockHeapGpuStart = 0x66660000ull;
constexpr UINT kMockDescriptorInc = 32;
constexpr D3D12_DESCRIPTOR_HEAP_DESC kMockHeapDesc{
    D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0x3u};
constexpr D3D12_COMMAND_QUEUE_DESC kMockQueueDesc{
    D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_QUEUE_PRIORITY_HIGH, D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT,
    0x5u};
// {5d1e0c4a-2b7f-4e61-9a3d-8c4b2f1e6a70}: an interface the mock queue implements that the proxy does not model.
constexpr GUID kExtraIid{0x5d1e0c4a, 0x2b7f, 0x4e61, {0x9a, 0x3d, 0x8c, 0x4b, 0x2f, 0x1e, 0x6a, 0x70}};

struct Mock {
    void** vtable;
    Kind kind;
    long refs;
    int device_level;        // device: highest ID3D12DeviceN it answers (0 = ID3D12Device only)
    bool fence1;             // fence: answers ID3D12Fence1; device: the fences it makes do
    bool allocator_tag;      // allocator: answers kLeonidaCommandAllocatorTag; device: the allocators it makes do
    bool full_chain_lists;   // device: CreateCommandList makes a full-chain list mock (Kind::list), else command_list
    bool create_null;        // device: creation returns create_result with no object
    HRESULT create_result;   // device: what creation returns (S_OK makes the object)
    unsigned view_heap_limit;  // device: refuses a larger shader-visible CBV/SRV/UAV heap (0: no limit), as native D3D12
    Mock* last_child;        // device: the object its last creation or open made
    Mock* device10_alias;    // device: when set, QueryInterface(ID3D12Device10) answers this object, not itself
    UINT64 allocation_size;  // device: what GetResourceAllocationInfo (slot 25) answers
    IUnknown* private_data;  // resource: the SetPrivateDataInterface object, released with the last reference
    SRWLOCK lock;            // fence: guards completed, armed_value and armed_event (zeroed = SRWLOCK_INIT)
    std::uint64_t completed; // fence: what GetCompletedValue (slot 8) returns
    std::uint64_t armed_value;  // fence: the value SetEventOnCompletion (slot 9) waits for
    HANDLE armed_event;      // fence: the event it sets then, or null
    unsigned calls[kMockSlots];
    std::uint64_t args[kMockSlots][6];
    std::uint64_t array[kMockSlots][4];  // device: the first entries of the array argument, as received
    D3D12_COMMAND_QUEUE_DESC queue_desc;  // device: the description of its last CreateCommandQueue (slot 8)
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;  // device: the description CreateShaderResourceView (slot 18) last received
    UINT om_rtv_count;                   // list slot 46 (OMSetRenderTargets): captured RTV/DSV handles
    BOOL om_single_range;
    SIZE_T om_rtvs[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
    bool om_has_dsv;
    SIZE_T om_dsv;
    SIZE_T clear_dsv_handle;             // list slot 47 (ClearDepthStencilView): typed float-ABI capture
    D3D12_CLEAR_FLAGS clear_dsv_flags;
    FLOAT clear_dsv_depth;
    UINT8 clear_dsv_stencil;
    UINT clear_dsv_num_rects;
    const D3D12_RECT* clear_dsv_rects;
};

Mock g_pool[256];
unsigned g_pool_used = 0;

void** mock_vtable();
void** mock_list_vtable();

Mock* new_mock(Kind kind) {
    if (g_pool_used == std::size(g_pool)) {
        std::fprintf(stderr, "%s: mock pool exhausted\n", g_label);
        std::exit(1);
    }
    Mock* m = &g_pool[g_pool_used++];
    std::memset(m, 0, sizeof(*m));
    m->vtable = (kind == Kind::list || kind == Kind::command_list) ? mock_list_vtable() : mock_vtable();
    m->kind = kind;
    if (kind == Kind::fence) m->completed = kCompletedValue;
    return m;
}

// A mock fence's completed value, read and written under its lock: the proxy's wait thread calls slots 8 and 9 while
// the test signals. Signal sets the one armed event once the value reaches it, as the PC fence does.
std::uint64_t fence_completed(Mock* m) {
    AcquireSRWLockExclusive(&m->lock);
    const std::uint64_t value = m->completed;
    ReleaseSRWLockExclusive(&m->lock);
    return value;
}

void fence_signal(Mock* m, std::uint64_t value) {
    AcquireSRWLockExclusive(&m->lock);
    m->completed = value;
    if (m->armed_event && value >= m->armed_value) {
        SetEvent(m->armed_event);
        m->armed_event = nullptr;
    }
    ReleaseSRWLockExclusive(&m->lock);
}

HRESULT fence_set_event(Mock* m, std::uint64_t value, HANDLE event) {
    if (!event) return S_OK;  // the PC fence would block until `value`; no caller here does that
    AcquireSRWLockExclusive(&m->lock);
    if (m->completed >= value) {
        SetEvent(event);
    } else {
        m->armed_value = value;
        m->armed_event = event;
    }
    ReleaseSRWLockExclusive(&m->lock);
    return S_OK;
}

bool mock_has(const Mock* m, REFIID iid) {
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object))) return true;
    if (m->kind == Kind::device) {
        if (IsEqualIID(iid, __uuidof(ID3D12Device))) return true;
        const IID levels[] = {__uuidof(ID3D12Device1), __uuidof(ID3D12Device2), __uuidof(ID3D12Device3),
                              __uuidof(ID3D12Device4), __uuidof(ID3D12Device5), __uuidof(ID3D12Device6),
                              __uuidof(ID3D12Device7), __uuidof(ID3D12Device8), __uuidof(ID3D12Device9),
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device10),
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device11),
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device12),
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device13),
#endif
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device14),
#endif
        };
        for (int i = 0; i < m->device_level && i < static_cast<int>(std::size(levels)); ++i)
            if (IsEqualIID(iid, levels[i])) return true;
        return false;
    }
    if (IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Pageable))) return true;
    switch (m->kind) {
        case Kind::queue: return IsEqualIID(iid, __uuidof(ID3D12CommandQueue)) || IsEqualIID(iid, kExtraIid);
        case Kind::fence:
            return IsEqualIID(iid, __uuidof(ID3D12Fence)) || (m->fence1 && IsEqualIID(iid, __uuidof(ID3D12Fence1)));
        case Kind::resource: return IsEqualIID(iid, __uuidof(ID3D12Resource));
        case Kind::command_list:
            return IsEqualIID(iid, __uuidof(ID3D12CommandList)) ||
                   IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList));
        case Kind::allocator:
            return IsEqualIID(iid, __uuidof(ID3D12CommandAllocator)) ||
                   (m->allocator_tag && IsEqualIID(iid, xs::kLeonidaCommandAllocatorTag));
        case Kind::heap: return IsEqualIID(iid, __uuidof(ID3D12DescriptorHeap));
        case Kind::list: {
            // The mock answers every graphics-command-list interface so the proxy sees the full 82-slot chain.
            const IID lists[] = {__uuidof(ID3D12CommandList),         __uuidof(ID3D12GraphicsCommandList),
                                 __uuidof(ID3D12GraphicsCommandList1), __uuidof(ID3D12GraphicsCommandList2),
                                 __uuidof(ID3D12GraphicsCommandList3), __uuidof(ID3D12GraphicsCommandList4),
                                 __uuidof(ID3D12GraphicsCommandList5), __uuidof(ID3D12GraphicsCommandList6),
                                 __uuidof(ID3D12GraphicsCommandList7), __uuidof(ID3D12GraphicsCommandList8)};
            for (const IID& list_iid : lists) if (IsEqualIID(iid, list_iid)) return true;
            return false;
        }
        default: return false;
    }
}

HRESULT mock_query(Mock* m, const IID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid || !mock_has(m, *riid)) return E_NOINTERFACE;
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (m->device10_alias && IsEqualIID(*riid, __uuidof(ID3D12Device10))) m = m->device10_alias;
#endif
    *out = m;
    ++m->refs;
    return S_OK;
}

// A device creating (or opening) a `kind` object as `riid`: S_FALSE without an output, as the PC runtime answers a
// capability check; the device's create_result and create_null otherwise.
HRESULT mock_create(Mock* device, Kind kind, std::uint64_t riid, std::uint64_t out_word) {
    auto** out = reinterpret_cast<void**>(out_word);
    if (!out) return S_FALSE;
    *out = nullptr;
    if (device->create_null || FAILED(device->create_result)) return device->create_result;
    Mock* child = new_mock(kind);
    child->fence1 = device->fence1;
    child->allocator_tag = device->allocator_tag;
    device->last_child = child;
    return mock_query(child, reinterpret_cast<const IID*>(riid), out);
}

void copy_array(Mock* m, unsigned slot, std::uint64_t objects, std::uint64_t count) {
    const auto* in = reinterpret_cast<const std::uint64_t*>(objects);
    for (std::uint32_t i = 0; in && i < static_cast<std::uint32_t>(count) && i < 4; ++i) m->array[slot][i] = in[i];
}

// GetResourceAllocationInfo as a PC driver without console tile padding answers it. A buffer is its width, a texture
// width x height x slices x element bytes (2 for B5G5R5A1, 4 for RGBA8, 8 for RGBA16F, 16 for RGBA32F; mips are not
// counted, and one resource is at most 4 GiB, the way a driver caps an allocation), each rounded up to the
// resource's alignment: 64 KiB, or 4 MiB with more than one sample. A texture with no width is refused. Resources are
// laid out one after another, each at its own alignment, and the total is rounded up to the largest alignment, as the
// Vulkan layer's place() does. The struct is written to the hidden return pointer (RDX of a C++ method call), which
// comes back in RAX. `each` (slot 56) gets every resource's offset, alignment and size.
std::uint64_t mock_align(std::uint64_t value, std::uint64_t to) {
    return value > UINT64_MAX - (to - 1) ? UINT64_MAX / to * to : (value + to - 1) / to * to;
}

D3D12_RESOURCE_ALLOCATION_INFO mock_one_allocation(const D3D12_RESOURCE_DESC& d) {
    if (d.Width == 0) return {UINT64_MAX, 65536};
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return {mock_align(d.Width, 65536), 65536};
    std::uint64_t element = 4;
    if (d.Format == DXGI_FORMAT_B5G5R5A1_UNORM) element = 2;
    if (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) element = 8;
    if (d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT) element = 16;
    const std::uint64_t samples = d.SampleDesc.Count ? d.SampleDesc.Count : 1;
    const std::uint64_t alignment = samples > 1 ? 4194304 : 65536;
    const std::uint64_t bytes = std::min<std::uint64_t>(
        d.Width * d.Height * d.DepthOrArraySize * element * samples, std::uint64_t{4} << 30);
    return {mock_align(bytes, alignment), alignment};
}

D3D12_RESOURCE_DESC g_seen_descs[4];  // the first descriptions the mock device's slot 25 was given
std::uint32_t g_seen_desc_count = 0;
std::uint32_t g_seen_calls = 0;  // slot 25 calls since the test cleared it; only the first is recorded

std::uint64_t mock_allocation_info(std::uint64_t ret, std::uint64_t count, std::uint64_t descs_word,
                                   D3D12_RESOURCE_ALLOCATION_INFO1* each = nullptr) {
    const auto* descs = reinterpret_cast<const D3D12_RESOURCE_DESC*>(descs_word);
    if (g_seen_calls++ == 0) {
        g_seen_desc_count = static_cast<std::uint32_t>(count);
        for (std::uint32_t i = 0; descs && i < g_seen_desc_count && i < std::size(g_seen_descs); ++i)
            g_seen_descs[i] = descs[i];
    }
    D3D12_RESOURCE_ALLOCATION_INFO total{0, 1};
    bool refused = false;
    for (std::uint32_t i = 0; descs && i < static_cast<std::uint32_t>(count); ++i) {
        const D3D12_RESOURCE_ALLOCATION_INFO one = mock_one_allocation(descs[i]);
        refused = refused || one.SizeInBytes == UINT64_MAX;
        const std::uint64_t offset = mock_align(total.SizeInBytes, one.Alignment);
        if (each) each[i] = {offset, one.Alignment, one.SizeInBytes};
        total.SizeInBytes = one.SizeInBytes > UINT64_MAX - offset ? UINT64_MAX : offset + one.SizeInBytes;
        total.Alignment = std::max(total.Alignment, one.Alignment);
    }
    auto* out = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(ret);
    *out = {refused ? UINT64_MAX : mock_align(total.SizeInBytes, total.Alignment), total.Alignment};
    return ret;
}

// `words` are the call's first six words (a1..a6), copied into the thunk's own frame.
std::uint64_t mock_call(Mock* m, unsigned slot, const std::uint64_t* words) {
    const std::uint64_t a1 = words[0], a2 = words[1], a3 = words[2], a4 = words[3], a5 = words[4], a6 = words[5];
    if (m->kind == Kind::device) {
        // The description lives on the caller's stack only for the call, so it is copied while the call is active.
        if (slot == 18 && a2) std::memcpy(&m->srv, reinterpret_cast<const void*>(a2), sizeof(m->srv));
        switch (slot) {
            case 8:                                                                           // CreateCommandQueue
                if (a1) std::memcpy(&m->queue_desc, reinterpret_cast<const void*>(a1), sizeof(m->queue_desc));
                return static_cast<std::uint32_t>(mock_create(m, Kind::queue, a2, a3));
            case 9: return static_cast<std::uint32_t>(mock_create(m, Kind::allocator, a2, a3));  // CreateCmdAllocator
            case 14:                                                         // CreateDescriptorHeap(desc, riid, out)
                if (const auto* d = reinterpret_cast<const D3D12_DESCRIPTOR_HEAP_DESC*>(a1)) {
                    m->array[14][m->calls[14] > 1 ? 1 : 0] = d->NumDescriptors;   // first and latest request
                    if (m->view_heap_limit && d->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
                        (d->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) && d->NumDescriptors > m->view_heap_limit)
                        return static_cast<std::uint32_t>(E_INVALIDARG);
                }
                return static_cast<std::uint32_t>(mock_create(m, Kind::heap, a2, a3));
            case 15: return kMockDescriptorInc;                                                  // GetDescriptorHandleIncrementSize
            case 23:                                                                             // CopyDescriptors
                copy_array(m, 23, a2, a1);                                                       // dest starts
                copy_array(m, 24, words[4], a4);                                                 // src starts (recorded in slot 24's array)
                break;
            // Slot 12 (CreateCommandList) never reaches this switch: the device vtable's slot is the dedicated
            // mock_create_command_list, which needs the sixth word the generic thunk does not declare on some
            // builds and which picks the list mock's chain breadth from full_chain_lists.
            case 75: return static_cast<std::uint32_t>(mock_create(m, Kind::queue, a3, a4));   // CreateCommandQueue1
            case 36: return static_cast<std::uint32_t>(mock_create(m, Kind::fence, a3, a4));   // CreateFence
            case 32:                                                                          // OpenSharedHandle
                if (a1 == kFenceHandle) return static_cast<std::uint32_t>(mock_create(m, Kind::fence, a2, a3));
                if (a1 == kResourceHandle) return static_cast<std::uint32_t>(mock_create(m, Kind::resource, a2, a3));
                return static_cast<std::uint32_t>(E_INVALIDARG);
            case 25: return mock_allocation_info(a1, a3, a4);  // GetResourceAllocationInfo (mask a2 is unused)
            case 56:  // GetResourceAllocationInfo1: (return buffer, mask, count, descs, per-description array)
                return mock_allocation_info(a1, a3, a4, reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO1*>(words[4]));
            case 34: case 35: case 46: copy_array(m, slot, a2, a1); break;  // (count, objects, ...)
            case 45: copy_array(m, slot, a1, a3); break;                    // (fences, values, count, ...)
            case 26: {  // GetCustomHeapProperties, the PC method: (return buffer, node mask, heap type)
                auto* out = reinterpret_cast<D3D12_HEAP_PROPERTIES*>(a1);
                if (!out) break;
                *out = {};
                out->Type = static_cast<D3D12_HEAP_TYPE>(static_cast<std::uint32_t>(a3));
                out->CreationNodeMask = static_cast<UINT>(a2);
                out->VisibleNodeMask = kHeapMarker;
                return a1;  // a struct-returning method hands the buffer's address back in RAX
            }
            case 41:                                                        // CreateCommandSignature(desc, ...)
                if (const auto* desc = reinterpret_cast<const D3D12_COMMAND_SIGNATURE_DESC*>(a1)) {
                    m->array[41][0] = desc->ByteStride | std::uint64_t{desc->NumArgumentDescs} << 32;
                    for (UINT i = 0; desc->pArgumentDescs && i < desc->NumArgumentDescs && i < 3; ++i) {
                        std::uint32_t type = 0;  // read as its 32 bits: a console type is outside the enum's range
                        std::memcpy(&type, &desc->pArgumentDescs[i].Type, sizeof(type));
                        m->array[41][i + 1] = type;
                    }
                }
                break;
            case 50: copy_array(m, slot, a3, a2); break;                    // (flags, count, objects, fence, value)
            case 68: {                                                      // GetResourceAllocationInfo2
                auto* ret = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(a1);
                const auto count = static_cast<std::uint32_t>(a3);
                const auto* descs = reinterpret_cast<const D3D12_RESOURCE_DESC1*>(a4);
                auto* info1 = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO1*>(words[4]);
                if (descs && count > 0) {
                    m->array[68][0] = static_cast<std::uint64_t>(descs[0].Layout);
                    m->array[68][1] = static_cast<std::uint64_t>(descs[0].Flags);
                    m->array[68][2] = descs[0].SamplerFeedbackMipRegion.Width |
                                      descs[0].SamplerFeedbackMipRegion.Height |
                                      descs[0].SamplerFeedbackMipRegion.Depth;
                    if (count > 1) {
                        const auto& last = descs[count - 1];
                        m->array[68][3] = (static_cast<std::uint64_t>(last.Layout) << 32) |
                                          static_cast<std::uint32_t>(last.Flags);
                    }
                }
                if (ret) *ret = {0x40000, 0x10000};
                if (info1 && count > 0) info1[0] = {0, 0x10000, 0x40000};
                return a1;
            }
            default: break;
        }
        return S_OK;
    }
    if (m->kind == Kind::fence) {
        switch (slot) {
            case 8: return fence_completed(m);  // GetCompletedValue
            case 9: return static_cast<std::uint32_t>(fence_set_event(m, a1, reinterpret_cast<HANDLE>(a2)));
            case 10: fence_signal(m, a1); return S_OK;  // Signal
            default: break;
        }
    }
    if (m->kind == Kind::resource && slot == 5) {                     // SetPrivateDataInterface(guid, object)
        auto* const object = reinterpret_cast<IUnknown*>(a2);
        if (object) object->AddRef();
        if (m->private_data) m->private_data->Release();
        m->private_data = object;
        return S_OK;
    }
    if (m->kind == Kind::heap && slot == 9) {
        if (a1) reinterpret_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(a1)->ptr = kMockHeapCpuStart;
        return kMockHeapCpuStart;
    }
    if (m->kind == Kind::heap && slot == 10) {
        if (a1) reinterpret_cast<D3D12_GPU_DESCRIPTOR_HANDLE*>(a1)->ptr = kMockHeapGpuStart;
        return kMockHeapGpuStart;
    }
    if (m->kind == Kind::queue && slot == 10) copy_array(m, 10, a2, a1);  // ExecuteCommandLists(count, lists)
    if (m->kind == Kind::queue && slot == 16 && a1) {
        // GetTimestampFrequency writes a fixed value — unless a1 is a mock object, which on this slot is a
        // newer-layout Signal's fence, not an out pointer.
        const auto at = static_cast<std::uintptr_t>(a1);
        const auto base = reinterpret_cast<std::uintptr_t>(g_pool);
        const bool is_mock = at >= base && at < base + sizeof(g_pool) && (at - base) % sizeof(Mock) == 0;
        if (!is_mock) *reinterpret_cast<UINT64*>(a1) = 1000000;
    }
    if (m->kind == Kind::heap && slot == 8) {                         // PC GetDesc(this, &out)
        auto* out = reinterpret_cast<D3D12_DESCRIPTOR_HEAP_DESC*>(a1);
        if (out) *out = kMockHeapDesc;
        return a1;
    }
    if (m->kind == Kind::queue && slot == 18) {                       // PC GetDesc(this, &out)
        auto* out = reinterpret_cast<D3D12_COMMAND_QUEUE_DESC*>(a1);
        if (out) *out = kMockQueueDesc;
        return a1;
    }
    if ((m->kind == Kind::list || m->kind == Kind::command_list) && slot == 46) {  // OMSetRenderTargets
        m->om_rtv_count = static_cast<UINT>(a1);
        m->om_single_range = static_cast<BOOL>(a3);
        std::memset(m->om_rtvs, 0, sizeof(m->om_rtvs));
        const auto* rtvs = reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(a2);
        if (rtvs && m->om_rtv_count > 0) {
            const UINT n = m->om_single_range
                               ? 1u
                               : (m->om_rtv_count < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT
                                      ? m->om_rtv_count
                                      : D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
            for (UINT i = 0; i < n; ++i) m->om_rtvs[i] = rtvs[i].ptr;
        }
        const auto* dsv = reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(a4);
        m->om_has_dsv = dsv != nullptr;
        m->om_dsv = dsv ? dsv->ptr : 0;
    }
    return S_OK;
}

template <unsigned Slot>
std::uint64_t WINAPI mock_slot(Mock* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4,
                               std::uint64_t a5, std::uint64_t a6) {
    if constexpr (Slot == 0) {
        return static_cast<std::uint32_t>(mock_query(self, reinterpret_cast<const IID*>(a1),
                                                     reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return static_cast<std::uint64_t>(++self->refs);
    } else if constexpr (Slot == 2) {
        const long left = --self->refs;
        if (left == 0 && self->private_data) {  // an object releases its private-data interfaces when it dies
            IUnknown* const held = self->private_data;
            self->private_data = nullptr;
            held->Release();
        }
        return static_cast<std::uint64_t>(left);
    } else {
        // A caller may pass fewer words than this thunk declares (OpenSharedHandle passes three), so its frame
        // has no room past the four register homes. The words are copied into this frame and passed by pointer:
        // forwarding them as stack arguments let MSVC tail-call mock_call and write into the caller's saved
        // registers (an optimized build corrupted the test's own locals that way).
        ++self->calls[Slot];
        const std::uint64_t words[] = {a1, a2, a3, a4, a5, a6};
        std::memcpy(self->args[Slot], words, sizeof(words));
        return mock_call(self, Slot, words);
    }
}

void WINAPI mock_clear_depth_stencil_view(Mock* self, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags,
                                          FLOAT depth, UINT8 stencil, UINT num_rects, const D3D12_RECT* rects) {
    ++self->calls[47];
    self->clear_dsv_handle = dsv.ptr;
    self->clear_dsv_flags = flags;
    self->clear_dsv_depth = depth;
    self->clear_dsv_stencil = stencil;
    self->clear_dsv_num_rects = num_rects;
    self->clear_dsv_rects = rects;
}

// PC ID3D12Device::CreateCommandList takes six words after `this`, one more than the generic mock thunk declares, so
// it gets its own entry in the table: the out pointer is a6 (first stack argument on x64) and has to be written.
HRESULT WINAPI mock_create_command_list(Mock* self, std::uint64_t node_mask, std::uint64_t type,
                                        std::uint64_t allocator, std::uint64_t initial_state, const IID* riid,
                                        void** out) {
    ++self->calls[12];
    self->args[12][0] = node_mask;
    self->args[12][1] = type;
    self->args[12][2] = allocator;
    self->args[12][3] = initial_state;
    self->args[12][4] = reinterpret_cast<std::uint64_t>(riid);
    if (!out) return S_FALSE;
    *out = nullptr;
    if (self->create_null || FAILED(self->create_result)) return self->create_result;
    Mock* child = new_mock(self->full_chain_lists ? Kind::list : Kind::command_list);
    self->last_child = child;
    return mock_query(child, riid, out);
}

#ifdef __ID3D12Device10_INTERFACE_DEFINED__
// PC ID3D12Device10::CreateCommittedResource3 takes ten words after `this`, so it has its own entry (PC slot 76) that
// keeps all of them: the resource description is copied because the caller's copy is a local of the proxy.
struct CommittedResource3Call {
    unsigned calls;
    const Mock* device;  // the PC object the call arrived on
    const D3D12_HEAP_PROPERTIES* heap;
    D3D12_HEAP_FLAGS heap_flags;
    D3D12_RESOURCE_DESC1 desc;
    std::uint32_t layout;
    const D3D12_CLEAR_VALUE* clear;
    const void* session;
    std::uint32_t castable_count;
    const DXGI_FORMAT* castable_formats;
    const IID* riid;
    void** out;
};
CommittedResource3Call g_committed3;

HRESULT WINAPI mock_create_committed_resource3(Mock* self, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags,
                                               const D3D12_RESOURCE_DESC1* desc, std::uint32_t layout,
                                               const D3D12_CLEAR_VALUE* clear, const void* session,
                                               std::uint32_t castable_count, const DXGI_FORMAT* castable_formats,
                                               const IID* riid, void** out) {
    g_committed3 = {g_committed3.calls + 1, self, heap, flags, *desc, layout, clear, session, castable_count,
                    castable_formats, riid, out};
    return mock_create(self, Kind::resource, reinterpret_cast<std::uint64_t>(riid), reinterpret_cast<std::uint64_t>(out));
}
#endif

// PC ID3D12Device::CreateCommittedResource (slot 27, seven words) and ID3D12Device4::CreateCommittedResource1 (slot 53,
// eight words) also take more words than the generic thunk; their entries keep the ones the checks read.
struct CommittedCall {
    unsigned calls;
    D3D12_HEAP_FLAGS heap_flags;
    D3D12_RESOURCE_DESC desc;
    const void* session;
    const IID* riid;
    void** out;
};
CommittedCall g_committed[2];  // [0]: slot 27, [1]: slot 53

HRESULT WINAPI mock_create_committed_resource(Mock* self, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS flags,
                                              const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES,
                                              const D3D12_CLEAR_VALUE*, const IID* riid, void** out) {
    ++self->calls[27];
    g_committed[0] = {g_committed[0].calls + 1, flags, desc ? *desc : D3D12_RESOURCE_DESC{}, nullptr, riid, out};
    return mock_create(self, Kind::resource, reinterpret_cast<std::uint64_t>(riid), reinterpret_cast<std::uint64_t>(out));
}

HRESULT WINAPI mock_create_committed_resource1(Mock* self, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS flags,
                                               const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES,
                                               const D3D12_CLEAR_VALUE*, const void* session, const IID* riid,
                                               void** out) {
    ++self->calls[53];
    g_committed[1] = {g_committed[1].calls + 1, flags, desc ? *desc : D3D12_RESOURCE_DESC{}, session, riid, out};
    return mock_create(self, Kind::resource, reinterpret_cast<std::uint64_t>(riid), reinterpret_cast<std::uint64_t>(out));
}

template <std::size_t... I>
void fill_mock_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&mock_slot<static_cast<unsigned>(I)>)), ...);
}

void** mock_vtable() {
    static void* table[kMockSlots];
    static const bool filled = [] {
        fill_mock_vtable(table, std::make_index_sequence<kMockSlots>{});
        table[12] = reinterpret_cast<void*>(&mock_create_command_list);
        table[27] = reinterpret_cast<void*>(&mock_create_committed_resource);
        table[53] = reinterpret_cast<void*>(&mock_create_committed_resource1);
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
        table[76] = reinterpret_cast<void*>(&mock_create_committed_resource3);
#endif
        return true;
    }();
    (void)filled;
    return table;
}

void** mock_list_vtable() {
    static void* table[kMockSlots];
    static const bool filled = [] {
        fill_mock_vtable(table, std::make_index_sequence<kMockSlots>{});
        table[47] = reinterpret_cast<void*>(&mock_clear_depth_stencil_view);
        return true;
    }();
    (void)filled;
    return table;
}

// ---- Calls by slot, the way a title compiled against the console headers makes them ----

using SlotFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

std::uint64_t raw(const void* object, unsigned slot, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                  std::uint64_t a3 = 0, std::uint64_t a4 = 0, std::uint64_t a5 = 0, std::uint64_t a6 = 0) {
    void* const* vtable = *reinterpret_cast<void* const* const*>(object);
    return reinterpret_cast<SlotFn>(vtable[slot])(const_cast<void*>(object), a1, a2, a3, a4, a5, a6, 0, 0, 0, 0, 0,
                                                  0, 0, 0);
}

// A call with the ten words of device slot 127 (and any other slot that takes up to ten).
std::uint64_t raw10(const void* object, unsigned slot, const std::uint64_t (&w)[10]) {
    void* const* vtable = *reinterpret_cast<void* const* const*>(object);
    return reinterpret_cast<SlotFn>(vtable[slot])(const_cast<void*>(object), w[0], w[1], w[2], w[3], w[4], w[5], w[6],
                                                  w[7], w[8], w[9], 0, 0, 0, 0);
}

HRESULT hr_of(std::uint64_t value) {
    return static_cast<HRESULT>(static_cast<std::uint32_t>(value));
}

template <class T>
std::uint64_t word(const T* p) {
    return reinterpret_cast<std::uint64_t>(p);
}

const IID kQueueIid = __uuidof(ID3D12CommandQueue);
const IID kFenceIid = __uuidof(ID3D12Fence);
const IID kFence1Iid = __uuidof(ID3D12Fence1);
const IID kUnknownIid = __uuidof(IUnknown);
const IID kResourceIid = __uuidof(ID3D12Resource);
const IID kDevice1Iid = __uuidof(ID3D12Device1);
const D3D12_COMMAND_QUEUE_DESC kQueueDesc{};
// The retail title's slot 48 desc is 32 bytes (the route logs words 4..7); type in word 0 as in kQueueDesc.
const std::uint32_t kXboxQueueDesc[8]{};

// A device proxy over a new mock device. The proxy takes the mock's one reference.
void* make_device(Mock*& mock, int level, bool fence1, bool allocator_tag = true, bool full_chain_lists = false) {
    mock = new_mock(Kind::device);
    mock->device_level = level;
    mock->fence1 = fence1;
    mock->allocator_tag = allocator_tag;
    mock->full_chain_lists = full_chain_lists;
    mock->refs = 1;
    void* device = nullptr;
    if (FAILED(xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(mock), __uuidof(ID3D12Device), &device,
                                       /*console_lists=*/true)))
        return nullptr;
    return device;
}

void* make_queue(void* device) {
    void* queue = nullptr;
    return SUCCEEDED(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&queue)))) ? queue : nullptr;
}

void* make_fence(void* device, const IID& iid = kFenceIid) {
    void* fence = nullptr;
    return SUCCEEDED(hr_of(raw(device, 36, 0, 0, word(&iid), word(&fence)))) ? fence : nullptr;
}

unsigned long release(void* object) {
    return static_cast<unsigned long>(raw(object, 2));
}

// ---- 1. Device interface levels ----

int check_levels() {
    struct Level { int level; unsigned slots; } const levels[] = {{0, 44}, {1, 47}, {3, 51}, {9, 76}};
    for (const Level& l : levels) {
        Mock* mock = nullptr;
        void* device = make_device(mock, l.level, true);
        EXPECT(device && xs::device_proxy_slot_count(device) == l.slots);
        void* queue = make_queue(device);
        void* fence = make_fence(device);
        EXPECT(xs::is_queue_proxy(queue) && xs::is_fence_proxy(fence));
        const auto real_queue = word(xs::queue_proxy_real_queue(queue));
        const auto real_fence = word(xs::fence_proxy_real_fence(fence));
        EXPECT(real_queue && real_fence && real_queue != word(queue) && real_fence != word(fence));
        // What every residency call gets, with the proxies in it replaced by their real objects.
        const std::uint64_t objects[4] = {word(queue), word(fence), real_queue, 0};
        const std::uint64_t unwrapped[4] = {real_queue, real_fence, real_queue, 0};
        const std::uint64_t fences[2] = {word(fence), word(fence)};
        const std::uint64_t values[2] = {1, 2};
        const D3D12_RESIDENCY_PRIORITY priorities[4] = {};
        const auto same = [](const std::uint64_t* got, const std::uint64_t* want, int n) {
            return std::memcmp(got, want, n * sizeof(std::uint64_t)) == 0;
        };

        // 34 MakeResident, with a count whose high dword is not zero: the PC prototype takes a UINT.
        EXPECT(hr_of(raw(device, 34, 0xdeadbeef00000002ull, word(objects))) == S_OK);
        EXPECT(mock->calls[34] == 1 && static_cast<std::uint32_t>(mock->args[34][0]) == 2);
        EXPECT(same(mock->array[34], unwrapped, 2) && mock->array[34][2] == 0);
        // 31 CreateSharedHandle
        EXPECT(hr_of(raw(device, 31, word(fence), 0, GENERIC_ALL, 0, 0)) == S_OK);
        EXPECT(mock->calls[31] == 1 && mock->args[31][0] == real_fence);

        // 45/46: ID3D12Device1
        const HRESULT r45 = hr_of(raw(device, 45, word(fences), word(values), 2, 0, 0));
        const HRESULT r46 = hr_of(raw(device, 46, 4, word(objects), word(priorities)));
        if (l.level >= 1) {
            EXPECT(r45 == S_OK && mock->calls[45] == 1 && mock->array[45][0] == real_fence &&
                   mock->array[45][1] == real_fence);
            EXPECT(r46 == S_OK && mock->calls[46] == 1 && same(mock->array[46], unwrapped, 4));
        } else {
            EXPECT(r45 == E_NOTIMPL && r46 == E_NOTIMPL && mock->calls[45] == 0 && mock->calls[46] == 0);
            void* device1 = nullptr;
            EXPECT(FAILED(hr_of(raw(device, 0, word(&kDevice1Iid), word(&device1)))) && !device1);
        }
        // 50: ID3D12Device3, with the fence argument unwrapped too
        const HRESULT r50 = hr_of(raw(device, 50, 0, 4, word(objects), word(fence), 1));
        if (l.level >= 3) {
            EXPECT(r50 == S_OK && mock->calls[50] == 1 && same(mock->array[50], unwrapped, 4) &&
                   mock->args[50][3] == real_fence);
        } else {
            EXPECT(r50 == E_NOTIMPL && mock->calls[50] == 0);
        }
        // 75: ID3D12Device9::CreateCommandQueue1
        void* queue1 = reinterpret_cast<void*>(1);  // cleared on every failure (bc59ef46's convention)
        const HRESULT r75 = hr_of(raw(device, 75, word(&kQueueDesc), 0, word(&kQueueIid), word(&queue1)));
        if (l.level >= 9) {
            EXPECT(r75 == S_OK && xs::is_queue_proxy(queue1) && mock->calls[75] == 1);
            EXPECT(release(queue1) == 0 && mock->last_child->refs == 0);
        } else {
            EXPECT(r75 == E_NOTIMPL && !queue1 && mock->calls[75] == 0);
        }

        // 133: past every PC device vtable; the retail title calls it. Refused, not a jump through a missing entry.
        EXPECT(hr_of(raw(device, 133)) == E_NOTIMPL && mock->calls[127] == 0);

        // 133 with a handler (CreatePlacedResourceX): the arguments in the title's positions (va, desc, state,
        // clear, riid, out) reach the handler with the real device, the Xbox flag bits masked to the PC ones the host
        // takes (0x20004 -> 0x4, and 0x200 dropped) and the buffer's Xbox layout word made ROW_MAJOR.
        {
            static void* seen_real = nullptr;
            static void* seen_va = nullptr;
            static D3D12_RESOURCE_DESC seen_desc{};
            static GUID seen_iid{};
            static std::uint32_t seen_layout = 0;
            const xs::PlacedAtVaHandler handler = [](ID3D12Device* real_device, void* va,
                                                     const D3D12_RESOURCE_DESC& desc, std::uint32_t layout,
                                                     REFIID iid, void** out) -> HRESULT {
                seen_layout = layout;
                seen_real = real_device;
                seen_va = va;
                seen_desc = desc;
                seen_iid = iid;
                *out = reinterpret_cast<void*>(0x1330);
                return S_OK;
            };
            xs::set_placed_at_va_handler(handler);
            D3D12_RESOURCE_DESC xbox_desc{};
            xbox_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            xbox_desc.Width = 0x10000;
            xbox_desc.Height = xbox_desc.DepthOrArraySize = xbox_desc.MipLevels = 1;
            xbox_desc.SampleDesc.Count = 1;
            xbox_desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);  // the Xbox row-major word
            xbox_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20204);
            const GUID resource_iid = __uuidof(ID3D12Resource);
            void* placed = nullptr;
            void* const va = reinterpret_cast<void*>(0x7f0000000000ull);
            const HRESULT r133 = hr_of(raw(device, 133, word(va), word(&xbox_desc), 0xFFFFFFFFull, 0,
                                           word(&resource_iid), word(&placed)));
            xs::set_placed_at_va_handler(nullptr);
            EXPECT(r133 == S_OK && placed == reinterpret_cast<void*>(0x1330));
            EXPECT(seen_real == xs::device_proxy_real_device(device) && seen_va == va);
            EXPECT(static_cast<std::uint32_t>(seen_desc.Flags) == 0x4);
            EXPECT(seen_desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR && seen_desc.Width == 0x10000);
            EXPECT(std::memcmp(&seen_iid, &resource_iid, sizeof(GUID)) == 0);
            EXPECT(seen_layout == 0xFFFFFFFFu);  // the title's third word, passed through as the barrier layout
            EXPECT(xs::device_slot_overridden(133));
        }

        // 86: Xbox CreatePipelineState -> PC slot 47 (ID3D12Device2)
        void* pso_out = reinterpret_cast<void*>(1);
        const HRESULT r86 = hr_of(raw(device, 86, 0, word(&kUnknownIid), word(&pso_out)));
        if (l.level >= 2) {
            EXPECT(r86 == S_OK && mock->calls[47] == 1 && mock->calls[86] == 0);
        } else {
            EXPECT(r86 == E_NOTIMPL && !pso_out && mock->calls[47] == 0 && mock->calls[86] == 0);
        }

        // 101: Xbox CreateStateObject -> PC slot 62 (ID3D12Device5)
        void* so_out = reinterpret_cast<void*>(1);
        const HRESULT r101 = hr_of(raw(device, 101, 0, word(&kUnknownIid), word(&so_out)));
        if (l.level >= 5) {
            EXPECT(r101 == S_OK && mock->calls[62] == 1 && mock->calls[101] == 0);
        } else {
            EXPECT(r101 == E_NOTIMPL && !so_out && mock->calls[62] == 0 && mock->calls[101] == 0);
        }

        // 108: Xbox GetResourceAllocationInfo2 -> PC slot 68 (ID3D12Device8)
        D3D12_RESOURCE_ALLOCATION_INFO alloc_info{1, 1};
        const std::uint64_t r108 = raw(device, 108, word(&alloc_info), 1, 0, 0, 0);
        if (l.level >= 8) {
            EXPECT(r108 == word(&alloc_info) && alloc_info.SizeInBytes == 0x40000 && alloc_info.Alignment == 0x10000 &&
                   mock->calls[68] == 1 && mock->calls[108] == 0);
        } else {
            EXPECT(r108 == word(&alloc_info) && alloc_info.SizeInBytes == UINT64_MAX && alloc_info.Alignment == 0 &&
                   mock->calls[68] == 0 && mock->calls[108] == 0);
        }

        Mock* const queue_mock = reinterpret_cast<Mock*>(real_queue);
        Mock* const fence_mock = reinterpret_cast<Mock*>(real_fence);
        EXPECT(release(queue) == 0 && release(fence) == 0);
        EXPECT(queue_mock->refs == 0 && fence_mock->refs == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: devices up to ID3D12Device, Device1, Device3, Device9: 44/47/51/76 slots, 45/46/50/75 refused "
                "without the interface, residency arrays and fences unwrapped otherwise\n", g_label);
    return 0;
}

// ---- 1b. Slot 48: the title's CreateCommandQueue with a 32-byte description ----

int check_slot48() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 3, true);
    EXPECT(device);
    // 16-byte PC prefix, then four Xbox words; the flags carry a bit the PC does not know besides the timeout bit.
    const std::uint32_t desc[8] = {D3D12_COMMAND_LIST_TYPE_COMPUTE, 100,
                                   0x8000u | D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT, 1, 0x11, 0x22, 0x33, 0x44};
    void* queue = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 48, word(desc), word(&kQueueIid), word(&queue))) == S_OK);
    EXPECT(xs::is_queue_proxy(queue) && mock->calls[8] == 1);
    EXPECT(mock->queue_desc.Type == D3D12_COMMAND_LIST_TYPE_COMPUTE && mock->queue_desc.Priority == 100 &&
           mock->queue_desc.Flags == D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT &&
           mock->queue_desc.NodeMask == 1);
    EXPECT(release(queue) == 0);
    void* none = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 48, 0, word(&kQueueIid), word(&none))) == E_INVALIDARG && !none);
    EXPECT(mock->calls[8] == 1);
    EXPECT(release(device) == 0);
    std::printf("%s: slot 48 makes a queue proxy from a 32-byte description, PC prefix and flags only\n", g_label);
    return 0;
}

// One COM identity with separate base and extended-interface pointers. The base table is padded with recording
// slots so an incorrect call through it fails a check instead of reading beyond a short vtable.
Mock* g_device_identity = nullptr;
Mock* g_device_extension = nullptr;

HRESULT WINAPI distinct_device_query(Mock*, const IID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_NOINTERFACE;
    Mock* answer = nullptr;
    if (IsEqualIID(*riid, __uuidof(IUnknown)) || IsEqualIID(*riid, __uuidof(ID3D12Object)) ||
        IsEqualIID(*riid, __uuidof(ID3D12Device))) {
        answer = g_device_identity;
    } else if (mock_has(g_device_extension, *riid)) {
        answer = g_device_extension;
    }
    if (!answer) return E_NOINTERFACE;
    ++answer->refs;
    *out = answer;
    return S_OK;
}

template <unsigned Slot>
HRESULT WINAPI distinct_device_create(Mock* self, const void* desc, const IID* riid, void** out) {
    ++self->calls[Slot];
    self->args[Slot][0] = word(desc);
    self->args[Slot][1] = word(riid);
    self->args[Slot][2] = word(out);
    if (out) *out = nullptr;
    return self->create_result;
}

int check_distinct_device_interfaces() {
    Mock* base = new_mock(Kind::device);
    Mock* extension = new_mock(Kind::device);
    base->refs = 1;
    extension->device_level = 8;
    g_device_identity = base;
    g_device_extension = extension;
    void* base_table[kMockSlots];
    void* extension_table[kMockSlots];
    std::memcpy(base_table, mock_vtable(), sizeof(base_table));
    std::memcpy(extension_table, mock_vtable(), sizeof(extension_table));
    base_table[0] = extension_table[0] = reinterpret_cast<void*>(&distinct_device_query);
    extension_table[47] = reinterpret_cast<void*>(&distinct_device_create<47>);
    extension_table[62] = reinterpret_cast<void*>(&distinct_device_create<62>);
    base->vtable = base_table;
    extension->vtable = extension_table;

    // Both pointers expose the same IUnknown identity, but QI(Device8) does not return the base pointer.
    const IID device8_iid = __uuidof(ID3D12Device8);
    void* queried = nullptr;
    EXPECT(hr_of(raw(base, 0, word(&device8_iid), word(&queried))) == S_OK && queried == extension);
    void* identity = nullptr;
    EXPECT(hr_of(raw(queried, 0, word(&kUnknownIid), word(&identity))) == S_OK && identity == base);
    EXPECT(release(identity) == 1 && release(queried) == 0);

    void* device = nullptr;
    EXPECT(xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(base), __uuidof(ID3D12Device), &device) == S_OK);
    EXPECT(xs::device_proxy_slot_count(device) == 73 && extension->refs == 0);
    D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{};
    D3D12_STATE_OBJECT_DESC state_desc{};
    const IID pso_iid = __uuidof(ID3D12PipelineState), state_iid = __uuidof(ID3D12StateObject);
    void* out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 86, word(&stream_desc), word(&pso_iid), word(&out))) == S_OK && !out);
    EXPECT(base->calls[47] == 0 && extension->calls[47] == 1 && extension->refs == 0);
    EXPECT(extension->args[47][0] == word(&stream_desc) && extension->args[47][1] == word(&pso_iid));
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 101, word(&state_desc), word(&state_iid), word(&out))) == S_OK && !out);
    EXPECT(base->calls[62] == 0 && extension->calls[62] == 1 && extension->refs == 0);
    EXPECT(extension->args[62][0] == word(&state_desc) && extension->args[62][1] == word(&state_iid));

    // A failing call still releases the QI reference and propagates the backend's HRESULT.
    extension->create_result = E_ABORT;
    EXPECT(hr_of(raw(device, 86, word(&stream_desc), word(&pso_iid), word(&out))) == E_ABORT);
    EXPECT(hr_of(raw(device, 101, word(&state_desc), word(&state_iid), word(&out))) == E_ABORT);
    EXPECT(base->calls[47] == 0 && base->calls[62] == 0 && extension->refs == 0);
    EXPECT(extension->calls[47] == 2 && extension->calls[62] == 2);

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 4096;
    desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
    D3D12_RESOURCE_ALLOCATION_INFO allocation{};
    EXPECT(raw(device, 108, word(&allocation), 1, 1, word(&desc), 0) == word(&allocation));
    EXPECT(allocation.SizeInBytes == 0x40000 && allocation.Alignment == 0x10000);
    EXPECT(base->calls[68] == 0 && extension->calls[68] == 1 && extension->refs == 0);
    EXPECT(extension->array[68][0] == D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    using CAbiAllocationFn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(
        D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT, const D3D12_RESOURCE_DESC*,
        D3D12_RESOURCE_ALLOCATION_INFO1*);
    const auto table = *reinterpret_cast<void* const* const*>(device);
    allocation = {};
    EXPECT(reinterpret_cast<CAbiAllocationFn>(table[108])(&allocation, device, 1, 1, &desc, nullptr) == &allocation);
    EXPECT(allocation.SizeInBytes == 0x40000 && allocation.Alignment == 0x10000);
    EXPECT(base->calls[68] == 0 && extension->calls[68] == 2 && extension->refs == 0);
    EXPECT(release(device) == 0 && base->refs == 0);
    g_device_identity = g_device_extension = nullptr;
    std::printf("%s: slots 86/101/108 use distinct QI pointers, preserve results and balance references\n", g_label);
    return 0;
}

// ---- 2. Fences with and without ID3D12Fence1 ----

int check_fence1() {
    for (const bool fence1 : {false, true}) {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, fence1);
        EXPECT(device);
        const unsigned long device_refs = xs::device_proxy_ref_count(device);
        void* fence = make_fence(device);
        EXPECT(xs::is_fence_proxy(fence) && xs::fence_proxy_slot_count(fence) == (fence1 ? 12u : 11u));
        Mock* const fence_mock = mock->last_child;
        void* as_fence1 = nullptr;
        const HRESULT qi = hr_of(raw(fence, 0, word(&kFence1Iid), word(&as_fence1)));
        if (fence1) {
            EXPECT(qi == S_OK && as_fence1 == fence && release(as_fence1) == 1);
        } else {
            EXPECT(qi == E_NOINTERFACE && !as_fence1);
        }
        EXPECT(raw(fence, 8) == kCompletedValue);  // GetCompletedValue forwarded
        const HRESULT slot11 = hr_of(raw(fence, 11));
        if (fence1) {
            EXPECT(slot11 == S_OK && fence_mock->calls[11] == 1);
        } else {
            EXPECT(slot11 == E_NOTIMPL && fence_mock->calls[11] == 0);  // not forwarded past an 11-slot vtable
        }
        EXPECT(release(fence) == 0 && fence_mock->refs == 0);

        // Asked for as ID3D12Fence1: made as ID3D12Fence, then refused when the fence lacks Fence1, and released.
        void* created1 = nullptr;
        const HRESULT made1 = hr_of(raw(device, 36, 0, 0, word(&kFence1Iid), word(&created1)));
        if (fence1) {
            EXPECT(made1 == S_OK && xs::is_fence_proxy(created1) && release(created1) == 0);
        } else {
            EXPECT(made1 == E_NOINTERFACE && !created1);
        }
        EXPECT(mock->last_child->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: fences without and with ID3D12Fence1: 11/12 slots, QueryInterface, slot 11 and creation as "
                "Fence1 refused or forwarded to match\n", g_label);
    return 0;
}

// ---- Fence slot 12: the completed value at an address ----

std::atomic<int> g_descriptor_calls{0};
void WINAPI descriptor_callback() { g_descriptor_calls.fetch_add(1); }

// True once *completed reads `value`, within five seconds (the proxy's wait thread writes it).
bool reads_within(const volatile std::uint64_t* completed, std::uint64_t value) {
    for (int i = 0; i < 5000; ++i) {
        if (*completed == value) return true;
        Sleep(1);
    }
    return *completed == value;
}

bool fence_armed_for(Mock* fence, std::uint64_t value) {
    AcquireSRWLockExclusive(&fence->lock);
    const bool armed = fence->armed_event && fence->armed_value == value;
    ReleaseSRWLockExclusive(&fence->lock);
    return armed;
}

int check_fence_completed_value() {
    for (const bool fence1 : {false, true}) {  // past ID3D12Fence1 either way
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, fence1);
        EXPECT(device);
        void* fence = make_fence(device);
        Mock* const fence_mock = mock->last_child;
        void* other = make_fence(device);
        Mock* const other_mock = mock->last_child;
        EXPECT(fence && other && fence_mock != other_mock);

        // The title's shape: (value, descriptor with a code pointer in its second word, flags).
        const std::uint64_t descriptor[4] = {0, reinterpret_cast<std::uint64_t>(&descriptor_callback), 0, 0};
        const auto* completed =
            reinterpret_cast<const volatile std::uint64_t*>(raw(fence, 12, 9, word(descriptor), 1));
        EXPECT(completed && *completed == kCompletedValue);
        EXPECT(xs::fence_slot_first_call_seen(12));
        EXPECT(raw(fence, 12, 9, word(descriptor), 1) == word(completed));  // one address per fence
        const auto* other_completed = reinterpret_cast<const volatile std::uint64_t*>(raw(other, 12, 0, 0, 0));
        EXPECT(other_completed && other_completed != completed);

        // A CPU Signal through the proxy shows at once.
        EXPECT(hr_of(raw(fence, 10, 9)) == S_OK && fence_mock->completed == 9);
        EXPECT(*completed == 9 && *other_completed == kCompletedValue);
        // A signal the proxy never sees (a queue's, on the GPU) shows without another call, and again after it.
        fence_signal(fence_mock, 12);
        EXPECT(reads_within(completed, 12));
        fence_signal(fence_mock, 40);
        EXPECT(reads_within(completed, 40));
        fence_signal(other_mock, 8);
        EXPECT(reads_within(other_completed, 8) && *completed == 40);
        // Wait for the wait thread to arm its next threshold before rewinding.
        const ULONGLONG until = GetTickCount64() + 1000;
        while (!fence_armed_for(fence_mock, 41) && GetTickCount64() < until) Sleep(1);
        EXPECT(fence_armed_for(fence_mock, 41));
        EXPECT(hr_of(raw(fence, 10, 3)) == S_OK && *completed == 3);
        fence_signal(fence_mock, 15);
        EXPECT(fence_completed(fence_mock) == 15);
        if (!reads_within(completed, 15)) {
            std::fprintf(stderr, "rewind: underlying fence %llu, slot 12 mirror %llu\n",
                         static_cast<unsigned long long>(fence_completed(fence_mock)),
                         static_cast<unsigned long long>(*completed));
            return 1;
        }
        // A rewind by a producer outside the proxy can go straight below its outstanding event.
        fence_signal(fence_mock, 2);
        EXPECT(reads_within(completed, 2));
        fence_signal(fence_mock, 7);
        EXPECT(reads_within(completed, 7));
        // The fallback must leave the pending completion event alone.
        EXPECT(fence_armed_for(fence_mock, 41));
        fence_signal(fence_mock, 45);
        EXPECT(reads_within(completed, 45));
        // Device removal: the real fence reports ~0, which the title tests for, and nothing is armed after it.
        fence_signal(fence_mock, ~std::uint64_t{0});
        EXPECT(reads_within(completed, ~std::uint64_t{0}));
        Sleep(50);
        AcquireSRWLockExclusive(&fence_mock->lock);
        const bool rearmed = fence_mock->armed_event != nullptr;
        ReleaseSRWLockExclusive(&fence_mock->lock);
        EXPECT(!rearmed);
        EXPECT(g_descriptor_calls.load() == 0);  // the descriptor's code pointer is not called

        // Release waits out the wait thread; both fences and the device go back to zero references.
        EXPECT(release(fence) == 0 && fence_mock->refs == 0);
        EXPECT(release(other) == 0 && other_mock->refs == 0);

        // A fence whose slot 12 is never called arms nothing on the real fence.
        void* quiet = make_fence(device);
        Mock* const quiet_mock = mock->last_child;
        EXPECT(quiet && hr_of(raw(quiet, 10, 3)) == S_OK && raw(quiet, 8) == 3 && quiet_mock->calls[9] == 0);
        EXPECT(release(quiet) == 0 && quiet_mock->refs == 0);

        // Out of memory for the mirror: slot 12 returns null, and the next call works.
        void* starved = make_fence(device);
        EXPECT(starved);
        g_fail_news = 1;
        const std::uint64_t none = raw(starved, 12, 0, 0, 0);
        g_fail_news = 0;
        EXPECT(none == 0);
        const auto* later = reinterpret_cast<const volatile std::uint64_t*>(raw(starved, 12, 0, 0, 0));
        EXPECT(later && *later == kCompletedValue);
        EXPECT(release(starved) == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: fence slot 12 returns one address per fence whose completed value follows CPU and unseen "
                "signals up to device removal; the descriptor is not called\n", g_label);
    return 0;
}

// ---- 3. Creation failures and refused interfaces ----

int check_create_failures() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* out = reinterpret_cast<void*>(1);

    mock->create_null = true;  // S_OK with no object
    mock->create_result = S_OK;
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))) == E_FAIL && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 36, 0, 0, word(&kFenceIid), word(&out))) == E_FAIL && !out);
    mock->create_null = false;
    mock->create_result = E_OUTOFMEMORY;  // the real failure, passed on
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))) == E_OUTOFMEMORY && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 75, word(&kQueueDesc), 0, word(&kQueueIid), word(&out))) == E_OUTOFMEMORY && !out);
    mock->create_result = S_OK;

    const unsigned calls8 = mock->calls[8];
    out = reinterpret_cast<void*>(1);  // a null IID: E_POINTER, the output cleared, nothing created
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), 0, word(&out))) == E_POINTER && !out && mock->calls[8] == calls8);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 36, 0, 0, 0, word(&out))) == E_POINTER && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 75, word(&kQueueDesc), 0, 0, word(&out))) == E_POINTER && !out);
    // No output: the device's own answer (S_FALSE here), as the PC runtime gives it.
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), 0)) == S_FALSE && mock->calls[8] == calls8 + 1);
    EXPECT(hr_of(raw(device, 36, 0, 0, word(&kFenceIid), 0)) == S_FALSE);

    // An interface the real queue implements but the proxy does not model: created, refused, released.
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kExtraIid), word(&out))) == E_NOINTERFACE && !out);
    EXPECT(mock->last_child->refs == 0);
    void* queue = make_queue(device);
    EXPECT(queue);
    Mock* const queue_mock = mock->last_child;
    void* extra = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(queue, 0, word(&kExtraIid), word(&extra))) == E_NOINTERFACE && !extra);
    EXPECT(xs::command_object_proxy_ref_count(queue) == 1 && queue_mock->refs == 1);
    EXPECT(release(queue) == 0 && queue_mock->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: creation with no object (E_FAIL), a failing device, null IID and output, and an interface the "
                "proxy does not model leave no reference\n", g_label);
    return 0;
}

// ---- 4. OpenSharedHandle ----

int check_open_shared() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, false);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);

    for (const IID* iid : {&kFenceIid, &kUnknownIid}) {  // a fence, whatever it is asked for as, comes back wrapped
        void* opened = nullptr;
        EXPECT(hr_of(raw(device, 32, kFenceHandle, word(iid), word(&opened))) == S_OK);
        Mock* const fence_mock = mock->last_child;
        EXPECT(xs::is_fence_proxy(opened) && word(xs::fence_proxy_real_fence(opened)) == word(fence_mock));
        EXPECT(xs::command_object_proxy_ref_count(opened) == 1 && fence_mock->refs == 1);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs + 1);
        EXPECT(release(opened) == 0 && fence_mock->refs == 0);
    }
    void* out = reinterpret_cast<void*>(1);  // as Fence1, which this fence lacks: the real open fails, passed on
    EXPECT(hr_of(raw(device, 32, kFenceHandle, word(&kFence1Iid), word(&out))) == E_NOINTERFACE && !out);
    EXPECT(mock->last_child->refs == 0);

    for (const IID* iid : {&kResourceIid, &kUnknownIid}) {  // not a fence: the real object, unchanged
        void* opened = nullptr;
        EXPECT(hr_of(raw(device, 32, kResourceHandle, word(iid), word(&opened))) == S_OK);
        Mock* const resource_mock = mock->last_child;
        EXPECT(opened == resource_mock && !xs::is_fence_proxy(opened) && !xs::is_queue_proxy(opened));
        EXPECT(resource_mock->refs == 1 && release(opened) == 0);
    }
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);

    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 32, 0xbad, word(&kFenceIid), word(&out))) == E_INVALIDARG && !out);
    EXPECT(hr_of(raw(device, 32, kFenceHandle, word(&kFenceIid), 0)) == S_FALSE);  // the device's own answer
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 32, kFenceHandle, 0, word(&out))) == E_POINTER && !out);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: OpenSharedHandle wraps a fence (as ID3D12Fence and IUnknown), passes a resource, a failed open, "
                "an invalid handle and a null output through, with balanced references\n", g_label);
    return 0;
}

// ---- 5. Allocation failures in the proxy ----

int check_alloc_failures() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    const std::uint64_t objects[2] = {word(queue), word(fence)};

    const auto failing = [&](auto call) {  // one allocation refused, the result of the call
        const int before = g_failed_news;
        g_fail_news = 1;
        const HRESULT hr = call();
        g_fail_news = 0;
        return g_failed_news == before + 1 ? hr : S_OK;
    };
    void* out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 36, 0, 0, word(&kFenceIid), word(&out))); }) == E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);  // the real fence was made and released
    out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))); }) ==
           E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);
    out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 32, kFenceHandle, word(&kFenceIid), word(&out))); }) ==
           E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);
    // Up to 16 entries the unwrapped copy is inline: an armed failure is not even reached, and the call goes on.
    g_fail_news = 1;
    const HRESULT inline_result = hr_of(raw(device, 34, 2, word(objects)));
    const bool untouched = g_fail_news == 1;
    g_fail_news = 0;
    EXPECT(inline_result == S_OK && untouched && mock->array[34][0] == word(xs::queue_proxy_real_queue(queue)));

    // A longer array is allocated; refused, the call returns E_OUTOFMEMORY and reaches no device.
    constexpr unsigned kLong = 32;
    std::uint64_t many[kLong], many_fences[kLong], values[kLong];
    D3D12_RESIDENCY_PRIORITY priorities[kLong] = {};
    for (unsigned i = 0; i < kLong; ++i) {
        many[i] = (i % 2 != 0) ? word(fence) : word(queue);
        many_fences[i] = word(fence);
        values[i] = 1;
    }
    const unsigned calls34 = mock->calls[34], calls45 = mock->calls[45], calls46 = mock->calls[46];
    const unsigned calls50 = mock->calls[50];
    EXPECT(failing([&] { return hr_of(raw(device, 34, kLong, word(many))); }) == E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 45, word(many_fences), word(values), kLong, 0, 0)); }) ==
           E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 46, kLong, word(many), word(priorities))); }) == E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 50, 0, kLong, word(many), word(fence), 1)); }) == E_OUTOFMEMORY);
    EXPECT(mock->calls[34] == calls34 && mock->calls[45] == calls45 && mock->calls[46] == calls46 &&
           mock->calls[50] == calls50);  // nothing reached the device
    // And without the failure the long array arrives unwrapped.
    EXPECT(hr_of(raw(device, 34, kLong, word(many))) == S_OK && mock->calls[34] == calls34 + 1);
    EXPECT(mock->array[34][0] == word(xs::queue_proxy_real_queue(queue)) &&
           mock->array[34][1] == word(xs::fence_proxy_real_fence(fence)));

    // The device proxy itself: on failure the caller keeps its reference.
    Mock* other = new_mock(Kind::device);
    other->refs = 1;
    void* second = reinterpret_cast<void*>(1);
    EXPECT(failing([&] {
               return xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(other), __uuidof(ID3D12Device),
                                              &second);
           }) == E_OUTOFMEMORY);
    EXPECT(!second && other->refs == 1);

    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    Mock* const fence_mock = reinterpret_cast<Mock*>(xs::fence_proxy_real_fence(fence));
    EXPECT(release(queue) == 0 && release(fence) == 0 && queue_mock->refs == 0 && fence_mock->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: a refused allocation in queue, fence, shared-fence and device proxy creation and in the four "
                "unwrapped arrays returns E_OUTOFMEMORY, reaches no device and leaks nothing\n", g_label);
    return 0;
}

// ---- 6. Command allocators (device slot 9) ----

int check_allocators() {
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    // An allocator that answers Leonida's tag (its lists resolve a proxy) comes back wrapped.
    {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, true, true);
        EXPECT(device);
        const unsigned long device_refs = xs::device_proxy_ref_count(device);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&allocator_iid), word(&allocator))) == S_OK);
        Mock* const real = mock->last_child;
        EXPECT(xs::is_allocator_proxy(allocator) && word(xs::allocator_proxy_real_allocator(allocator)) == word(real));
        EXPECT(mock->args[9][0] == D3D12_COMMAND_LIST_TYPE_DIRECT && real->refs == 1);
        EXPECT(xs::allocator_proxy_slot_count(allocator) == 9 && xs::device_proxy_ref_count(device) == device_refs + 1);
        // Reset (slot 8) reaches the real allocator; slot 9 (SetAllocatorCallbackX) records callback/context and
        // succeeds without touching the real vtable; every slot past 9 up to the table end is refused.
        EXPECT(hr_of(raw(allocator, 8)) == S_OK && real->calls[8] == 1);
        EXPECT(xs::allocator_proxy_callback(allocator) == 0 && xs::allocator_proxy_callback_context(allocator) == 0);
        EXPECT(xs::allocator_proxy_callback(nullptr) == 0 && xs::allocator_proxy_callback_context(device) == 0);
        constexpr std::uintptr_t fake_cb = 0x12345678u;
        constexpr std::uintptr_t fake_ctx = 0x9abcdef0u;
        const std::uint64_t slot9_hits_before = xs::allocator_slot_hits(9);
        EXPECT(hr_of(raw(allocator, 9, fake_cb, fake_ctx, 0, 0xfeedfaceu)) == S_OK && real->calls[9] == 0);
        EXPECT(xs::allocator_slot_hits(9) == slot9_hits_before + 1 && xs::allocator_slot_first_call_seen(9));
        EXPECT(xs::allocator_proxy_callback(allocator) == fake_cb &&
               xs::allocator_proxy_callback_context(allocator) == fake_ctx);
        EXPECT(hr_of(raw(allocator, 9, 0, 0)) == S_OK && real->calls[9] == 0);
        EXPECT(xs::allocator_proxy_callback(allocator) == 0 && xs::allocator_proxy_callback_context(allocator) == 0);
        for (unsigned slot = 10; slot < xs::allocator_proxy_table_size(); ++slot)
            EXPECT(hr_of(raw(allocator, slot)) == E_NOTIMPL && real->calls[slot] == 0);
        // The tag and the real object stay hidden: QueryInterface answers only the modelled interfaces.
        void* found = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(allocator, 0, word(&xs::kLeonidaCommandAllocatorTag), word(&found))) == E_NOINTERFACE &&
               !found);
        EXPECT(hr_of(raw(allocator, 0, word(&allocator_iid), word(&found))) == S_OK && found == allocator);
        EXPECT(release(found) == 1);
        // An allocator is a pageable: MakeResident gives the real runtime the real allocator.
        const std::uint64_t objects[1] = {word(allocator)};
        EXPECT(hr_of(raw(device, 34, 1, word(objects))) == S_OK && mock->array[34][0] == word(real));
        EXPECT(release(allocator) == 0 && real->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        // Null IID, null output, a failing or empty creation, an IID the allocator lacks.
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, 0, word(&out))) == E_POINTER && !out);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), 0)) == S_FALSE);  // the device's own answer
        mock->create_result = E_OUTOFMEMORY;
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out))) == E_OUTOFMEMORY && !out);
        mock->create_result = S_OK;
        mock->create_null = true;
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out))) == E_FAIL && !out);
        mock->create_null = false;
        const IID fence_iid = __uuidof(ID3D12Fence);
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&fence_iid), word(&out))) == E_NOINTERFACE && !out);
        EXPECT(mock->last_child->refs == 0);
        // A refused allocation for the proxy: E_OUTOFMEMORY, and the real allocator is released.
        g_fail_news = 1;
        out = reinterpret_cast<void*>(1);
        const HRESULT starved = hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out)));
        g_fail_news = 0;
        EXPECT(starved == E_OUTOFMEMORY && !out && mock->last_child->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    // An allocator without the tag (a PC runtime allocator, whose lists could not resolve a proxy) comes back as it
    // is, as the interface asked for.
    {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, true, false);
        EXPECT(device);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
        EXPECT(allocator == mock->last_child && !xs::is_allocator_proxy(allocator) && mock->last_child->refs == 1);
        Mock* const real = mock->last_child;
        EXPECT(release(allocator) == 0 && real->refs == 0);
        const IID fence_iid = __uuidof(ID3D12Fence);
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&fence_iid), word(&out))) == E_NOINTERFACE && !out);
        EXPECT(mock->last_child->refs == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: allocators answering Leonida's tag are wrapped (Reset forwarded, tail refused, residency "
                "unwrapped, references balanced); others come back unwrapped; failures leave no reference\n", g_label);
    return 0;
}

// ---- 10. Device slot 25, GetResourceAllocationInfo: a render target takes whole console tiles ----

D3D12_RESOURCE_DESC target_desc(std::uint64_t width, UINT height, DXGI_FORMAT format, UINT slices = 1) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = static_cast<UINT16>(slices);
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);  // the Xbox tiled layout word
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return d;
}

int check_render_target_sizes() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 0, true);
    EXPECT(device);
    void* const* vtable = *reinterpret_cast<void* const* const*>(device);
    // The retail executable's call: the buffer for the struct in RCX, the device in RDX.
    using ConsoleFn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT,
                                                               const D3D12_RESOURCE_DESC*);
    const auto get_info = reinterpret_cast<ConsoleFn>(vtable[25]);
    auto ask = [&](const D3D12_RESOURCE_DESC* descs, UINT count) {
        D3D12_RESOURCE_ALLOCATION_INFO info{0xDEAD, 0xBEEF};
        const bool same_buffer = get_info(&info, device, 0, count, descs) == &info;
        if (!same_buffer) info.SizeInBytes = 0;
        return info;
    };

    // A 1920 x 1080 RGBA8 target. A 4-byte element tile is 128 x 128 pixels: 15 tiles across, 9 down (1080 / 128 is
    // 8.4), 135 x 64 KiB = 8,847,360 B. The PC device packs it as 8,294,400 B, 8,323,072 B rounded to 64 KiB.
    const D3D12_RESOURCE_DESC hd = target_desc(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM);
    D3D12_RESOURCE_ALLOCATION_INFO info = ask(&hd, 1);
    EXPECT(info.SizeInBytes == 8847360 && info.Alignment == 65536);
    // A 3840 x 2160 RGBA16F target: an 8-byte element tile is 128 x 64, 30 across and 34 down (2160 / 64 is 33.75),
    // 1020 tiles = 66,846,720 B, where the PC device says 66,387,968 B.
    const D3D12_RESOURCE_DESC uhd = target_desc(3840, 2160, DXGI_FORMAT_R16G16B16A16_FLOAT);
    EXPECT(ask(&uhd, 1).SizeInBytes == 66846720);
    // Both in one call: the sum of the two console sizes.
    const D3D12_RESOURCE_DESC both[] = {hd, uhd};
    EXPECT(ask(both, 2).SizeInBytes == 8847360 + 66846720);
    // Two slices take two surfaces.
    const D3D12_RESOURCE_DESC two = target_desc(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 2);
    EXPECT(ask(&two, 1).SizeInBytes == 2 * 8847360);

    // What is left alone: a texture that is not a render target, a buffer, a render target of a format the rule does
    // not list, one the PC device refuses, and a surface that is already whole tiles (1024 x 1024 RGBA8).
    D3D12_RESOURCE_DESC sampled = hd;
    sampled.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    EXPECT(ask(&sampled, 1).SizeInBytes == 8323072);
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 100000;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    EXPECT(ask(&buffer, 1).SizeInBytes == 131072);
    D3D12_RESOURCE_DESC odd_format = hd;
    odd_format.Format = DXGI_FORMAT_BC1_UNORM;
    EXPECT(ask(&odd_format, 1).SizeInBytes == 8323072);
    D3D12_RESOURCE_DESC empty = hd;
    empty.Width = 0;
    EXPECT(ask(&empty, 1).SizeInBytes == UINT64_MAX);
    const D3D12_RESOURCE_DESC exact = target_desc(1024, 1024, DXGI_FORMAT_R8G8B8A8_UNORM);
    EXPECT(ask(&exact, 1).SizeInBytes == 4194304);
    // A refused description in a list refuses the list, as before.
    const D3D12_RESOURCE_DESC refused[] = {hd, empty};
    EXPECT(ask(refused, 2).SizeInBytes == UINT64_MAX);
    // A mip chain: 1920 x 1080 with 3 mips is mip 0 in 135 tiles, mip 1 (960 x 540) in 8 x 5 = 40 tiles and mip 2
    // (480 x 270) in 4 x 3 = 12 tiles.
    D3D12_RESOURCE_DESC mips = hd;
    mips.MipLevels = 3;
    EXPECT(ask(&mips, 1).SizeInBytes == (135 + 40 + 12) * 65536ull);

    // The PC form (this in RCX, the buffer in RDX) answers the same sizes.
    D3D12_RESOURCE_ALLOCATION_INFO pc{};
    EXPECT(raw(device, 25, word(&pc), 0, 1, word(&hd)) == word(&pc));
    EXPECT(pc.SizeInBytes == 8847360);

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slot 25 pads render targets to whole console tiles and leaves the other sizes as the PC "
                "device gives them\n", g_label);
    return 0;
}

// ---- 7b. Device slot 25 hands the PC device clean descriptions and leaves the title's array alone ----

int check_allocation_descriptions_cleaned() {
    constexpr std::uint32_t kXboxTiledLayout = 0x101, kXboxRowMajorLayout = 0x1, kXboxFlagBits = 0x40000 | 0x20000;
    constexpr D3D12_RESOURCE_FLAGS kPcFlags =
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Mock* mock = nullptr;
    void* device = make_device(mock, 0, true);
    EXPECT(device);
    void* const* vtable = *reinterpret_cast<void* const* const*>(device);
    using ConsoleFn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT,
                                                               const D3D12_RESOURCE_DESC*);
    const auto get_info = reinterpret_cast<ConsoleFn>(vtable[25]);

    // A texture with the Xbox tiled layout word and Xbox bits above the PC flags, and a buffer with the row-major word.
    D3D12_RESOURCE_DESC titles[2] = {target_desc(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM), {}};
    titles[0].Flags = static_cast<D3D12_RESOURCE_FLAGS>(kPcFlags | kXboxFlagBits);
    titles[0].Layout = static_cast<D3D12_TEXTURE_LAYOUT>(kXboxTiledLayout);
    titles[1].Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    titles[1].Width = 100000;
    titles[1].Height = 1;
    titles[1].DepthOrArraySize = 1;
    titles[1].MipLevels = 1;
    titles[1].SampleDesc.Count = 1;
    titles[1].Layout = static_cast<D3D12_TEXTURE_LAYOUT>(kXboxRowMajorLayout);
    titles[1].Flags = static_cast<D3D12_RESOURCE_FLAGS>(kXboxFlagBits);
    D3D12_RESOURCE_DESC kept[2];
    std::memcpy(kept, titles, sizeof(kept));

    D3D12_RESOURCE_ALLOCATION_INFO info{};
    g_seen_desc_count = 0;
    g_seen_calls = 0;
    std::memset(g_seen_descs, 0xCC, sizeof(g_seen_descs));
    EXPECT(get_info(&info, device, 0, 2, titles) == &info);
    // The PC device saw both descriptions with PC flags and a PC layout, the rest of each as the title wrote it.
    EXPECT(g_seen_desc_count == 2);
    EXPECT(g_seen_descs[0].Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN && g_seen_descs[0].Flags == kPcFlags);
    EXPECT(g_seen_descs[0].Width == 1920 && g_seen_descs[0].Height == 1080 &&
           g_seen_descs[0].Format == DXGI_FORMAT_R8G8B8A8_UNORM);
    EXPECT(g_seen_descs[1].Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR && g_seen_descs[1].Flags == D3D12_RESOURCE_FLAG_NONE);
    EXPECT(g_seen_descs[1].Width == 100000);
    // The title's array is as it was, and the size still follows the render-target flag it kept (the proxy's later
    // per-target questions to the PC device are not recorded).
    EXPECT(std::memcmp(kept, titles, sizeof(kept)) == 0);
    EXPECT(info.SizeInBytes == 8847360 + 131072);

    // PC calls must keep their layout and flags so the host can reject an invalid descriptor.
    using PcFn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(void*, D3D12_RESOURCE_ALLOCATION_INFO*, UINT, UINT,
                                                          const D3D12_RESOURCE_DESC*);
    const auto get_pc_info = reinterpret_cast<PcFn>(vtable[25]);
    D3D12_RESOURCE_DESC pc_buffer = titles[1];
    pc_buffer.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    pc_buffer.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x1000);
    g_seen_calls = 0;
    EXPECT(get_pc_info(device, &info, 0, 1, &pc_buffer) == &info);
    EXPECT(g_seen_desc_count == 1 && g_seen_descs[0].Layout == D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE);
    EXPECT(g_seen_descs[0].Flags == pc_buffer.Flags);

    // A null array with a zero count reaches the PC device as it came.
    g_seen_desc_count = 99;
    g_seen_calls = 0;
    EXPECT(get_info(&info, device, 0, 0, nullptr) == &info && g_seen_calls == 1 && g_seen_desc_count == 0);

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slot 25 gives the PC device descriptions with PC flags and layout, and the title's own "
                "array is not written\n", g_label);
    return 0;
}

// ---- 11. Console render target layout: alignment, per-description offsets, mips, formats, overflow ----

int check_render_target_layout() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    void* const* vtable = *reinterpret_cast<void* const* const*>(device);
    // The retail executable's calls: the buffer for the struct in RCX, the device in RDX.
    using InfoFn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT,
                                                            const D3D12_RESOURCE_DESC*);
    using Info1Fn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT,
                                                             const D3D12_RESOURCE_DESC*,
                                                             D3D12_RESOURCE_ALLOCATION_INFO1*);
    const auto get_info = reinterpret_cast<InfoFn>(vtable[25]);
    const auto get_info1 = reinterpret_cast<Info1Fn>(vtable[56]);
    auto ask = [&](const D3D12_RESOURCE_DESC* descs, UINT count) {
        D3D12_RESOURCE_ALLOCATION_INFO info{0xDEAD, 0xBEEF};
        if (get_info(&info, device, 0, count, descs) != &info) info = {0, 0};
        return info;
    };
    auto ask_one = [&](const D3D12_RESOURCE_DESC& d) { return ask(&d, 1); };
    auto target = [](std::uint64_t w, UINT h, DXGI_FORMAT f, UINT slices, UINT mips) {
        D3D12_RESOURCE_DESC d = target_desc(w, h, f, slices);
        d.MipLevels = static_cast<UINT16>(mips);
        return d;
    };

    // A four-sample 1 x 1 target is 4 MiB at 4 MiB alignment. Next to a 1080p target that grows to 8,847,360 B, the
    // two need 4 MiB + 8,847,360 B = 13,041,664 B placed, and the total is a whole number of 4 MiB: 16 MiB, in
    // either order. (The PC device says 12 MiB for them; adding the 512 KiB the padding takes would say 12.5 MiB.)
    const D3D12_RESOURCE_DESC hd = target_desc(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM);
    D3D12_RESOURCE_DESC msaa = target_desc(1, 1, DXGI_FORMAT_R8G8B8A8_UNORM);
    msaa.SampleDesc.Count = 4;
    const D3D12_RESOURCE_DESC msaa_first[] = {msaa, hd};
    const D3D12_RESOURCE_DESC msaa_last[] = {hd, msaa};
    D3D12_RESOURCE_ALLOCATION_INFO info = ask(msaa_first, 2);
    EXPECT(info.SizeInBytes == 16777216 && info.Alignment == 4194304);
    info = ask(msaa_last, 2);
    EXPECT(info.SizeInBytes == 16777216 && info.Alignment == 4194304);
    EXPECT(ask(&msaa, 1).SizeInBytes == 4194304);  // multisampled targets are left as the PC device sizes them

    // GetResourceAllocationInfo1: the offsets follow the padded sizes, in the console form and the PC form.
    const D3D12_RESOURCE_DESC two[] = {hd, hd};
    D3D12_RESOURCE_ALLOCATION_INFO1 each[2]{};
    D3D12_RESOURCE_ALLOCATION_INFO total{};
    EXPECT(get_info1(&total, device, 0, 2, two, each) == &total);
    EXPECT(total.SizeInBytes == 17694720 && total.Alignment == 65536);
    EXPECT(each[0].Offset == 0 && each[0].SizeInBytes == 8847360 && each[0].Alignment == 65536);
    EXPECT(each[1].Offset == 8847360 && each[1].SizeInBytes == 8847360 && each[1].Alignment == 65536);
    // The per-description array is optional: without one the total is the same 17,694,720 B.
    D3D12_RESOURCE_ALLOCATION_INFO no_array{};
    EXPECT(get_info1(&no_array, device, 0, 2, two, nullptr) == &no_array);
    EXPECT(no_array.SizeInBytes == 17694720 && no_array.Alignment == 65536);
    D3D12_RESOURCE_ALLOCATION_INFO1 mixed[2]{};
    D3D12_RESOURCE_ALLOCATION_INFO pc{};
    EXPECT(raw(device, 56, word(&pc), 0, 2, word(msaa_first), word(mixed)) == word(&pc));
    EXPECT(pc.SizeInBytes == 16777216 && pc.Alignment == 4194304);
    EXPECT(mixed[0].Offset == 0 && mixed[0].SizeInBytes == 4194304 && mixed[0].Alignment == 4194304);
    EXPECT(mixed[1].Offset == 4194304 && mixed[1].SizeInBytes == 8847360 && mixed[1].Alignment == 65536);
    // Nothing to pad: the PC device's answer stands, per-description array included.
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 100000;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_RESOURCE_DESC buffers[] = {buffer, buffer};
    D3D12_RESOURCE_ALLOCATION_INFO1 plain[2]{};
    EXPECT(get_info1(&total, device, 0, 2, buffers, plain) == &total);
    EXPECT(total.SizeInBytes == 262144 && plain[1].Offset == 131072 && plain[1].SizeInBytes == 131072);

    // Mips: 0 is the full chain. 256 x 128 RGBA8 takes two tiles for mip 0, one for mip 1 (128 x 64 is wider than half a
    // tile) and one tail tile for the rest.
    EXPECT(ask_one(target(256, 128, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0)).SizeInBytes ==
           4 * 65536ull);
    // 128 x 32 with two mips and two slices: mip 0 is a whole tile, mip 1 (64 x 16) is the tail tile, per slice.
    EXPECT(ask_one(target(128, 32, DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2)).SizeInBytes ==
           4 * 65536ull);
    // 257 x 32 with two mips: mip 0 is 3 tiles across, mip 1 is 129 x 16 (an odd extent rounds up): 2 tiles.
    EXPECT(ask_one(target(257, 32, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 2)).SizeInBytes ==
           5 * 65536ull);
    // 2-byte B5G5R5A1 is a 256 x 128 tile: 257 x 64 takes two.
    EXPECT(ask_one(target(257, 64, DXGI_FORMAT_B5G5R5A1_UNORM, 1, 1)).SizeInBytes ==
           2 * 65536ull);
    // 16-byte RGBA32F is a 64 x 64 tile: 65 x 32 takes two.
    EXPECT(ask_one(target(65, 32, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1)).SizeInBytes ==
           2 * 65536ull);

    // Sums saturate where the PC device's do. A 2^26 x 2^26 RGBA32F target of 200 slices is 200 x 2^56 B (the PC mock
    // caps it at 4 GiB); two of them are past 2^64.
    const D3D12_RESOURCE_DESC huge = target(1ull << 26, 1u << 26, DXGI_FORMAT_R32G32B32A32_FLOAT, 200, 1);
    EXPECT(ask(&huge, 1).SizeInBytes == 200ull << 56);
    const D3D12_RESOURCE_DESC huge_two[] = {huge, huge};
    info = ask(huge_two, 2);
    EXPECT(info.SizeInBytes == 0xFFFFFFFFFFFF0000ull && info.Alignment == 65536);

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: render targets are laid out at their own alignments, with per-description offsets, mip chains "
                "and saturating sums\n", g_label);
    return 0;
}

// ---- 7. Xbox device slots from the public HLE (48, 73, 78, 79, 80) and the retail title (41, 61) ----
//
// A device that answers ID3D12Device11 (80 PC slots) is the Vulkan layer's shape: slots 78/79 are inside its table
// (CreateReservedResource2/CreateSampler2 there) and 80 is just past it. None of the five may reach the mock's own
// slot: the mock's call counter proves the route, and the returned token/queue/list proves the handler ran.
int check_xbox_slots() {
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    Mock* mock = nullptr;
    // Full-chain lists: the console IID maps to ID3D12GraphicsCommandList6, which a command_list-kind mock
    // does not answer.
    void* device = make_device(mock, 11, true, true, true);
    EXPECT(device && xs::device_proxy_slot_count(device) == 80);
    xs::reset_device_proxy_stats();

    // 78 SetFrameIntervalX(output, interval_us, latency, flags): S_OK, not forwarded to PC slot 78.
    EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK);
    EXPECT(mock->calls[78] == 0 && xs::device_slot_hits(78) == 1);
    // 79 ScheduleFrameEventX(event, offset_us, pEvent, flags): S_OK, not forwarded to PC slot 79.
    EXPECT(hr_of(raw(device, 79, 0, 0, 0x1234, 0)) == S_OK);
    EXPECT(mock->calls[79] == 0 && xs::device_slot_hits(79) == 1);

    // 80 WaitFrameEventX(event, timeout_ms, pEvent, flags, &token): a fresh non-zero token each time; a null token
    // pointer is E_POINTER; the call never blocks (this test would hang otherwise).
    std::uint64_t token = 0, token2 = 0;
    EXPECT(hr_of(raw(device, 80, 0, 0xffffffffu, 0, 0, word(&token))) == S_OK && token != 0);
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, word(&token2))) == S_OK && token2 != 0 && token2 != token);
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, 0)) == E_POINTER);
    EXPECT(mock->calls[80] == 0 && xs::device_slot_hits(80) == 3);

    // 48 CreateCommandQueueX(desc, riid, out): the queue proxy, created through the PC slot 8 path (filtered desc copy).
    void* queue = nullptr;
    EXPECT(hr_of(raw(device, 48, word(kXboxQueueDesc), word(&kQueueIid), word(&queue))) == S_OK);
    EXPECT(xs::is_queue_proxy(queue) && mock->calls[48] == 0 && mock->calls[8] == 1);

    // 73 CreateCommandListX(desc, allocator, initialState, riid, out): the type is desc's first word, the allocator
    // proxy is unwrapped, the node mask is 0 and the PC six-argument CreateCommandList (slot 12) is reached.
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
    EXPECT(xs::is_allocator_proxy(allocator));
    Mock* const real_allocator = reinterpret_cast<Mock*>(xs::allocator_proxy_real_allocator(allocator));
    struct XboxCommandListDesc {
        std::uint32_t type;
        std::uint32_t node_mask;
        std::uint64_t reserved[2];
    } desc{};
    desc.type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.node_mask = 7;  // ignored by the console call, as the HLE does
    const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
    void* list = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&list))) == S_OK);
    // A console device answers with the console-shaped list proxy over the real list, as slot 12 does.
    EXPECT(xs::is_list_proxy(list) &&
           reinterpret_cast<Mock*>(xs::list_proxy_real_list(list))->kind == Kind::list);
    EXPECT(mock->calls[73] == 0 && mock->calls[12] == 1 && xs::device_slot_hits(73) == 1);
    EXPECT(mock->args[12][0] == 0 && mock->args[12][1] == D3D12_COMMAND_LIST_TYPE_DIRECT);
    EXPECT(mock->args[12][2] == word(real_allocator) && mock->args[12][3] == 0);
    // The real list is created as ID3D12GraphicsCommandList; the console IID the caller asked for is answered by
    // the proxy's QueryInterface instead.
    EXPECT(mock->args[12][4] == word(&__uuidof(ID3D12GraphicsCommandList)));

    // The measured console list IID (the 1.0.2.1 executable's only GUID of that form) maps to
    // ID3D12GraphicsCommandList6: slot 73 answers it, and a QueryInterface for it on the proxy returns the proxy.
    const IID console_list_iid = {0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
    void* console_list = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&console_list_iid),
                     word(&console_list))) == S_OK);
    EXPECT(xs::is_list_proxy(console_list));
    EXPECT(mock->calls[12] == 2);
    void* asked = nullptr;
    EXPECT(hr_of(raw(console_list, 0, word(&console_list_iid), word(&asked))) == S_OK && asked == console_list);
    EXPECT(release(asked) == 1 && release(console_list) == 0);

    // Release the list before the refusal calls below: the COM convention (and the handler) nulls the caller's
    // output on failure, so an invalid call would clear this pointer.
    EXPECT(release(list) == 0);

    // Missing description, allocator or output: E_INVALIDARG/E_POINTER before the real device is called.
    EXPECT(hr_of(raw(device, 73, 0, word(allocator), 0, word(&list_iid), word(&list))) == E_INVALIDARG);
    EXPECT(hr_of(raw(device, 73, word(&desc), 0, 0, word(&list_iid), word(&list))) == E_INVALIDARG);
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), 0)) == E_POINTER);

    // 61 CreateSamplerX(desc, dest): the PC CreateSampler (slot 22) gets the same two words, not the PC slot 61
    // (CreateMetaCommand). A null description is skipped without reaching the device.
    D3D12_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    const std::uint64_t sampler_handle = 0x7ff012340040ull;
    raw(device, 61, word(&sampler), sampler_handle);
    EXPECT(mock->calls[61] == 0 && mock->calls[22] == 1 && xs::device_slot_hits(61) == 1);
    EXPECT(mock->args[22][0] == word(&sampler) && mock->args[22][1] == sampler_handle);
    raw(device, 61, 0, sampler_handle);
    EXPECT(mock->calls[61] == 0 && mock->calls[22] == 1 && xs::device_slot_hits(61) == 2);

    // 41 CreateCommandSignature: a console argument of type 105 reaches the PC slot 41 as DISPATCH, in a copy of the
    // description (stride, count, the root argument before it and the root signature kept). A PC description is
    // passed as the same pointer.
    D3D12_INDIRECT_ARGUMENT_DESC console_args[2]{};
    console_args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    console_args[0].Constant.Num32BitValuesToSet = 1;
    constexpr std::uint32_t kType105 = 105;  // outside the enum's range: written and read as its 32 bits
    std::memcpy(&console_args[1].Type, &kType105, sizeof(kType105));
    const D3D12_COMMAND_SIGNATURE_DESC console_sig{16, 2, console_args, 0};
    const IID sig_iid = __uuidof(ID3D12CommandSignature);
    void* sig = nullptr;
    EXPECT(hr_of(raw(device, 41, word(&console_sig), 0x5150, word(&sig_iid), word(&sig))) == S_OK);
    EXPECT(mock->calls[41] == 1 && xs::device_slot_hits(41) == 1);
    EXPECT(mock->args[41][0] != word(&console_sig) && mock->args[41][1] == 0x5150);
    EXPECT(mock->args[41][2] == word(&sig_iid) && mock->args[41][3] == word(&sig));
    EXPECT(mock->array[41][0] == (16 | std::uint64_t{2} << 32));
    EXPECT(mock->array[41][1] == D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT &&
           mock->array[41][2] == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH);
    std::uint32_t caller_type = 0;
    std::memcpy(&caller_type, &console_args[1].Type, sizeof(caller_type));
    EXPECT(caller_type == kType105);  // the caller's description is untouched
    D3D12_INDIRECT_ARGUMENT_DESC pc_arg{};
    pc_arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
    const D3D12_COMMAND_SIGNATURE_DESC pc_sig{12, 1, &pc_arg, 0};
    EXPECT(hr_of(raw(device, 41, word(&pc_sig), 0, word(&sig_iid), word(&sig))) == S_OK);
    EXPECT(mock->calls[41] == 2 && mock->args[41][0] == word(&pc_sig));
    EXPECT(mock->array[41][1] == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH);

    EXPECT(xs::device_slot_overridden(41) && xs::device_slot_overridden(48) && xs::device_slot_overridden(61) &&
           xs::device_slot_overridden(73) && xs::device_slot_overridden(78) && xs::device_slot_overridden(79) &&
           xs::device_slot_overridden(80) && xs::device_slot_overridden(86) && xs::device_slot_overridden(101) &&
           xs::device_slot_overridden(108));

    // 86 CreatePipelineState(desc, riid, out) -> PC slot 47 (ID3D12Device2::CreatePipelineState).
    D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{};
    const IID pso_iid = __uuidof(ID3D12PipelineState);
    void* pso_stream_out = nullptr;
    EXPECT(hr_of(raw(device, 86, word(&stream_desc), word(&pso_iid), word(&pso_stream_out))) == S_OK);
    EXPECT(mock->calls[86] == 0 && mock->calls[47] == 1 && xs::device_slot_hits(86) == 1);
    EXPECT(mock->args[47][0] == word(&stream_desc) && mock->args[47][1] == word(&pso_iid) &&
           mock->args[47][2] == word(&pso_stream_out));
    pso_stream_out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 86, word(&stream_desc), 0, word(&pso_stream_out))) == E_POINTER && !pso_stream_out);

    // 101 CreateStateObject(desc, riid, out) -> PC slot 62 (ID3D12Device5::CreateStateObject).
    D3D12_STATE_OBJECT_DESC state_obj_desc{};
    const IID so_iid = __uuidof(ID3D12StateObject);
    void* so_out = nullptr;
    EXPECT(hr_of(raw(device, 101, word(&state_obj_desc), word(&so_iid), word(&so_out))) == S_OK);
    EXPECT(mock->calls[101] == 0 && mock->calls[62] == 1 && xs::device_slot_hits(101) == 1);
    EXPECT(mock->args[62][0] == word(&state_obj_desc) && mock->args[62][1] == word(&so_iid) &&
           mock->args[62][2] == word(&so_out));
    so_out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 101, word(&state_obj_desc), 0, word(&so_out))) == E_POINTER && !so_out);

    // 108 GetResourceAllocationInfo2 -> PC slot 68 (ID3D12Device8::GetResourceAllocationInfo2), called with the
    // retail executable's C-ABI struct return (return buffer in RCX, device proxy in RDX), 56-byte D3D12_RESOURCE_DESC
    // entries, and an Xbox layout/flag word, as well as the PC C++ argument order.
    void* const* proxy_vtable = *reinterpret_cast<void* const* const*>(device);
    using CAbiSlot108Fn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(
        D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT, const D3D12_RESOURCE_DESC*,
        D3D12_RESOURCE_ALLOCATION_INFO1*);
    const auto call_slot108_c_abi = reinterpret_cast<CAbiSlot108Fn>(proxy_vtable[108]);
    D3D12_RESOURCE_DESC xbox_descs[2]{};
    xbox_descs[0].Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    xbox_descs[0].Width = 3840;
    xbox_descs[0].Height = 2160;
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
    xbox_descs[1].Flags = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | 0x200000u);
    D3D12_RESOURCE_ALLOCATION_INFO ret_info{};
    D3D12_RESOURCE_ALLOCATION_INFO1 detail_info[2]{};
    EXPECT(call_slot108_c_abi(&ret_info, device, 1, 2, xbox_descs, detail_info) == &ret_info);
    EXPECT(ret_info.SizeInBytes == 0x40000 && ret_info.Alignment == 0x10000);
    EXPECT(detail_info[0].SizeInBytes == 0x40000 && detail_info[0].Alignment == 0x10000);
    EXPECT(mock->calls[108] == 0 && mock->calls[68] == 1 && xs::device_slot_hits(108) == 1);
    EXPECT(mock->array[68][0] == static_cast<std::uint64_t>(D3D12_TEXTURE_LAYOUT_UNKNOWN));
    EXPECT(mock->array[68][1] == static_cast<std::uint64_t>(
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    EXPECT(mock->array[68][2] == 0);
    EXPECT(mock->array[68][3] == ((static_cast<std::uint64_t>(D3D12_TEXTURE_LAYOUT_ROW_MAJOR) << 32) |
                                  static_cast<std::uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)));

    D3D12_RESOURCE_DESC xbox_buf_desc = xbox_descs[1];
    xbox_buf_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x200000u);
    D3D12_RESOURCE_ALLOCATION_INFO buf_ret{};
    EXPECT(raw(device, 108, word(&buf_ret), 1, 1, word(&xbox_buf_desc), 0) == word(&buf_ret));
    EXPECT(buf_ret.SizeInBytes == 0x40000 && mock->calls[68] == 2 && xs::device_slot_hits(108) == 2);
    EXPECT(mock->array[68][0] == static_cast<std::uint64_t>(D3D12_TEXTURE_LAYOUT_ROW_MAJOR));
    EXPECT(mock->array[68][1] == static_cast<std::uint64_t>(D3D12_RESOURCE_FLAG_NONE));

    D3D12_RESOURCE_DESC batch1025[1025]{};
    for (int i = 0; i < 1024; ++i) batch1025[i] = xbox_descs[0];
    batch1025[1024] = xbox_descs[1];
    D3D12_RESOURCE_ALLOCATION_INFO batch_ret{};
    EXPECT(call_slot108_c_abi(&batch_ret, device, 1, 1025, batch1025, nullptr) == &batch_ret);
    EXPECT(batch_ret.SizeInBytes == 0x40000 && mock->calls[68] == 3 && xs::device_slot_hits(108) == 3);
    EXPECT(mock->array[68][0] == static_cast<std::uint64_t>(D3D12_TEXTURE_LAYOUT_UNKNOWN));
    EXPECT(mock->array[68][3] == ((static_cast<std::uint64_t>(D3D12_TEXTURE_LAYOUT_ROW_MAJOR) << 32) |
                                  static_cast<std::uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)));

    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(release(queue) == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slots 48/73/78/79/80 route to a queue, a list and frame-event tokens, slot 61 to "
                "CreateSampler and slot 41 creates argument type 105 as DISPATCH, without reaching the mock's own PC "
                "slots\n", g_label);
    return 0;
#else
    std::printf("%s: ID3D12Device11 is not in this SDK; Xbox slot 80's beyond-table case is not built\n", g_label);
    return 0;
#endif
}

// ---- 7b. Xbox queue ExecuteCommandLists variant (queue slot 35 -> PC slot 10) ----

int check_xbox_queue_slots() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    void* queue = make_queue(device);
    EXPECT(queue);
    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    xs::reset_device_proxy_stats();

    EXPECT(xs::queue_slot_overridden(35) && xs::queue_slot_overridden(14) && xs::queue_slot_overridden(15));
    EXPECT(!xs::queue_slot_overridden(10) && !xs::queue_slot_overridden(34) && !xs::queue_slot_overridden(36));

    // Two readable non-proxy objects: slot 35 unwraps list proxies as slot 10 does, so each entry's vtable is read.
    void* not_a_proxy_vtable[1] = {};
    void* objects[2][1] = {{not_a_proxy_vtable}, {not_a_proxy_vtable}};
    void* lists[2] = {objects[0], objects[1]};
    EXPECT(raw(queue, 35, (0xdeadbeefull << 32) | 2ull, word(lists)) == 0);
    EXPECT(xs::queue_slot_hits(35) == 1 && xs::queue_slot_first_call_seen(35));
    EXPECT(xs::queue_slot_hits(10) == 0);
    EXPECT(queue_mock->calls[35] == 0 && queue_mock->calls[10] == 1);
    EXPECT(queue_mock->args[10][0] == 2 && queue_mock->array[10][0] == word(lists[0]) &&
           queue_mock->array[10][1] == word(lists[1]));  // passed on unchanged, in an unwrapped copy

    EXPECT(raw(queue, 35, 0, 0) == 0);
    EXPECT(xs::queue_slot_hits(35) == 2 && queue_mock->calls[35] == 0 && queue_mock->calls[10] == 2);
    EXPECT(queue_mock->args[10][0] == 0 && queue_mock->args[10][1] == 0);

    EXPECT(release(queue) == 0 && release(device) == 0 && mock->refs == 0);
    std::printf("%s: queue slot 35 routes ExecuteCommandLists(count, lists) to PC slot 10 with UINT count masking\n",
                g_label);
    return 0;
}

// ---- 8. Environment overrides for the Xbox device slots (one process each, read once) ----

int run_env_slots(bool valid, bool on_slot70 = false, bool on_slot74 = false) {
    const char* frame_interval = on_slot70 ? "70" : valid ? "96" : "2";
    const char* schedule_event = valid ? "97" : "2";
    const char* wait_event = on_slot74 ? "74" : valid ? "98" : "2";
    const char* queue_x = valid ? "99" : "2";
    const char* list_x = valid ? "100" : "2";
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_FRAME_INTERVAL_SLOT", frame_interval) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_SCHEDULE_EVENT_SLOT", schedule_event) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_WAIT_EVENT_SLOT", wait_event) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_QUEUE_X_SLOT", queue_x) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_LIST_X_SLOT", list_x) != FALSE);

    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device);
    xs::reset_device_proxy_stats();
    std::uint64_t token = 0;
    if (on_slot70) {
        // SetFrameIntervalX moved onto 70, which also has a built-in handler: the override wins, so (output, interval,
        // latency, flags) is a frame-interval call, never a shader-resource view whose handle is the latency.
        EXPECT(hr_of(raw(device, 70, 0, 16667, 1, 0)) == S_OK && mock->calls[70] == 0 && mock->calls[18] == 0);
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 1);  // the old default is a PC slot now
    } else if (on_slot74) {
        // WaitFrameEventX moved onto 74, which also has a built-in handler: the override wins, so the call writes its
        // wait token through the last pointer, which the stand-in never does, and reaches no PC slot.
        std::uint64_t token_74 = 0;
        EXPECT(hr_of(raw(device, 74, 0, 0, 0, 0, word(&token_74))) == S_OK && token_74 && mock->calls[74] == 0);
    } else if (valid) {
        // Every route moved: the new slot is handled, the default slot is forwarded to the mock.
        EXPECT(hr_of(raw(device, 96, 0, 16667, 1, 0)) == S_OK && mock->calls[96] == 0);
        EXPECT(hr_of(raw(device, 97, 0, 0, 0, 0)) == S_OK && mock->calls[97] == 0);
        EXPECT(hr_of(raw(device, 98, 0, 0, 0, 0, word(&token))) == S_OK && token && mock->calls[98] == 0);
        void* queue = nullptr;
        EXPECT(hr_of(raw(device, 99, word(kXboxQueueDesc), word(&kQueueIid), word(&queue))) == S_OK);
        EXPECT(xs::is_queue_proxy(queue) && mock->calls[99] == 0);
        const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
        struct {
            std::uint32_t type, node_mask;
        } desc{static_cast<std::uint32_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), 0};
        const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
        void* list = nullptr;
        EXPECT(hr_of(raw(device, 100, word(&desc), word(allocator), 0, word(&list_iid), word(&list))) == S_OK);
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 1);
        EXPECT(release(list) == 0);
        EXPECT(release(allocator) == 0);
        EXPECT(release(queue) == 0);
    } else {
        // Values outside 3..127 are ignored: the five defaults still answer, and a PC slot that is not a default is
        // forwarded unchanged.
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 0);
        EXPECT(hr_of(raw(device, 79, 0, 0, 0, 0)) == S_OK && mock->calls[79] == 0);
        EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, word(&token))) == S_OK && token && mock->calls[80] == 0);
        EXPECT(hr_of(raw(device, 71)) == S_OK && mock->calls[71] == 1);  // not an Xbox slot: forwarded
    }
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: environment slot overrides %s\n", g_label,
                valid ? "move all five routes" : "outside 3..127 are ignored");
    return 0;
}

// ---- 8. Device slot 70: a view from raw words, never the PC CreatePlacedResource1 ----

// What the layer's resource provider knows: one buffer at [kBufferStart, kBufferStart + kBufferSize).
Mock* g_buffer = nullptr;
constexpr std::uint64_t kBufferStart = 0x10000, kBufferSize = 0x400;

std::uint64_t g_asked_bytes = 0;  // the byte count the proxy last asked the provider to cover

ID3D12Resource* find_buffer(ID3D12Device*, std::uint64_t va, std::uint64_t bytes, std::uint64_t* start,
                            std::uint64_t* size) {
    g_asked_bytes = bytes;
    if (!g_buffer || va < kBufferStart || va >= kBufferStart + kBufferSize) return nullptr;
    *start = kBufferStart;
    *size = kBufferSize;
    ++g_buffer->refs;  // the provider hands out a reference; the proxy releases it
    return reinterpret_cast<ID3D12Resource*>(g_buffer);
}

// The measured retail shape: a desktop buffer description, then a raw R32_TYPELESS view description with the VA right
// after it.
struct RawShape {
    D3D12_RESOURCE_DESC resource;
    struct {
        D3D12_SHADER_RESOURCE_VIEW_DESC view;
        D3D12_GPU_VIRTUAL_ADDRESS va;
    } view;
};

RawShape raw_shape(std::uint64_t va, std::uint64_t width) {
    RawShape shape{};
    shape.resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    shape.resource.Width = width;
    shape.resource.Height = 1;
    shape.resource.DepthOrArraySize = 1;
    shape.resource.MipLevels = 1;
    shape.resource.SampleDesc.Count = 1;
    shape.resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    shape.view.view.Format = DXGI_FORMAT_R32_TYPELESS;
    shape.view.view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    shape.view.view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shape.view.view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    shape.view.va = va;
    return shape;
}

bool is_null_view(const Mock* mock) {
    return mock->args[18][0] == 0 && mock->srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D &&
           mock->srv.Format == DXGI_FORMAT_R8G8B8A8_UNORM && mock->srv.Texture2D.MipLevels == 1 &&
           mock->srv.Shader4ComponentMapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
}

int check_slot70() {
    // Level 9 has the PC slot 70; level 0 does not (44 PC slots). Neither may reach CreatePlacedResource1.
    for (const int level : {9, 0}) {
        Mock* mock = nullptr;
        void* device = make_device(mock, level, true);
        EXPECT(device && xs::device_slot_overridden(70));
        xs::reset_device_proxy_stats();
        const std::uint32_t words[8] = {0x1000, 0, 0, 0, 0, 0, 0, 0};
        const std::uint32_t view[8] = {};
        const std::uint64_t destination = 0x7000;
        raw(device, 70, word(words), word(view), destination);
        EXPECT(mock->calls[70] == 0 && xs::device_slot_hits(70) == 1);
        // Words that are not the measured shape: the destination gets the PC null view (CreateShaderResourceView, slot
        // 18, no resource), the destination handle unchanged.
        EXPECT(mock->calls[18] == 1 && mock->args[18][2] == destination && is_null_view(mock));
        // No destination, no write.
        raw(device, 70, word(words), word(view), 0);
        EXPECT(mock->calls[70] == 0 && mock->calls[18] == 1);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    return 0;
}

// A raw buffer view of a registered VA: the view is of that resource, from (va - start) / 4, as wide as the
// description says. Every other shape, and a VA no buffer holds, still gets the null view.
int check_slot70_raw_buffer() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    g_buffer = new_mock(Kind::resource);
    g_buffer->refs = 1;
    EXPECT(xs::set_device_resource_provider(device, &find_buffer));
    const std::uint64_t destination = 0x7000;

    // 0x100 bytes at +0x40 of the buffer.
    RawShape shape = raw_shape(kBufferStart + 0x40, 0x100);
    raw(device, 70, word(&shape.resource), word(&shape.view), destination);
    EXPECT(mock->calls[70] == 0 && mock->calls[18] == 1 && mock->args[18][0] == word(g_buffer) &&
           mock->args[18][2] == destination);
    EXPECT(mock->srv.Format == DXGI_FORMAT_R32_TYPELESS && mock->srv.ViewDimension == D3D12_SRV_DIMENSION_BUFFER &&
           (mock->srv.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) && mock->srv.Buffer.FirstElement == 0x10 &&
           mock->srv.Buffer.NumElements == 0x40 && mock->srv.Buffer.StructureByteStride == 0 &&
           mock->srv.Shader4ComponentMapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
    EXPECT(g_buffer->refs == 1);  // the proxy released the reference the provider added
    EXPECT(g_asked_bytes == 0x100);  // it asks for a buffer that holds the whole width, so an alias that does wins

    // A width that runs past the buffer is cut at its end: (0x400 - 0x3f0) / 4 elements.
    shape = raw_shape(kBufferStart + 0x3f0, 0x1000);
    raw(device, 70, word(&shape.resource), word(&shape.view), destination);
    EXPECT(mock->calls[18] == 2 && mock->args[18][0] == word(g_buffer) && mock->srv.Buffer.FirstElement == 0xfc &&
           mock->srv.Buffer.NumElements == 4);

    // A VA no buffer holds: the null view, and nothing is left referenced.
    shape = raw_shape(kBufferStart + kBufferSize + 0x40, 0x100);
    raw(device, 70, word(&shape.resource), word(&shape.view), destination);
    EXPECT(mock->calls[18] == 3 && is_null_view(mock) && g_buffer->refs == 1);

    // A known VA but not a raw R32_TYPELESS buffer view (a typed view; a texture description): the null view.
    shape = raw_shape(kBufferStart + 0x40, 0x100);
    shape.view.view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    raw(device, 70, word(&shape.resource), word(&shape.view), destination);
    EXPECT(mock->calls[18] == 4 && is_null_view(mock));
    shape = raw_shape(kBufferStart + 0x40, 0x100);
    shape.resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    raw(device, 70, word(&shape.resource), word(&shape.view), destination);
    EXPECT(mock->calls[18] == 5 && is_null_view(mock) && g_buffer->refs == 1);

    // No provider: the null view as well.
    EXPECT(xs::set_device_resource_provider(device, nullptr));
    shape = raw_shape(kBufferStart + 0x40, 0x100);
    raw(device, 70, word(&shape.resource), word(&shape.view), destination);
    EXPECT(mock->calls[18] == 6 && is_null_view(mock) && mock->calls[70] == 0);

    g_buffer = nullptr;
    EXPECT(release(device) == 0 && mock->refs == 0);
    return 0;
}

// ---- 9. Device slot 26, GetCustomHeapProperties, the console's way and the PC's ----

int check_custom_heap_properties() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 0, true);
    EXPECT(device && xs::device_slot_overridden(26));
    void* const* vtable = *reinterpret_cast<void* const* const*>(device);

    // The retail executable's call: the buffer for the struct in RCX, the device in RDX, node mask and heap type in
    // R8 and R9 (their upper halves are not the callee's to read). The buffer sits inside a pattern that must stay.
    struct Padded {
        std::uint8_t before[16];
        D3D12_HEAP_PROPERTIES value;
        std::uint8_t after[64];
    } padded;
    std::memset(&padded, 0xA5, sizeof(padded));
    using ConsoleFn = std::uint64_t(WINAPI*)(void*, void*, std::uint64_t, std::uint64_t);
    const std::uint64_t returned = reinterpret_cast<ConsoleFn>(vtable[26])(
        &padded.value, device, 0xdeadbeef00000001ull, 0xdeadbeef00000002ull);
    EXPECT(returned == word(&padded.value));
    // The PC method got (buffer, node mask, heap type) once, on the device behind the proxy.
    EXPECT(mock->calls[26] == 1);
    EXPECT(static_cast<std::uint32_t>(mock->args[26][1]) == 1 && static_cast<std::uint32_t>(mock->args[26][2]) == 2);
    EXPECT(padded.value.Type == D3D12_HEAP_TYPE_UPLOAD && padded.value.CreationNodeMask == 1 &&
           padded.value.VisibleNodeMask == kHeapMarker);
    for (const std::uint8_t byte : padded.before) EXPECT(byte == 0xA5);
    for (const std::uint8_t byte : padded.after) EXPECT(byte == 0xA5);
    // The proxy is what it was: main's forwarder took the buffer for the proxy, and a thunk that took the proxy
    // for the buffer would write the struct over it.
    EXPECT(xs::is_device_proxy(device) && word(xs::device_proxy_real_device(device)) == word(mock) &&
           xs::device_proxy_slot_count(device) == 44 && xs::device_proxy_ref_count(device) == 1);

    // A PC C++ caller: the proxy in RCX and its buffer in RDX. That call is forwarded as it is.
    D3D12_HEAP_PROPERTIES pc{};
    EXPECT(raw(device, 26, word(&pc), 1, 3) == word(&pc));
    EXPECT(mock->calls[26] == 2 && mock->args[26][0] == word(&pc));
    EXPECT(pc.Type == D3D12_HEAP_TYPE_READBACK && pc.CreationNodeMask == 1 && pc.VisibleNodeMask == kHeapMarker);

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slot 26 with the buffer in RCX and the device in RDX reaches the PC method and writes "
                "only the struct; the PC form is forwarded unchanged\n", g_label);
    return 0;
}

// ---- 8b. Device slot 74: (pointer, u32, pointer) succeeds and touches nothing, never the PC ShaderCacheControl ----

int check_slot74() {
    // Level 9 has the PC slot 74 (ShaderCacheControl); level 0 does not (44 PC slots). Neither may reach it.
    for (const int level : {9, 0}) {
        Mock* mock = nullptr;
        void* device = make_device(mock, level, true);
        EXPECT(device && xs::device_slot_overridden(74));
        xs::reset_device_proxy_stats();
        std::uint32_t first[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
        std::uint32_t second[4] = {0x55555555, 0x66666666, 0x77777777, 0x88888888};
        const std::uint32_t first_before[4] = {first[0], first[1], first[2], first[3]};
        const std::uint32_t second_before[4] = {second[0], second[1], second[2], second[3]};
        EXPECT(hr_of(raw(device, 74, word(first), 7, word(second))) == S_OK);
        EXPECT(mock->calls[74] == 0 && xs::device_slot_hits(74) == 1);
        EXPECT(std::memcmp(first, first_before, sizeof(first)) == 0 && std::memcmp(second, second_before, sizeof(second)) == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    return 0;
}

// ---- 9. Command lists (device slot 12): the console-shaped proxy and AllocateUploadMemory ----

int check_lists() {
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    const IID gcl_iid = __uuidof(ID3D12GraphicsCommandList);
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true, true, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
    Mock* const real_allocator = mock->last_child;
    // CreateCommandList(node_mask, type, allocator, pso, iid, out): the allocator proxy is unwrapped on the way
    // in and the list comes back console-shaped.
    void* list = nullptr;
    EXPECT(hr_of(raw(device, 12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, word(allocator), 0, word(&gcl_iid),
                     word(&list))) == S_OK);
    Mock* const real = mock->last_child;
    EXPECT(real->kind == Kind::list && mock->args[12][2] == word(real_allocator));
    EXPECT(xs::is_list_proxy(list) && word(xs::list_proxy_real_list(list)) == word(real));
    EXPECT(xs::device_proxy_ref_count(device) == device_refs + 2);  // allocator + list
    // The console object body sits at +0x18 with the write cursor pinned at the record limits.
    const auto* body = reinterpret_cast<const std::uint64_t*>(word(list) + 0x18);
    // Object offsets +0x18/+0x28/+0x30 are body[0]/body[2]/body[3]: the cursor is pinned at both record limits.
    EXPECT(body[0] == body[2] && body[0] == body[3]);
    // GetDevice answers with the Xbox-ABI device.
    void* found = nullptr;
    const IID device_iid = __uuidof(ID3D12Device);
    EXPECT(hr_of(raw(list, 7, word(&device_iid), word(&found))) == S_OK && found == device);
    EXPECT(release(found) >= 1);
    // A forwarded console slot: Dispatch is console 14 (PC slot 14 too — order holds through 26).
    EXPECT(hr_of(raw(list, 14, 1, 1, 1)) == S_OK && real->calls[14] == 1);
    // Console slot 139 is the retail title's Reset(allocator, pso) (Pipodi measured it on 1.0.2.1): the allocator
    // proxy unwraps and the call reaches the real list's PC slot 10.
    EXPECT(hr_of(raw(list, 139, word(allocator), 0)) == S_OK && real->calls[10] == 1 &&
           real->args[10][0] == word(real_allocator));
    // Console-only extras (27-30) and the unmeasured tail are recorded and return 0, never forwarded.
    EXPECT(raw(list, 27, 1, 2, 3, 4) == 0 && real->calls[27] == 0);
    EXPECT(raw(list, 148, 0, 16) == 0);  // AllocateUploadMemory(0, ...) refuses
    // AllocateUploadMemory carves from the upload arena; a later root SRV gets a real address for it.
    const std::uint64_t p1 = raw(list, 148, 4096, 64);
    const std::uint64_t p2 = raw(list, 148, 16, 16);
    EXPECT(p1 && p2 > p1 && (p1 & 63) == 0 && p2 >= p1 + 4096);
    // A size that wraps the arena arithmetic is refused and moves nothing: UINT64_MAX used to return a
    // pointer and corrupt the cursor so the next request re-handed the same range (daemosofchaos's
    // report on #2490). A non-power-of-two alignment is unmeasured and refused rather than guessed.
    EXPECT(raw(list, 148, ~0ull, 16) == 0);
    EXPECT(raw(list, 148, 16, 48) == 0);
    const std::uint64_t p3 = raw(list, 148, 16, 16);
    EXPECT(p3 && p3 == p2 + 16);  // fresh memory right after p2 — both refusals left the cursor alone
    // Console 43 -> PC 39 (SetComputeRootShaderResourceView): the arena pointer crosses as a GPU VA (the mock
    // device cannot map a real buffer, so the fallback arena translates to the same address).
    EXPECT(hr_of(raw(list, 43, 0, p1)) == S_OK && real->calls[39] == 1 && real->args[39][1] == p1);
    // The view-struct paths (IASetIndexBuffer console 47 -> PC 43, IASetVertexBuffers console 48 -> PC 44) reach
    // the real list; the arena VAs inside the views are rewritten in the proxy's copies.
    D3D12_INDEX_BUFFER_VIEW ib_view{};
    ib_view.BufferLocation = p1;
    EXPECT(hr_of(raw(list, 47, word(&ib_view))) == S_OK && real->calls[43] == 1);
    D3D12_VERTEX_BUFFER_VIEW vb_views[2]{};
    vb_views[0].BufferLocation = p1;
    vb_views[1].BufferLocation = 0xDEAD0000;  // a VA outside the arena passes through
    EXPECT(hr_of(raw(list, 48, 0, 2, word(vb_views))) == S_OK && real->calls[44] == 1);
    D3D12_STREAM_OUTPUT_BUFFER_VIEW so_view{};
    so_view.BufferLocation = p1;
    so_view.BufferFilledSizeLocation = p2;
    EXPECT(hr_of(raw(list, 49, 0, 1, word(&so_view))) == S_OK && real->calls[45] == 1);
    // The console tail past the PC chain is refused with E_NOTIMPL only when the mapped PC slot is missing.
    EXPECT(hr_of(raw(list, 146, word(list), 0)) == S_OK && real->calls[80] == 1);  // enhanced Barrier -> GCL7 80
    EXPECT(release(list) == 0 && real->refs == 0);
    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);

    // A PC caller (console_lists=false — the default, what the GPU harnesses use) gets the real list back:
    // slot 12 returns the mock, not a proxy, so its PC vtable indices stay valid.
    Mock* pc_mock = new_mock(Kind::device);
    pc_mock->device_level = 9;
    pc_mock->refs = 1;
    void* pc_device = nullptr;
    EXPECT(SUCCEEDED(xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(pc_mock), __uuidof(ID3D12Device),
                                           &pc_device)));
    void* pc_list = nullptr;
    EXPECT(hr_of(raw(pc_device, 12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, word(&gcl_iid),
                     word(&pc_list))) == S_OK);
    EXPECT(pc_list && !xs::is_list_proxy(pc_list) &&
           reinterpret_cast<Mock*>(pc_list)->kind == Kind::command_list);
    EXPECT(release(pc_list) == 0);
    EXPECT(release(pc_device) == 0 && pc_mock->refs == 0);
    std::printf("%s: lists come back console-shaped (cursor pinned, GetDevice, Dispatch forwarded, extras "
                "recorded); AllocateUploadMemory hands out arena pointers a root SRV receives\n", g_label);
    return 0;
}

// ---- 10. SignalX layout detection ------------------------------------------------------------
// A fence at queue slot 25 while the layout is still unknown is itself the newer-layout evidence: a title that
// signals only through SignalX never reaches Signal/Wait at 16/17, so an unanswered SignalX used to return
// S_OK having signalled nothing — a waiter on that fence value then hangs (shdann's report on #2490: the first
// SignalX gave hr=0 calls=0, the value was lost).

int check_signalx_detection() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    const auto real_fence = word(xs::fence_proxy_real_fence(fence));
    // Undetected layout, fence at slot 25: forwarded as Signal and pins the device to the newer layout.
    EXPECT(hr_of(raw(queue, 25, word(fence), 7)) == S_OK && queue_mock->calls[14] == 1 &&
           queue_mock->args[14][0] == real_fence && queue_mock->args[14][1] == 7);
    // Now proven newer: Signal at 16 and SignalX at 25 keep forwarding.
    EXPECT(hr_of(raw(queue, 16, word(fence), 8)) == S_OK && queue_mock->calls[14] == 2 &&
           queue_mock->args[14][1] == 8);
    EXPECT(hr_of(raw(queue, 25, word(fence), 9)) == S_OK && queue_mock->calls[14] == 3 &&
           queue_mock->args[14][1] == 9);
    // A second queue on the same device inherits the shared detection.
    void* queue2 = make_queue(device);
    EXPECT(queue2);
    Mock* const queue2_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue2));
    EXPECT(hr_of(raw(queue2, 25, word(fence), 11)) == S_OK && queue2_mock->calls[14] == 1 &&
           queue2_mock->args[14][1] == 11);
    EXPECT(release(queue2) == 0);
    EXPECT(release(queue) == 0 && release(fence) == 0 && release(device) == 0 && mock->refs == 0);

    // Proven older stays older: a non-fence at slot 16 pins the older layout (the frequency getter), and a
    // fence at 25 afterwards is an unmodelled console method — S_OK with no forward, not a second detection.
    Mock* mock2 = nullptr;
    void* device2 = make_device(mock2, 9, true);
    void* queue3 = make_queue(device2);
    void* fence3 = make_fence(device2);
    EXPECT(device2 && queue3 && fence3);
    Mock* const queue3_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue3));
    EXPECT(hr_of(raw(queue3, 16, 0, 0)) == S_OK && queue3_mock->calls[16] == 1 &&
           queue3_mock->calls[14] == 0);
    EXPECT(hr_of(raw(queue3, 25, word(fence3), 13)) == S_OK && queue3_mock->calls[14] == 0);
    // The older SignalX at slot 23 still forwards under the pinned-older layout.
    EXPECT(hr_of(raw(queue3, 23, word(fence3), 15)) == S_OK && queue3_mock->calls[14] == 1 &&
           queue3_mock->args[14][1] == 15);
    EXPECT(release(queue3) == 0 && release(fence3) == 0 && release(device2) == 0 && mock2->refs == 0);

    // A fence at the older SignalX slot while undetected forwards but does NOT pin: a newer title could reach
    // an unmodelled slot-23 method with a fence argument, and a premature older pin would drop its real
    // SignalX at 25.
    Mock* mock3 = nullptr;
    void* device3 = make_device(mock3, 9, true);
    void* queue4 = make_queue(device3);
    void* fence4 = make_fence(device3);
    EXPECT(device3 && queue4 && fence4);
    Mock* const queue4_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue4));
    EXPECT(hr_of(raw(queue4, 23, word(fence4), 17)) == S_OK && queue4_mock->calls[14] == 1 &&
           queue4_mock->args[14][1] == 17);
    EXPECT(hr_of(raw(queue4, 25, word(fence4), 19)) == S_OK && queue4_mock->calls[14] == 2 &&
           queue4_mock->args[14][1] == 19);
    EXPECT(release(queue4) == 0 && release(fence4) == 0 && release(device3) == 0 && mock3->refs == 0);

    // Ambiguous slot 18 on an undetected queue: the older GetDesc or the newer frequency getter. It is
    // served as the frequency query — a desc write is far wider than the 8 bytes a frequency caller
    // passes (a title often reads the frequency before its first fence call); once a fence proves the
    // layout the same call keeps reaching the getter through the shifted path.
    Mock* mock4 = nullptr;
    void* device4 = make_device(mock4, 9, true);
    void* queue5 = make_queue(device4);
    void* fence5 = make_fence(device4);
    EXPECT(device4 && queue5 && fence5);
    Mock* const queue5_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue5));
    UINT64 hz = 0;
    EXPECT(hr_of(raw(queue5, 18, word(&hz))) == S_OK && hz == 1000000 &&
           queue5_mock->calls[16] == 1 && queue5_mock->calls[18] == 0);
    hz = 0;
    EXPECT(hr_of(raw(queue5, 16, word(fence5), 8)) == S_OK);
    EXPECT(hr_of(raw(queue5, 18, word(&hz))) == S_OK && hz == 1000000 &&
           queue5_mock->calls[16] == 2 && queue5_mock->calls[18] == 0);
    EXPECT(release(queue5) == 0 && release(fence5) == 0 && release(device4) == 0 && mock4->refs == 0);
    std::printf("xs_proxy_mock: SignalX on an undetected queue's slot 25 forwards as Signal and marks the "
                "newer layout; proven-older stays older, a fence at 23 does not pin, and an undetected "
                "slot 18 answers the frequency getter\n");
    return 0;
}

// ---- 11. The console tail and the guard region ----

int check_tail(bool want_guarded) {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    EXPECT(xs::command_object_tables_guarded() == want_guarded);
    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    // Signal (14) still reaches the real queue, with the real fence.
    EXPECT(hr_of(raw(queue, 14, word(fence), 5)) == S_OK && queue_mock->calls[14] == 1 &&
           queue_mock->args[14][0] == word(xs::fence_proxy_real_fence(fence)) && queue_mock->args[14][1] == 5);
    // Console-only queue methods (the tail past the PC chain, SignalX, PresentX) report S_OK instead of
    // refusing: frame pacing flows through them. Nothing is forwarded, except the slots the proxy models (35).
    for (unsigned slot = 19; slot < xs::queue_proxy_table_size(); ++slot) {
        if (xs::queue_slot_overridden(slot)) continue;
        EXPECT(hr_of(raw(queue, slot)) == S_OK && queue_mock->calls[slot] == 0);
    }
    EXPECT(raw(fence, 12) != 0);  // the completed value's address (check_fence_completed_value)
    for (unsigned slot = 13; slot < xs::fence_proxy_table_size(); ++slot) EXPECT(hr_of(raw(fence, slot)) == E_NOTIMPL);
    EXPECT(xs::queue_table_overruns() == 0 && xs::fence_table_overruns() == 0);
    EXPECT(release(queue) == 0 && release(fence) == 0 && release(device) == 0 && mock->refs == 0);
    return 0;
}

struct HeapHookState {
    void* next_ptr = nullptr;
    std::size_t last_alloc_size = 0;
    std::uint64_t last_alloc_attr = 0;
    int alloc_calls = 0;
    void* last_free_ptr = nullptr;
    std::uint64_t last_free_attr = 0;
    int free_calls = 0;
} g_heap_hook;

void* mock_heap_alloc(std::size_t byte_size, std::uint64_t attributes) {
    ++g_heap_hook.alloc_calls;
    g_heap_hook.last_alloc_size = byte_size;
    g_heap_hook.last_alloc_attr = attributes;
    return g_heap_hook.next_ptr;
}

void mock_heap_free(void* ptr, std::uint64_t attributes) {
    ++g_heap_hook.free_calls;
    g_heap_hook.last_free_ptr = ptr;
    g_heap_hook.last_free_attr = attributes;
}

int check_descriptor_heap_hooks() {
    g_heap_hook = {};
    xs::set_descriptor_heap_allocator(&mock_heap_alloc, &mock_heap_free);

    Mock* real = new_mock(Kind::device);
    real->refs = 1;
    real->device_level = 9;
    void* device = nullptr;
    EXPECT(SUCCEEDED(xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(real), __uuidof(ID3D12Device), &device)));

    constexpr SIZE_T kCustomBase = 0x77770000ull;
    g_heap_hook.next_ptr = reinterpret_cast<void*>(kCustomBase);
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 8;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const IID iid_heap = __uuidof(ID3D12DescriptorHeap);
    void* heap = nullptr;
    EXPECT(SUCCEEDED(hr_of(raw(device, 14, word(&desc), word(&iid_heap), word(&heap)))));
    EXPECT(heap != nullptr && xs::is_heap_proxy(heap));
    EXPECT(g_heap_hook.alloc_calls == 1);
    EXPECT(g_heap_hook.last_alloc_size == desc.NumDescriptors * kMockDescriptorInc);
    EXPECT(g_heap_hook.last_alloc_attr == xs::kXmemDescriptorHeapAttributes);

    // Slot 9 returns the title-supplied base; slot 10 keeps the real GPU start so (gpu - cpu) delta stays exact.
    EXPECT(raw(heap, 9) == kCustomBase);
    EXPECT(raw(heap, 10) == kMockHeapGpuStart);

    // Public translator and device slots 17-24 remap handles in [kCustomBase, +256) to kMockHeapCpuStart + offset.
    const SIZE_T off3 = 3 * kMockDescriptorInc;
    const SIZE_T off7 = 7 * kMockDescriptorInc;
    const SIZE_T outside = kCustomBase + desc.NumDescriptors * kMockDescriptorInc;
    EXPECT(xs::translate_cpu_descriptor_handle(device, D3D12_CPU_DESCRIPTOR_HANDLE{kCustomBase + off3}).ptr ==
           kMockHeapCpuStart + off3);
    EXPECT(xs::translate_cpu_descriptor_handle(device, D3D12_CPU_DESCRIPTOR_HANDLE{outside}).ptr == outside);

    raw(device, 17, 0x11, kCustomBase + off3);                                          // CreateConstantBufferView
    EXPECT(real->args[17][1] == kMockHeapCpuStart + off3);
    raw(device, 18, 0x22, 0, kCustomBase + off3);                                       // CreateShaderResourceView
    EXPECT(real->args[18][2] == kMockHeapCpuStart + off3);
    raw(device, 19, 0x22, 0x33, 0x44, kCustomBase + off7);                              // CreateUnorderedAccessView
    EXPECT(real->args[19][3] == kMockHeapCpuStart + off7);
    raw(device, 20, 0x22, 0x33, kCustomBase + off3);                                    // CreateRenderTargetView
    EXPECT(real->args[20][2] == kMockHeapCpuStart + off3);
    raw(device, 21, 0x22, 0x33, kCustomBase + off7);                                    // CreateDepthStencilView
    EXPECT(real->args[21][2] == kMockHeapCpuStart + off7);
    raw(device, 22, 0x55, kCustomBase + off3);                                          // CreateSampler
    EXPECT(real->args[22][1] == kMockHeapCpuStart + off3);
    raw(device, 24, 1, kCustomBase + off7, kCustomBase + off3, 0);                      // CopyDescriptorsSimple
    EXPECT(real->args[24][1] == kMockHeapCpuStart + off7);
    EXPECT(real->args[24][2] == kMockHeapCpuStart + off3);
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsts[2] = {{kCustomBase + off3}, {outside}};
        D3D12_CPU_DESCRIPTOR_HANDLE srcs[2] = {{kCustomBase + off7}, {0x99990000ull}};
        UINT sizes[2] = {1, 1};
        static_cast<ID3D12Device*>(device)->CopyDescriptors(2, dsts, sizes, 2, srcs, sizes,
                                                            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        EXPECT(real->array[23][0] == kMockHeapCpuStart + off3 && real->array[23][1] == outside);
        EXPECT(real->array[24][0] == kMockHeapCpuStart + off7 && real->array[24][1] == 0x99990000ull);
    }

    // Releasing the heap unregisters the range and invokes the free callback with the same attribute word.
    EXPECT(raw(heap, 2) == 0);
    EXPECT(g_heap_hook.free_calls == 1);
    EXPECT(g_heap_hook.last_free_ptr == reinterpret_cast<void*>(kCustomBase));
    EXPECT(g_heap_hook.last_free_attr == xs::kXmemDescriptorHeapAttributes);
    EXPECT(xs::translate_cpu_descriptor_handle(device, D3D12_CPU_DESCRIPTOR_HANDLE{kCustomBase + off3}).ptr ==
           kCustomBase + off3);

    // Null return from the hook falls back cleanly to the PC heap's start and never calls free.
    g_heap_hook.next_ptr = nullptr;
    void* plain_heap = nullptr;
    EXPECT(SUCCEEDED(hr_of(raw(device, 14, word(&desc), word(&iid_heap), word(&plain_heap)))));
    EXPECT(plain_heap != nullptr && raw(plain_heap, 9) == kMockHeapCpuStart);
    EXPECT(raw(plain_heap, 2) == 0);
    EXPECT(g_heap_hook.free_calls == 1);

    // QI failure on heap proxy creation (unsupported IID) frees the hook backing immediately.
    g_heap_hook.next_ptr = reinterpret_cast<void*>(kCustomBase);
    void* bad_heap = nullptr;
    EXPECT(hr_of(raw(device, 14, word(&desc), word(&kExtraIid), word(&bad_heap))) == E_NOINTERFACE);
    EXPECT(bad_heap == nullptr && g_heap_hook.free_calls == 2);
    EXPECT(g_heap_hook.last_free_ptr == reinterpret_cast<void*>(kCustomBase));

    // OOM on HeapProxy allocation frees the hook backing and leaves no dangling range.
    g_fail_news = 1;
    void* oom_heap = nullptr;
    EXPECT(hr_of(raw(device, 14, word(&desc), word(&iid_heap), word(&oom_heap))) == E_OUTOFMEMORY);
    EXPECT(oom_heap == nullptr && g_heap_hook.free_calls == 3);
    EXPECT(xs::translate_cpu_descriptor_handle(device, D3D12_CPU_DESCRIPTOR_HANDLE{kCustomBase}).ptr == kCustomBase);

    EXPECT(raw(device, 2) == 0 && real->refs == 0);
    xs::set_descriptor_heap_allocator(nullptr, nullptr);
    return 0;
}

// ---- 10. Console C-ABI GetDesc (out in RCX, proxy in RDX) on heap slot 8 and queue slot 18 ----

int check_get_desc_c_abi() {
    EXPECT(xs::heap_slot_overridden(8));
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);

    const IID heap_iid = __uuidof(ID3D12DescriptorHeap);
    void* heap = nullptr;
    EXPECT(hr_of(raw(device, 14, word(&kMockHeapDesc), word(&heap_iid), word(&heap))) == S_OK);
    EXPECT(xs::is_heap_proxy(heap));
    Mock* const heap_mock = reinterpret_cast<Mock*>(xs::heap_proxy_real_heap(heap));
    EXPECT(heap_mock && heap_mock->kind == Kind::heap);

    void* queue = make_queue(device);
    EXPECT(xs::is_queue_proxy(queue));
    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    EXPECT(queue_mock && queue_mock->kind == Kind::queue);

    xs::reset_device_proxy_stats();

    // Heap slot 8: console C-ABI (out in RCX, proxy in RDX) + null out + PC C++ ABI (proxy in RCX, out in RDX).
    {
        void* const* heap_vtbl = *reinterpret_cast<void* const* const*>(heap);
        using ConsoleHeapGetDescFn = D3D12_DESCRIPTOR_HEAP_DESC*(WINAPI*)(D3D12_DESCRIPTOR_HEAP_DESC*, void*);
        using PcHeapGetDescFn = D3D12_DESCRIPTOR_HEAP_DESC*(WINAPI*)(void*, D3D12_DESCRIPTOR_HEAP_DESC*);
        auto console_get_desc = reinterpret_cast<ConsoleHeapGetDescFn>(heap_vtbl[8]);
        auto pc_get_desc = reinterpret_cast<PcHeapGetDescFn>(heap_vtbl[8]);

        struct PaddedHeapDesc {
            D3D12_DESCRIPTOR_HEAP_DESC value;
            std::uint8_t after[64];
        } padded;
        std::memset(&padded, 0xA5, sizeof(padded));
        EXPECT(console_get_desc(&padded.value, heap) == &padded.value);
        EXPECT(std::memcmp(&padded.value, &kMockHeapDesc, sizeof(kMockHeapDesc)) == 0);
        for (const std::uint8_t byte : padded.after) EXPECT(byte == 0xA5);
        EXPECT(heap_mock->calls[8] == 1 && heap_mock->args[8][0] == word(&padded.value));
        EXPECT(xs::heap_slot_hits(8) == 1 && xs::heap_slot_first_call_seen(8));
        EXPECT(xs::is_heap_proxy(heap) &&
               reinterpret_cast<Mock*>(xs::heap_proxy_real_heap(heap)) == heap_mock);

        EXPECT(console_get_desc(nullptr, heap) == nullptr);
        EXPECT(heap_mock->calls[8] == 1 && xs::heap_slot_hits(8) == 2);

        D3D12_DESCRIPTOR_HEAP_DESC pc_out{};
        EXPECT(pc_get_desc(heap, &pc_out) == &pc_out);
        EXPECT(std::memcmp(&pc_out, &kMockHeapDesc, sizeof(kMockHeapDesc)) == 0);
        EXPECT(heap_mock->calls[8] == 2 && heap_mock->args[8][0] == word(&pc_out));
        EXPECT(xs::heap_slot_hits(8) == 3);
    }

    // Queue slot 18: console C-ABI (out in RCX, proxy in RDX) translates to GetDesc; PC-order (proxy in RCX) falls
    // through to the generic queue slot path rather than being intercepted by a GetDesc-only branch (so queue layout
    // detection can answer GetTimestampFrequency on undetected queues and full arguments are preserved).
    {
        void* const* queue_vtbl = *reinterpret_cast<void* const* const*>(queue);
        using ConsoleQueueGetDescFn = D3D12_COMMAND_QUEUE_DESC*(WINAPI*)(D3D12_COMMAND_QUEUE_DESC*, void*);
        using PcQueueGetDescFn = D3D12_COMMAND_QUEUE_DESC*(WINAPI*)(void*, D3D12_COMMAND_QUEUE_DESC*);
        auto console_get_desc = reinterpret_cast<ConsoleQueueGetDescFn>(queue_vtbl[18]);
        auto pc_get_desc = reinterpret_cast<PcQueueGetDescFn>(queue_vtbl[18]);

        struct PaddedQueueDesc {
            D3D12_COMMAND_QUEUE_DESC value;
            std::uint8_t after[64];
        } padded;
        std::memset(&padded, 0xA5, sizeof(padded));
        EXPECT(console_get_desc(&padded.value, queue) == &padded.value);
        EXPECT(std::memcmp(&padded.value, &kMockQueueDesc, sizeof(kMockQueueDesc)) == 0);
        for (const std::uint8_t byte : padded.after) EXPECT(byte == 0xA5);
        EXPECT(queue_mock->calls[18] == 1 && queue_mock->args[18][0] == word(&padded.value));
        EXPECT(xs::queue_slot_hits(18) == 1 && xs::queue_slot_first_call_seen(18));
        EXPECT(xs::is_queue_proxy(queue) &&
               reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue)) == queue_mock);

        EXPECT(console_get_desc(nullptr, queue) == nullptr);
        EXPECT(queue_mock->calls[18] == 1 && xs::queue_slot_hits(18) == 2);

        // Pin the older layout first (#2490): an undetected queue answers PC-order slot 18 as the newer layout's
        // frequency getter, which cannot overrun an 8-byte out; PC-order GetDesc there is the older layout's.
        UINT64 frequency = 0;
        EXPECT(hr_of(raw(queue, 16, word(&frequency))) == S_OK && frequency == 1000000);

        D3D12_COMMAND_QUEUE_DESC pc_out{};
        EXPECT(pc_get_desc(queue, &pc_out) == &pc_out);
        EXPECT(std::memcmp(&pc_out, &kMockQueueDesc, sizeof(kMockQueueDesc)) == 0);
        EXPECT(queue_mock->calls[18] == 2 && queue_mock->args[18][0] == word(&pc_out));
        EXPECT(xs::queue_slot_hits(18) == 3);

        // Regression: PC-order slot 18 must fall through to the generic queue forwarder (preserving all forwarded
        // argument words rather than short-circuiting through a 2-argument GetDesc-only branch).
        D3D12_COMMAND_QUEUE_DESC pc_passthrough_out{};
        constexpr std::uint64_t kTag2 = 0x1122334455667788ull;
        constexpr std::uint64_t kTag3 = 0x2233445566778899ull;
        constexpr std::uint64_t kTag4 = 0x33445566778899aaull;
        constexpr std::uint64_t kTag5 = 0x445566778899aabbull;
        EXPECT(raw(queue, 18, word(&pc_passthrough_out), kTag2, kTag3, kTag4, kTag5) == word(&pc_passthrough_out));
        EXPECT(std::memcmp(&pc_passthrough_out, &kMockQueueDesc, sizeof(kMockQueueDesc)) == 0);
        EXPECT(queue_mock->calls[18] == 3);
        EXPECT(queue_mock->args[18][0] == word(&pc_passthrough_out) && queue_mock->args[18][1] == kTag2 &&
               queue_mock->args[18][2] == kTag3 && queue_mock->args[18][3] == kTag4 &&
               queue_mock->args[18][4] == kTag5);
        EXPECT(xs::queue_slot_hits(18) == 4);
    }

    EXPECT(release(heap) == 0 && heap_mock->refs == 0);
    EXPECT(release(queue) == 0 && queue_mock->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: heap slot 8 and queue slot 18 answer console C-ABI (out, proxy) and PC C++ (proxy, out) GetDesc\n",
                g_label);
    return 0;
}

int check_list_cpu_handles() {
    g_heap_hook = {};
    xs::set_descriptor_heap_allocator(&mock_heap_alloc, &mock_heap_free);

    Mock* real = nullptr;
    void* device = make_device(real, 9, true, true, true);
    EXPECT(device != nullptr);

    constexpr SIZE_T kCustomBase = 0x77880000ull;
    g_heap_hook.next_ptr = reinterpret_cast<void*>(kCustomBase);
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    desc.NumDescriptors = 8;
    const IID iid_heap = __uuidof(ID3D12DescriptorHeap);
    void* heap = nullptr;
    EXPECT(SUCCEEDED(hr_of(raw(device, 14, word(&desc), word(&iid_heap), word(&heap)))));
    EXPECT(heap != nullptr && raw(heap, 9) == kCustomBase);

    const IID iid_alloc = __uuidof(ID3D12CommandAllocator);
    const IID iid_gcl = __uuidof(ID3D12GraphicsCommandList);
    void* allocator = nullptr;
    EXPECT(SUCCEEDED(hr_of(raw(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&iid_alloc), word(&allocator)))));
    void* list = nullptr;
    EXPECT(SUCCEEDED(hr_of(raw(device, 12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, word(allocator), 0,
                               word(&iid_gcl), word(&list)))));
    EXPECT(list != nullptr && xs::is_list_proxy(list));
    Mock* const real_list = reinterpret_cast<Mock*>(xs::list_proxy_real_list(list));
    EXPECT(real_list != nullptr);

    const SIZE_T off1 = 1 * kMockDescriptorInc;
    const SIZE_T off4 = 4 * kMockDescriptorInc;
    const SIZE_T off6 = 6 * kMockDescriptorInc;
    const SIZE_T outside = kCustomBase + desc.NumDescriptors * kMockDescriptorInc;

    // Console slot 50 -> PC slot 46 (OMSetRenderTargets): multiple RTV handles + DSV handle, caller arrays untouched.
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {{kCustomBase + off1}, {outside}};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{kCustomBase + off6};
        raw(list, 50, 2, word(rtvs), FALSE, word(&dsv));
        EXPECT(real_list->calls[46] == 1);
        EXPECT(real_list->om_rtv_count == 2 && real_list->om_single_range == FALSE);
        EXPECT(real_list->om_rtvs[0] == kMockHeapCpuStart + off1 && real_list->om_rtvs[1] == outside);
        EXPECT(real_list->om_has_dsv && real_list->om_dsv == kMockHeapCpuStart + off6);
        EXPECT(rtvs[0].ptr == kCustomBase + off1 && rtvs[1].ptr == outside && dsv.ptr == kCustomBase + off6);
    }
    // Single descriptor range: only rtvs[0] is read and translated even when NumRenderTargetDescriptors > 1; null DSV.
    {
        D3D12_CPU_DESCRIPTOR_HANDLE single_rtv{kCustomBase + off4};
        raw(list, 50, 3, word(&single_rtv), TRUE, 0);
        EXPECT(real_list->calls[46] == 2);
        EXPECT(real_list->om_rtv_count == 3 && real_list->om_single_range == TRUE);
        EXPECT(real_list->om_rtvs[0] == kMockHeapCpuStart + off4 && !real_list->om_has_dsv);
        EXPECT(single_rtv.ptr == kCustomBase + off4);
    }
    // Zero RTVs with a non-null DSV.
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{kCustomBase + off1};
        raw(list, 50, 0, 0, FALSE, word(&dsv));
        EXPECT(real_list->calls[46] == 3);
        EXPECT(real_list->om_rtv_count == 0 && real_list->om_has_dsv && real_list->om_dsv == kMockHeapCpuStart + off1);
    }
    // Count beyond D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT (8): forwards safely without overrunning stack scratch.
    {
        D3D12_CPU_DESCRIPTOR_HANDLE nine[9]{};
        for (UINT i = 0; i < 9; ++i) nine[i].ptr = outside + i * kMockDescriptorInc;
        raw(list, 50, 9, word(nine), FALSE, 0);
        EXPECT(real_list->calls[46] == 4 && real_list->om_rtv_count == 9);
    }

    // Console slot 51 -> PC slot 47 (ClearDepthStencilView): CPU handle translated AND FLOAT Depth in XMM3 preserved.
    {
        using ClearDsvFn = void(WINAPI*)(void*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT,
                                         const D3D12_RECT*);
        void* const* list_vtbl = *reinterpret_cast<void* const* const*>(list);
        auto clear_dsv = reinterpret_cast<ClearDsvFn>(list_vtbl[51]);
        const D3D12_RECT rect{1, 2, 3, 4};
        clear_dsv(list, D3D12_CPU_DESCRIPTOR_HANDLE{kCustomBase + off6},
                  D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.375f, 0x5Au, 1, &rect);
        EXPECT(real_list->calls[47] == 1);
        EXPECT(real_list->clear_dsv_handle == kMockHeapCpuStart + off6);
        EXPECT(real_list->clear_dsv_flags == (D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL));
        EXPECT(real_list->clear_dsv_depth == 0.375f);
        EXPECT(real_list->clear_dsv_stencil == 0x5Au);
        EXPECT(real_list->clear_dsv_num_rects == 1 && real_list->clear_dsv_rects == &rect);

        // Second call (past first-call log): outside-range handle passes through and a different depth float holds.
        clear_dsv(list, D3D12_CPU_DESCRIPTOR_HANDLE{outside}, D3D12_CLEAR_FLAG_DEPTH, 0.625f, 0u, 0, nullptr);
        EXPECT(real_list->calls[47] == 2);
        EXPECT(real_list->clear_dsv_handle == outside);
        EXPECT(real_list->clear_dsv_depth == 0.625f);
        EXPECT(real_list->clear_dsv_num_rects == 0 && real_list->clear_dsv_rects == nullptr);
    }

    // Console slot 52 -> PC slot 48 (ClearRenderTargetView): CPU handle translated, color and rects forwarded.
    {
        const FLOAT color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        const D3D12_RECT rect{0, 0, 8, 8};
        raw(list, 52, kCustomBase + off1, word(color), 1, word(&rect));
        EXPECT(real_list->calls[48] == 1);
        EXPECT(real_list->args[48][0] == kMockHeapCpuStart + off1);
        EXPECT(real_list->args[48][1] == word(color) && real_list->args[48][2] == 1 &&
               real_list->args[48][3] == word(&rect));

        raw(list, 52, outside, word(color), 0, 0);
        EXPECT(real_list->calls[48] == 2 && real_list->args[48][0] == outside);
    }

    // Console slots 53/54 -> PC slots 49/50 (ClearUnorderedAccessViewUint/Float): GPU handle kept, CPU handle remapped.
    {
        const UINT uvals[4] = {1, 2, 3, 4};
        const FLOAT fvals[4] = {1.0f, 0.5f, 0.25f, 0.0f};
        const D3D12_RECT rect{2, 2, 6, 6};
        raw(list, 53, kMockHeapGpuStart + off1, kCustomBase + off4, 0x3333ull, word(uvals), 1, word(&rect));
        EXPECT(real_list->calls[49] == 1);
        EXPECT(real_list->args[49][0] == kMockHeapGpuStart + off1);
        EXPECT(real_list->args[49][1] == kMockHeapCpuStart + off4);
        EXPECT(real_list->args[49][2] == 0x3333ull && real_list->args[49][3] == word(uvals) &&
               real_list->args[49][4] == 1 && real_list->args[49][5] == word(&rect));

        raw(list, 54, kMockHeapGpuStart + off6, kCustomBase + off6, 0x4444ull, word(fvals), 0, 0);
        EXPECT(real_list->calls[50] == 1);
        EXPECT(real_list->args[50][0] == kMockHeapGpuStart + off6);
        EXPECT(real_list->args[50][1] == kMockHeapCpuStart + off6);
        EXPECT(real_list->args[50][2] == 0x4444ull && real_list->args[50][3] == word(fvals) &&
               real_list->args[50][4] == 0 && real_list->args[50][5] == 0);

        raw(list, 54, kMockHeapGpuStart, outside, 0x4444ull, word(fvals), 0, 0);
        EXPECT(real_list->calls[50] == 2 && real_list->args[50][1] == outside);
    }

    EXPECT(raw(list, 2) == 0);
    EXPECT(raw(allocator, 2) == 0);
    EXPECT(raw(heap, 2) == 0);
    EXPECT(raw(device, 2) == 0 && real->refs == 0);
    xs::set_descriptor_heap_allocator(nullptr, nullptr);
    return 0;
}

// ---- Guard-region setup failures: this executable's import table, patched before the first proxy table ----

std::atomic<int> g_injected{0};
bool g_armed = false;
void* g_real = nullptr;

LPVOID WINAPI fail_virtual_alloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    if (g_armed && (type & MEM_RESERVE) && size == 64 * 1024) {
        ++g_injected;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    return reinterpret_cast<decltype(&VirtualAlloc)>(g_real)(address, size, type, protect);
}

BOOL WINAPI fail_virtual_protect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old) {
    if (g_armed && protect == PAGE_READONLY) {
        ++g_injected;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return reinterpret_cast<decltype(&VirtualProtect)>(g_real)(address, size, protect, old);
}

PVOID WINAPI fail_add_veh(ULONG, PVECTORED_EXCEPTION_HANDLER) {
    ++g_injected;
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
}

// Points this executable's import of `function` (by name) at `replacement`; the proxy code is linked into it.
bool patch_import(const char* function, void* replacement) {
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return false;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress); desc->Name; ++desc) {
        const DWORD lookup = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + lookup);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            g_real = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);  // through the replacement, not armed yet
            return true;
        }
    }
    return false;
}

int run_fault_mode(const char* mode) {
    struct Mode { const char* flag; const char* function; void* replacement; bool guarded; } const modes[] = {
        {"--fail-virtualalloc", "VirtualAlloc", reinterpret_cast<void*>(&fail_virtual_alloc), false},
        {"--fail-virtualprotect", "VirtualProtect", reinterpret_cast<void*>(&fail_virtual_protect), true},
        {"--fail-veh", "AddVectoredExceptionHandler", reinterpret_cast<void*>(&fail_add_veh), false},
    };
    for (const Mode& m : modes) {
        if (std::strcmp(mode, m.flag) != 0) continue;
        g_label = m.flag;
        EXPECT(patch_import(m.function, m.replacement));
        g_armed = true;
        const int failed = check_tail(m.guarded);
        // Disarmed once the tables exist: with a static CRT the CRT's own calls go through this import table too
        // (its exit path asks VirtualProtect for read-only pages), and only the proxy's calls are meant here.
        g_armed = false;
        if (failed) return 1;
        EXPECT(g_injected.load() > 0);  // the proxy's own call was the one refused
        std::printf("%s: %s refused %d time(s): the tables %s guarded, the proxies forward and answer their "
                    "console tail as before\n", m.flag, m.function, g_injected.load(),
                    m.guarded ? "stay" : "are not");
        return 0;
    }
    return 2;
}

// Device slot 127 is ID3D12Device10::CreateCommittedResource3 with Xbox descriptors (RT-5 stop 15). Past the PC
// vtable it used to be refused with E_NOTIMPL; the PC device must now get the resource with the Xbox layout word and
// flag bits gone and every other word as the title passed it.
int check_committed_resource3_x() {
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    constexpr std::uint32_t kXboxLayoutTiled = 0x101, kXboxFlagBit = 0x40000, kBarrierLayoutUndefined = 0xFFFFFFFFu;
    constexpr std::uint32_t kTightAlignment = 0x400,   // D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT, a PC bit
        kHeapNotZeroed = 0x1000;                       // D3D12_HEAP_FLAG_CREATE_NOT_ZEROED
    static const int session_sentinel = 0;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    const DXGI_FORMAT castable[2] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    D3D12_RESOURCE_DESC1 texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = 1920;
    texture.Height = 1080;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    texture.SampleDesc.Count = 1;
    texture.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(kXboxLayoutTiled);
    texture.Flags = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | kXboxFlagBit | kTightAlignment);

    // A PC device with ID3D12Device10: the render target comes back, as the console's call returns it.
    Mock* mock = nullptr;
    void* device = make_device(mock, 10, true);
    EXPECT(device && xs::device_proxy_slot_count(device) == 79);
    xs::reset_device_proxy_stats();
    g_committed3 = {};
    void* resource = nullptr;
    std::uint64_t words[10] = {word(&heap), kHeapNotZeroed, word(&texture), kBarrierLayoutUndefined,
                               word(&clear), word(&session_sentinel), 2, word(castable), word(&kResourceIid), word(&resource)};
    EXPECT(hr_of(raw10(device, 127, words)) == S_OK);
    EXPECT(resource && reinterpret_cast<Mock*>(resource)->kind == Kind::resource);
    EXPECT(g_committed3.calls == 1 && xs::device_slot_hits(127) == 1);
    EXPECT(g_committed3.heap == &heap && g_committed3.clear == &clear);
    EXPECT(g_committed3.heap_flags == static_cast<D3D12_HEAP_FLAGS>(kHeapNotZeroed));  // the title's heap flags
    EXPECT(g_committed3.session == &session_sentinel);                                  // and protected session
    EXPECT(g_committed3.layout == kBarrierLayoutUndefined);
    EXPECT(g_committed3.castable_count == 2 && g_committed3.castable_formats == castable);
    EXPECT(g_committed3.riid == &kResourceIid);
    EXPECT(g_committed3.desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN);
    // The Xbox-only bit goes; every PC bit stays, tight alignment included.
    EXPECT(static_cast<std::uint32_t>(g_committed3.desc.Flags) ==
           (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | kTightAlignment));
    EXPECT(g_committed3.desc.Width == 1920 && g_committed3.desc.Height == 1080 &&
           g_committed3.desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS && g_committed3.desc.SampleDesc.Count == 1);
    EXPECT(texture.Layout == static_cast<D3D12_TEXTURE_LAYOUT>(kXboxLayoutTiled));  // the title's description is read only
    EXPECT(release(resource) == 0);  // the title's one reference is the only one: nothing leaked, nothing extra

    // A buffer takes the row-major layout.
    D3D12_RESOURCE_DESC1 buffer = texture;
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Height = 1;
    buffer.Format = DXGI_FORMAT_UNKNOWN;
    words[2] = word(&buffer);
    words[6] = 0;
    words[7] = 0;
    resource = nullptr;
    EXPECT(hr_of(raw10(device, 127, words)) == S_OK && resource);
    EXPECT(g_committed3.calls == 2 && g_committed3.desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    EXPECT(release(resource) == 0);

    // The PC device's own refusal reaches the title with a null object.
    mock->create_result = E_OUTOFMEMORY;
    resource = reinterpret_cast<void*>(1);
    words[9] = word(&resource);
    EXPECT(hr_of(raw10(device, 127, words)) == E_OUTOFMEMORY && !resource);
    mock->create_result = S_OK;

    // A null heap, description, IID or output is refused before the PC device is called.
    const unsigned before = g_committed3.calls;
    std::uint64_t bad[10];
    std::memcpy(bad, words, sizeof(bad));
    bad[0] = 0;
    EXPECT(hr_of(raw10(device, 127, bad)) == E_POINTER);
    std::memcpy(bad, words, sizeof(bad));
    bad[2] = 0;
    EXPECT(hr_of(raw10(device, 127, bad)) == E_POINTER);
    std::memcpy(bad, words, sizeof(bad));
    bad[8] = 0;
    EXPECT(hr_of(raw10(device, 127, bad)) == E_POINTER);
    std::memcpy(bad, words, sizeof(bad));
    bad[9] = 0;
    EXPECT(hr_of(raw10(device, 127, bad)) == E_POINTER);
    EXPECT(g_committed3.calls == before);
    EXPECT(xs::device_slot_overridden(127));
    EXPECT(release(device) == 0);

    // A PC device whose ID3D12Device10 is another pointer than the device itself: the call goes to that one, and the
    // reference the proxy took on it is given back. The base pointer is a trap that answers E_UNEXPECTED.
    Mock* split_mock = nullptr;
    void* split_device = make_device(split_mock, 10, true);
    EXPECT(split_device);
    Mock* alias = new_mock(Kind::device);
    alias->device_level = 10;
    alias->refs = 1;
    split_mock->device10_alias = alias;
    split_mock->create_result = E_UNEXPECTED;
    resource = nullptr;
    words[9] = word(&resource);
    EXPECT(hr_of(raw10(split_device, 127, words)) == S_OK && resource);
    EXPECT(g_committed3.device == alias);
    EXPECT(alias->refs == 1);
    EXPECT(release(resource) == 0);
    EXPECT(release(split_device) == 0);

    // A PC device that stops at ID3D12Device9 has no CreateCommittedResource3: E_NOTIMPL, a null object, no PC call.
    Mock* old_mock = nullptr;
    void* old_device = make_device(old_mock, 9, true);
    EXPECT(old_device && xs::device_proxy_slot_count(old_device) == 76);
    resource = reinterpret_cast<void*>(1);
    words[9] = word(&resource);
    const unsigned old_before = g_committed3.calls;
    EXPECT(hr_of(raw10(old_device, 127, words)) == E_NOTIMPL && !resource);
    EXPECT(g_committed3.calls == old_before && old_mock->calls[76] == 0);
    EXPECT(release(old_device) == 0);
    std::printf("%s: device slot 127 creates the committed resource with the PC flags and layout\n", g_label);
#endif
    return 0;
}

// ---- Device slot 127 takes the resource's memory through the title's XMem hook ----
//
// The console runtime allocates a committed resource's memory with XMemAlloc, and a title's hook may count it (a budget
// check after its first render targets). The proxy calls the installed hook with the console allocation size and the
// committed-resource attributes, and gives the memory back through the hook's free when the resource dies.
int check_committed_resource3_xmem_hook() {
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC1 texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = 3840;
    texture.Height = 2160;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Flags = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    constexpr UINT64 kSize = 33423360;  // 3840 x 2176 x 4: whole 64 KiB tiles
    constexpr std::uintptr_t kArena = 0x7a000000;

    Mock* mock = nullptr;
    void* device = make_device(mock, 10, true);
    EXPECT(device);
    mock->allocation_size = kSize;
    g_heap_hook = {};
    g_heap_hook.next_ptr = reinterpret_cast<void*>(kArena);
    xs::set_descriptor_heap_allocator(&mock_heap_alloc, &mock_heap_free);

    void* resource = nullptr;
    std::uint64_t words[10] = {word(&heap), 0, word(&texture), 0, 0, 0, 0, 0, word(&kResourceIid), word(&resource)};
    EXPECT(hr_of(raw10(device, 127, words)) == S_OK && resource);
    EXPECT(g_heap_hook.alloc_calls == 1 && g_heap_hook.last_alloc_size == kSize);
    EXPECT(g_heap_hook.last_alloc_attr == xs::kXmemCommittedResourceAttributes);
    EXPECT(((g_heap_hook.last_alloc_attr >> 14) & 3) == 2 && ((g_heap_hook.last_alloc_attr >> 16) & 0xff) == 0x80);
    EXPECT(mock->calls[25] == 0);  // a listed render target takes its console tiles, not the PC size
    Mock* const res = reinterpret_cast<Mock*>(resource);
    EXPECT(res->private_data != nullptr && g_heap_hook.free_calls == 0);  // held while the resource lives
    EXPECT(release(resource) == 0);
    EXPECT(g_heap_hook.free_calls == 1 && g_heap_hook.last_free_ptr == reinterpret_cast<void*>(kArena) &&
           g_heap_hook.last_free_attr == xs::kXmemCommittedResourceAttributes);

    // A hook that refuses: the resource is still created and nothing is freed.
    g_heap_hook = {};
    resource = nullptr;
    EXPECT(hr_of(raw10(device, 127, words)) == S_OK && resource);
    EXPECT(g_heap_hook.alloc_calls == 1 && reinterpret_cast<Mock*>(resource)->private_data == nullptr);
    EXPECT(release(resource) == 0 && g_heap_hook.free_calls == 0);

    // The PC device's refusal: no resource, so the hook is not asked.
    g_heap_hook = {};
    g_heap_hook.next_ptr = reinterpret_cast<void*>(kArena);
    mock->create_result = E_OUTOFMEMORY;
    EXPECT(hr_of(raw10(device, 127, words)) == E_OUTOFMEMORY && !resource);
    EXPECT(g_heap_hook.alloc_calls == 0);
    mock->create_result = S_OK;

    // No hook installed: slot 127 behaves as before.
    xs::set_descriptor_heap_allocator(nullptr, nullptr);
    g_heap_hook = {};
    EXPECT(hr_of(raw10(device, 127, words)) == S_OK && resource);
    EXPECT(g_heap_hook.alloc_calls == 0 && reinterpret_cast<Mock*>(resource)->private_data == nullptr);
    EXPECT(release(resource) == 0);
    EXPECT(release(device) == 0);
    std::printf("%s: device slot 127 takes the resource's memory through the XMem hook and frees it with the resource\n",
                g_label);
#endif
    return 0;
}

// ---- Committed buffers on the title's XMem hook memory ----
//
// With a placed-at-VA handler registered (the Vulkan layer registers create_buffer_at_host_address), a committed buffer
// whose memory the hook gives is made on that memory by the handler instead of on the PC device's memory, so the
// title's CPU view of those bytes is the resource. A texture, a shared heap, a handler that fails and a hook that
// refuses keep the PC device's resource, with the hook memory as bookkeeping where the hook gave some.
struct PlacedHandlerState {
    int calls = 0;
    ID3D12Device* real = nullptr;
    void* va = nullptr;
    D3D12_RESOURCE_DESC desc{};
    unsigned char first_byte = 0xcc;  // the memory's first byte when the handler ran: zeroed or the title's
    HRESULT result = S_OK;
} g_placed;

HRESULT mock_placed_handler(ID3D12Device* real, void* va, const D3D12_RESOURCE_DESC& desc, std::uint32_t,
                           REFIID riid, void** out) {
    ++g_placed.calls;
    g_placed.real = real;
    g_placed.va = va;
    g_placed.desc = desc;
    g_placed.first_byte = *static_cast<const unsigned char*>(va);
    if (FAILED(g_placed.result)) return g_placed.result;
    Mock* child = new_mock(Kind::resource);
    return mock_query(child, &riid, out);
}

int check_committed_buffers_on_xmem() {
    constexpr std::size_t kTaken = 0x20000;  // two 64 KiB pages: what GetResourceAllocationInfo answers
    alignas(4096) static unsigned char arena[kTaken];  // the mock handler checks no alignment
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 0x18000;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);  // the Xbox row-major word slot 76 replaces
    D3D12_RESOURCE_DESC texture = buffer;
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = texture.Height = 256;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);

    Mock* mock = nullptr;
    void* device = make_device(mock, 10, true);
    EXPECT(device);
    mock->allocation_size = kTaken;
    xs::set_descriptor_heap_allocator(&mock_heap_alloc, &mock_heap_free);
    xs::set_placed_at_va_handler(&mock_placed_handler);
    const auto reset = [&] {
        g_heap_hook = {};
        g_heap_hook.next_ptr = arena;
        g_placed = {};
        std::memset(arena, 0xab, sizeof(arena));
    };
    // Slot 76: (heap, flags, desc, state, clear, riid, out).
    const auto slot76 = [&](const D3D12_RESOURCE_DESC& desc, std::uint32_t flags, void** out) {
        const std::uint64_t w[10] = {word(&heap), flags, word(&desc), 0, 0, word(&kResourceIid), word(out), 0, 0, 0};
        return hr_of(raw10(device, 76, w));
    };

    // A buffer: the handler makes it on the hook's memory, zeroed first; the PC device makes nothing; the memory goes
    // back through the hook's free with the resource.
    reset();
    void* resource = nullptr;
    EXPECT(slot76(buffer, 0, &resource) == S_OK && resource);
    EXPECT(g_heap_hook.alloc_calls == 1 && g_heap_hook.last_alloc_size == kTaken &&
           g_heap_hook.last_alloc_attr == xs::kXmemCommittedResourceAttributes);
    EXPECT(g_placed.calls == 1 && g_placed.va == arena && g_placed.real == reinterpret_cast<ID3D12Device*>(mock));
    EXPECT(g_placed.desc.Width == buffer.Width && g_placed.desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    EXPECT(g_placed.first_byte == 0 && arena[kTaken - 1] == 0);  // the whole allocation, as a committed resource
    EXPECT(mock->calls[27] == 0);
    EXPECT(reinterpret_cast<Mock*>(resource)->private_data != nullptr && g_heap_hook.free_calls == 0);
    EXPECT(release(resource) == 0);
    EXPECT(g_heap_hook.free_calls == 1 && g_heap_hook.last_free_ptr == arena);

    // D3D12_HEAP_FLAG_CREATE_NOT_ZEROED: the title's bytes stay.
    reset();
    resource = nullptr;
    EXPECT(slot76(buffer, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &resource) == S_OK && resource);
    EXPECT(g_placed.calls == 1 && g_placed.first_byte == 0xab);
    EXPECT(release(resource) == 0 && g_heap_hook.free_calls == 1);

    // A texture: the PC device makes it, the hook memory is bookkeeping, the handler is not asked.
    reset();
    resource = nullptr;
    const unsigned pc_before = mock->calls[27];
    EXPECT(slot76(texture, 0, &resource) == S_OK && resource);
    EXPECT(g_placed.calls == 0 && mock->calls[27] == pc_before + 1 && g_heap_hook.alloc_calls == 1);
    EXPECT(arena[0] == 0xab);  // bookkeeping memory is the title's, not touched
    EXPECT(release(resource) == 0 && g_heap_hook.free_calls == 1);

    // A shared heap: the PC device's memory, the hook memory as bookkeeping.
    reset();
    resource = nullptr;
    EXPECT(slot76(buffer, D3D12_HEAP_FLAG_SHARED, &resource) == S_OK && resource);
    EXPECT(g_placed.calls == 0 && g_heap_hook.alloc_calls == 1 && arena[0] == 0xab);
    EXPECT(release(resource) == 0 && g_heap_hook.free_calls == 1);

    // The handler cannot use the memory: the PC device makes the buffer, and the memory the hook gave once is the
    // bookkeeping (not a second allocation).
    reset();
    g_placed.result = E_INVALIDARG;
    resource = nullptr;
    const unsigned pc_fallback_before = mock->calls[27];
    EXPECT(slot76(buffer, 0, &resource) == S_OK && resource);
    EXPECT(g_placed.calls == 1 && mock->calls[27] == pc_fallback_before + 1 && g_heap_hook.alloc_calls == 1);
    EXPECT(reinterpret_cast<Mock*>(resource)->kind == Kind::resource && reinterpret_cast<Mock*>(resource)->private_data);
    EXPECT(release(resource) == 0 && g_heap_hook.free_calls == 1);

    // ...and the PC device refuses too: the title gets the PC answer and a null object, the memory goes back at once.
    reset();
    g_placed.result = E_INVALIDARG;
    mock->create_result = E_OUTOFMEMORY;
    resource = reinterpret_cast<void*>(1);
    EXPECT(slot76(buffer, 0, &resource) == E_OUTOFMEMORY && !resource);
    EXPECT(g_heap_hook.alloc_calls == 1 && g_heap_hook.free_calls == 1 && g_heap_hook.last_free_ptr == arena);
    mock->create_result = S_OK;

    // The hook refuses: the PC device makes the buffer, the handler is not asked.
    reset();
    g_heap_hook.next_ptr = nullptr;
    resource = nullptr;
    EXPECT(slot76(buffer, 0, &resource) == S_OK && resource);
    EXPECT(g_heap_hook.alloc_calls == 1 && g_placed.calls == 0 && !reinterpret_cast<Mock*>(resource)->private_data);
    EXPECT(release(resource) == 0 && g_heap_hook.free_calls == 0);

    // Slot 27 (CreateCommittedResource) and slot 53 (CreateCommittedResource1) take the same path; their arguments
    // reach the PC device unchanged when it makes the resource. (Slot 76's PC calls above went through PC slot 27.)
    g_committed[0] = g_committed[1] = {};
    for (const unsigned slot : {27u, 53u}) {
        const int which = slot == 27 ? 0 : 1;
        const auto call = [&](const D3D12_RESOURCE_DESC& desc, void** out, const IID* riid = &kResourceIid) {
            const std::uint64_t session = 0x5e55;
            std::uint64_t w[10] = {word(&heap), 0, word(&desc), 0, 0, word(riid), word(out), 0, 0, 0};
            if (slot == 53) {  // (heap, flags, desc, state, clear, session, riid, out)
                w[5] = session;
                w[6] = word(riid);
                w[7] = word(out);
            }
            return hr_of(raw10(device, slot, w));
        };
        reset();
        resource = nullptr;
        D3D12_RESOURCE_DESC pc_buffer = buffer;
        pc_buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        EXPECT(call(pc_buffer, &resource) == S_OK && resource);
        EXPECT(g_placed.calls == 1 && g_placed.va == arena && g_committed[which].calls == 0);
        EXPECT(release(resource) == 0 && g_heap_hook.free_calls == 1);

        reset();
        resource = nullptr;
        D3D12_RESOURCE_DESC pc_texture = texture;
        pc_texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        EXPECT(call(pc_texture, &resource) == S_OK && resource);
        EXPECT(g_placed.calls == 0 && g_committed[which].calls == 1 && g_heap_hook.alloc_calls == 1);
        EXPECT(g_committed[which].desc.Width == 256 && g_committed[which].riid == &kResourceIid);
        if (slot == 53) EXPECT(g_committed[which].session == reinterpret_cast<const void*>(0x5e55));
        EXPECT(release(resource) == 0 && g_heap_hook.free_calls == 1);

        // A capability check (no output) is the PC device's, and takes no memory from the title.
        reset();
        EXPECT(call(pc_texture, nullptr) == S_FALSE);
        EXPECT(g_committed[which].calls == 2 && g_heap_hook.alloc_calls == 0 && g_placed.calls == 0);
        EXPECT(xs::device_slot_overridden(slot));
        g_committed[which] = {};
    }

    // Slot 127's buffer goes on the hook memory the same way.
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    {
        reset();
        D3D12_RESOURCE_DESC1 buffer1{};
        buffer1.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer1.Width = buffer.Width;
        buffer1.Height = buffer1.DepthOrArraySize = buffer1.MipLevels = 1;
        buffer1.SampleDesc.Count = 1;
        buffer1.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);
        g_committed3 = {};
        resource = nullptr;
        std::uint64_t words[10] = {word(&heap), 0, word(&buffer1), 0, 0, 0, 0, 0, word(&kResourceIid), word(&resource)};
        EXPECT(hr_of(raw10(device, 127, words)) == S_OK && resource);
        EXPECT(g_placed.calls == 1 && g_placed.va == arena && g_committed3.calls == 0);
        EXPECT(g_placed.desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
        EXPECT(release(resource) == 0 && g_heap_hook.free_calls == 1);
    }
#endif

    // No hook installed: the handler is never asked, even for a buffer.
    xs::set_descriptor_heap_allocator(nullptr, nullptr);
    reset();
    resource = nullptr;
    EXPECT(slot76(buffer, 0, &resource) == S_OK && resource);
    EXPECT(g_placed.calls == 0 && g_heap_hook.alloc_calls == 0 && arena[0] == 0xab);
    EXPECT(release(resource) == 0);

    xs::set_placed_at_va_handler(nullptr);
    EXPECT(release(device) == 0);
    std::printf("%s: committed buffers go on the title's XMem hook memory (slots 27, 53, 76, 127); textures, shared "
                "heaps and refusals keep the PC device's memory\n",
                g_label);
    return 0;
}

// ---- 12. List tail slot names (D3D-445) ---------------------------------------------------------------------
// From CopyTextureRegionX on, the newer layout sits two slots later than the older one (d3d12_xs_console_abi.md
// 5.3). A diagnostic name follows the device-wide layout, so slot 85 of the newer layout and slot 83 of the older
// layout are both CopyTextureRegionX, and neither number gets that name under the other layout.

int check_list_tail_names() {
    const unsigned char older = xs::kConsoleLayoutOlder, newer = xs::kConsoleLayoutNewer;
    const auto same = [](const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; };
    EXPECT(same(xs::list_tail_slot_name(83, older), "CopyTextureRegionX"));
    EXPECT(same(xs::list_tail_slot_name(85, newer), "CopyTextureRegionX"));
    // The same numbers under the other layout are different methods.
    EXPECT(!xs::list_tail_slot_name(83, newer));  // not a measured slot of the newer layout
    EXPECT(xs::list_tail_slot_name(85, older) && !same(xs::list_tail_slot_name(85, older), "CopyTextureRegionX"));
    // Every measured newer->older pair has one name, and the eight names differ.
    const unsigned pairs[][2] = {{85, 83}, {86, 84}, {87, 85}, {90, 88}, {91, 89}, {92, 90}, {95, 93}, {96, 94}};
    for (const auto& pair : pairs) {
        EXPECT(same(xs::list_tail_slot_name(pair[0], newer), xs::list_tail_slot_name(pair[1], older)));
        for (const auto& other : pairs)
            EXPECT(pair[1] == other[1] ||
                   !same(xs::list_tail_slot_name(pair[1], older), xs::list_tail_slot_name(other[1], older)));
    }
    // Slots outside the measured set have no tail name in either layout, and no slot has one before the layout is
    // known: the log then prints the slot number only, as before.
    for (unsigned slot = 0; slot < 160; ++slot) {
        bool older_tail = false, newer_tail = false;
        for (const auto& pair : pairs) {
            newer_tail = newer_tail || slot == pair[0];
            older_tail = older_tail || slot == pair[1];
        }
        EXPECT((xs::list_tail_slot_name(slot, older) != nullptr) == older_tail);
        EXPECT((xs::list_tail_slot_name(slot, newer) != nullptr) == newer_tail);
        EXPECT(!xs::list_tail_slot_name(slot, 0));
        EXPECT(!xs::list_tail_slot_name(slot, 3));
    }
    return 0;
}

}  // namespace

// A failure ends the process with a message on stderr, never in a dialog a test run would wait on: abort() (an
// exception that escapes, for one), the debug CRT's reports, and Windows' fault and critical-error boxes.
static void no_dialogs() {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG  // the report functions exist only in the debug CRT (empty macros otherwise)
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

// Slot 14: the title's 1,245,184-descriptor shader-visible CBV/SRV/UAV heap. A device that makes it (the Vulkan
// device has no limit) keeps the full size. A device that refuses it, as native D3D12 does past
// MaxViewDescriptorHeapSize (the mock reports none, so 1,000,000), gets a second request at that limit. CPU-only
// heaps and heaps within the limit are passed through once, as asked.
int check_view_heap_cap() {
    const IID heap_iid = __uuidof(ID3D12DescriptorHeap);
    void* made = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1245184,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    {
        Mock* mock = nullptr;
        void* device = make_device(mock, 0, false);   // no view-heap limit
        EXPECT(device);
        raw(device, 14, word(&heap), word(&heap_iid), word(&made));
        if (made) release(made);  // the heap proxy holds a device reference
        EXPECT(mock->calls[14] == 1 && mock->array[14][0] == 1245184);   // not capped when the device takes it
        EXPECT(release(device) == 0);
    }
    Mock* mock = nullptr;
    void* device = make_device(mock, 0, false);
    EXPECT(device);
    mock->view_heap_limit = D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2;
    raw(device, 14, word(&heap), word(&heap_iid), word(&made));
    if (made) release(made);  // the heap proxy holds a device reference
    EXPECT(mock->calls[14] == 2 && mock->array[14][0] == 1245184 &&
           mock->array[14][1] == D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2);   // refused, then at the limit
    EXPECT(heap.NumDescriptors == 1245184);   // the title's description is left as it was
    mock->calls[14] = 0;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    raw(device, 14, word(&heap), word(&heap_iid), word(&made));
    if (made) release(made);  // the heap proxy holds a device reference
    EXPECT(mock->calls[14] == 1 && mock->array[14][0] == 1245184);   // CPU-only: no limit, one request
    mock->calls[14] = 0;
    heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    raw(device, 14, word(&heap), word(&heap_iid), word(&made));
    if (made) release(made);  // the heap proxy holds a device reference
    EXPECT(mock->calls[14] == 1 && mock->array[14][0] == 4096);   // within the limit: one request, unchanged
    EXPECT(release(device) == 0);
    std::printf("%s: a view heap the device refuses past its limit is made again at the limit; one it takes is kept "
                "at full size\n", g_label);
    return 0;
}

int main(int argc, char** argv) {
    no_dialogs();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2) {
        if (std::strcmp(argv[1], "--env-slots") == 0) return run_env_slots(true);
        if (std::strcmp(argv[1], "--env-slots-invalid") == 0) return run_env_slots(false);
        if (std::strcmp(argv[1], "--env-slots-70") == 0) return run_env_slots(true, true);
        if (std::strcmp(argv[1], "--env-slots-74") == 0) return run_env_slots(true, false, true);
        return run_fault_mode(argv[1]);
    }
    if (argc != 1) return 2;
    if (check_tail(true) || check_levels() || check_distinct_device_interfaces() || check_slot48() || check_fence1() ||
        check_fence_completed_value() || check_create_failures() || check_open_shared() || check_alloc_failures() ||
        check_allocators() || check_render_target_sizes() || check_allocation_descriptions_cleaned() ||
        check_render_target_layout() || check_xbox_slots() || check_xbox_queue_slots() || check_custom_heap_properties() ||
        check_slot70() || check_slot70_raw_buffer() || check_descriptor_heap_hooks() || check_committed_resource3_x() ||
        check_committed_resource3_xmem_hook() || check_committed_buffers_on_xmem() || check_slot74() ||
        check_view_heap_cap() || check_lists() || check_signalx_detection() || check_list_tail_names() ||
        check_get_desc_c_abi() || check_list_cpu_handles())
        return 1;
    std::printf("xs_proxy_mock_test: PASS (%d allocations refused on purpose)\n", g_failed_news);
    return 0;
}
