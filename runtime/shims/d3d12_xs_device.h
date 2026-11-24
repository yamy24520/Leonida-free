// Xbox-ABI proxy around a PC ID3D12Device.
//
// Measured Xbox device deviations used here (vtable indices, x64):
//   slot 26   GetCustomHeapProperties, the PC slot, but the retail executable calls it with the buffer for the
//             returned struct in RCX and the device in RDX (node mask in R8, heap type in R9). A PC C++ method has
//             the device in RCX and the buffer in RDX. The thunk takes the console form when RDX holds a device
//             proxy, and forwards the PC form otherwise. Slots 25 and 43 return structs too and are still
//             forwarded in the PC form: 25 is reported called the same way later in the boot (#6982), with console
//             resource descriptions the PC method cannot take as they are, and no call of 43 has been seen.
//   slot 61   CreateSamplerX(desc, dest) -> PC CreateSampler (slot 22); the description's PC
//             D3D12_SAMPLER_DESC prefix is passed. The PC slot 61 is CreateMetaCommand. Measured on the retail title's boot.
//   slot 41   CreateCommandSignature at its PC position; a console argument of type 105 (a 12-byte dispatch, read
//             from the retail title's call sites) is created as D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH.
//   slot 62   CreateComputePipelineState(desc, w2, w3, iid, out): two words (0 from XboxBvhBuilder, meaning unknown)
//             before the IID. The PC slot 62 is CreateStateObject.
//   slot 67   CreateComputePipelineState from native code (desc, iid, out): converts the Xbox native-code desc
//             (+0x00 root signature, +0x28 code, +0x30 size) to D3D12_COMPUTE_PIPELINE_STATE_DESC and forwards
//             to PC CreateComputePipelineState (slot 11). The PC slot 67 is CreateProtectedResourceSession1.
//   slot 70   (texture words, view desc, destination handle), no return value: a shader-resource view from raw words
//             and a GPU address, with no resource (retail boot chain stop 14). The PC slot 70 is CreatePlacedResource1.
//             A measured shape (PR 7066, eugen1763): a raw buffer SRV. arg1 starts with a desktop D3D12_RESOURCE_DESC
//             (buffer), arg2 with a desktop D3D12_SHADER_RESOURCE_VIEW_DESC (R32_TYPELESS, BUFFER, RAW) followed at
//             once by a D3D12_GPU_VIRTUAL_ADDRESS, the console VA a slot-133 call registered. When the VA is inside a
//             buffer the device's resource provider knows, the destination gets a raw view of it (PC
//             CreateShaderResourceView with that resource). Any other shape, or an unknown VA, gets the PC null view
//             (CreateShaderResourceView with no resource) and the call returns.
//   slot 74   (pointer, u32, pointer): not measured, returns S_OK. The PC slot 74 is ShaderCacheControl(kinds, control),
//             which reads the first word as flags and returns E_INVALIDARG. Nothing is written, the PC device is not called.
//   slot 76   resource creation -> PC CreateCommittedResource (PC slot 27), with the Xbox layout word ignored.
//   slot 127  CreateCommittedResource3 (heap, flags, desc1, layout, clear, session, castable count/list, riid, out) ->
//             PC ID3D12Device10::CreateCommittedResource3 (PC slot 76), with the Xbox layout word replaced and the
//             Xbox flag bits dropped (RT-5 stop 15, docs/retail_boot_chain.md). E_NOTIMPL when the PC device lacks it.
//   slot 26   GetCustomHeapProperties, called by the retail title with the C ABI: return buffer in RCX, the device
//             in RDX (the PC form is `this` in RCX, return buffer in RDX). Slot 25 (GetResourceAllocationInfo, also a
//             struct wider than 8 bytes) takes the same form by the same rule. Both accept either form.
//   slot 48   CreateCommandQueueX(desc, riid, out) -> the same queue proxy as slot 8. The 32-byte Xbox desc starts with
//             a D3D12_COMMAND_QUEUE_DESC; only that prefix is passed on, with the console-only flag bits cleared
//             (DISABLE_GPU_TIMEOUT is kept). The four extra words are logged once.
//   slot 73   CreateCommandListX(desc, allocator, initialState, riid, out) -> PC CreateCommandList(0, type, allocator,
//             initialState, riid, out). The type is the first word of the Xbox desc; there is no node-mask argument
//             in the console call (public HLE; the PC six-argument form differs).
//   slots 78/79/80  SetFrameIntervalX, ScheduleFrameEventX, WaitFrameEventX. A per-device token state machine answers
//             them before the PC vtable is indexed; WaitFrameEventX returns at once with a fresh non-zero token.
//             Slot 80 is outside a PC ID3D12Device11 table, so it is checked before the beyond-table refusal.
//   slot 86   CreatePipelineState(desc, riid, out) -> PC ID3D12Device2::CreatePipelineState (PC slot 47).
//   slot 101  CreateStateObject(desc, riid, out) -> PC ID3D12Device5::CreateStateObject (PC slot 62, displaced on
//             Xbox by compute-PSO slot 62).
//   slot 108  GetResourceAllocationInfo2(ret, this, visible_mask, count, descs, info1) -> PC
//             ID3D12Device8::GetResourceAllocationInfo2 (PC slot 68), accepting the console C-ABI struct return
//             (return buffer in RCX, device in RDX) as well as the PC C++ order, with each 56-byte
//             D3D12_RESOURCE_DESC expanded to a zero-initialized D3D12_RESOURCE_DESC1 and Xbox layout words and
//             flag bits normalized on the copy.
// The slots 48/73/78/79/80 are the public HLE Freminet/ScarlettX's observation, not Microsoft documentation, and can
// be moved with LEONIDA_XS_QUEUE_X_SLOT, LEONIDA_XS_LIST_X_SLOT, LEONIDA_XS_FRAME_INTERVAL_SLOT,
// LEONIDA_XS_SCHEDULE_EVENT_SLOT and LEONIDA_XS_WAIT_EVENT_SLOT (3..127; anything else is logged once and ignored).
// The measured slots 9/10 (GetCPU/GPUDescriptorHandleForHeapStart) are ID3D12DescriptorHeap methods, not device
// methods: the PC device's 9/10 are CreateCommandAllocator/CreateGraphicsPipelineState and are forwarded unchanged.
// On the PC heap those getters use the hidden return pointer in RDX; the Xbox ABI returns the 8-byte handle in
// RAX. Device slot 14 therefore wraps the created heap in a heap proxy that performs that conversion. The heap
// proxy holds a reference to the device proxy, and its ID3D12DeviceChild::GetDevice returns that same proxy.
// The proxy's heap slots 9/10 expose only the Xbox shape: a PC C++ caller using the PC header's return convention
// would not receive the value. Heap slot 8 (GetDesc) and queue slot 18 (GetDesc) translate the console x64 C-ABI
// form (return buffer in RCX, proxy in RDX) to PC order; PC-order calls on queue slot 18 (proxy in RCX) fall through
// to the standard queue slot forwarding / layout-detection path, and the remaining heap slots forward unchanged. A
// direct one-argument call against a real PC heap is invalid, so the device test exercises the conversion on the
// wrapper and compares it with a hidden-pointer call against the real heap.
//
// Every other PC device slot is forwarded by a generated thunk that swaps `this` to the PC device. The proxy's
// vtable therefore follows the PC device order, with slots 62 and 76 and the public HLE's 48/73/78/79/80 deliberately
// shadowed. The full Xbox vtable order is not known from the spec; slots beyond the PC device's own vtable return
// E_NOTIMPL and log once.
//
// Command queues and fences (device slots 8, 75, 36, and 32 for a shared fence) come back wrapped in queue and fence
// proxies of the same kind: the public chain (19 queue slots, 11 or 12 fence slots in SDK 10.0.26100) is forwarded
// in PC order. Queue slot 35 is the console ExecuteCommandLists(count, lists) variant and forwards to PC slot 10 with
// the list array unwrapped; fence slot 12 returns the address of the fence's 64-bit completed value (the retail
// executable reads it so), a per-fence mirror that a wait on the real fence keeps current (d3d12_xs_device.cpp, "Fence
// slot 12"). Any other slot past the public chain, up to the end of the proxy's table (128 queue, 64 fence slots), is
// logged with its first four argument words and answered S_OK on a queue or refused with E_NOTIMPL on a fence or
// allocator. A slot past the table faults on the vtable read inside a reserved, never-committed region, and the fault
// is logged with the slot. The sibling-layer measurement puts PresentX at queue slot 30 or 32 and SignalX at 23 or 25
// by runtime layout (docs/d3d12_xs_console_abi.md §4, unverified here). The proxy detects the layout from a fence
// reaching slot 16/17, forwards SignalX as PC Signal, routes PresentX to the presentation core under
// LEONIDA_VULKAN_DEVICE and answers the remaining console-only queue methods S_OK (frame pacing flows through them)
// without forwarding; that the console keeps the public slots is an assumption (docs/d3d12_command_objects.md §5.2).
// Heap, queue and fence proxies are unwrapped wherever a device or queue method hands them to the real runtime: queue
// Signal/Wait (14/15), device CreateSharedHandle (31), MakeResident/Evict (34/35), SetEventOnMultipleFenceCompletion
// (45), SetResidencyPriority (46) and EnqueueMakeResident (50). Command lists come back from device slots 12 and 73
// behind a console-shaped list proxy: console vtable order (PC order through slot 26, four console-only extras at
// 27-30, the PC chain shifted +4 through console 59, measured tail slots at 60/61/139/146/148), unmodelled slots
// recorded and refused. Queue ExecuteCommandLists (10 and 35) unwraps the list array; the list's own Reset,
// ExecuteBundle and SetDescriptorHeaps unwrap allocator, list and heap proxies. QueryInterface answers only the
// modelled interfaces; one the real object has but the proxy does not model is logged once and refused.
#pragma once

#include <d3d12.h>

#include <cstddef>
#include <cstdint>

namespace rt::d3d12_xs {

// Wraps `real` and returns the requested device interface through `out`. Takes ownership of one reference to
// `real` on success; on failure the caller keeps its reference. Returns E_NOINTERFACE if `riid` is not a D3D12
// device interface the PC device implements.
// `console_lists` shapes the command lists CreateCommandList returns: true (the title path) gives the
// console-shaped object the Xbox ABI calls; false hands the real list back for callers compiled against
// the PC headers (the GPU test harnesses), whose vtable indices past slot 26 do not exist in console order.
// D3D12XboxCreateDevice opts in; the default is PC-shaped so direct callers keep the old behaviour.
HRESULT create_device_proxy(ID3D12Device* real, REFIID riid, void** out, bool console_lists = false);

// True when `device` points at a proxy created by create_device_proxy. Used by the tests.
bool is_device_proxy(const void* device);

// IDXGIDevice1 on the device. A GDKX title asks its device for IDXGIDevice1, then GetAdapter and EnumOutputs(0),
// to find the output it gives SetFrameIntervalX (docs/d3d12_vulkan.md section 4.5). The proxy answers IDXGIObject,
// IDXGIDevice and IDXGIDevice1 itself, as one COM object with the device (same reference count, QueryInterface
// both ways). GetAdapter and GetParent return the adapter of the device behind the proxy: by default the host DXGI
// adapter with that device's LUID. A provider replaces that: `real` is the device behind the proxy, and it returns
// the adapter as `riid`. rt::d3d12::set_device_outer sets one for the Vulkan-backed device (the layer's own adapter).
//
// A title may ask D3D12XboxCreateDevice (so create_device_proxy) for IDXGIDevice1 directly. What comes back then is
// the proxy's DXGI pointer, not the proxy: is_device_proxy is false for it. Code that takes "the proxy" from a
// creation call goes through device_proxy_primary first.
using DxgiAdapterProvider = HRESULT (*)(ID3D12Device* real, REFIID riid, void** out);
// `device` is the proxy or its DXGI pointer. False, with a line on stderr, when it is neither: nothing is set then.
bool set_device_adapter_provider(void* device, DxgiAdapterProvider provider);
// Finds the buffer resource that holds a console GPU virtual address, for device slot 70. `real` is the device behind
// the proxy; `bytes` is how many bytes from `va` the caller wants, so a resource that holds all of them is preferred
// over a shorter alias. Returns the resource with a reference added (the proxy releases it), or null when no resource holds
// `va`; `start` and `size` are then unused, otherwise the resource's first VA and size in bytes.
using ResourceByVaProvider = ID3D12Resource* (*)(ID3D12Device* real, std::uint64_t va, std::uint64_t bytes,
                                                std::uint64_t* start, std::uint64_t* size);
// `device` is the proxy or its DXGI pointer. False, with a line on stderr, when it is neither. Without a provider slot
// 70 writes only the null view.
bool set_device_resource_provider(void* device, ResourceByVaProvider provider);
// The device proxy an IDXGIDevice pointer handed out by one belongs to, or null.
void* device_proxy_from_dxgi_device(void* dxgi_device);
// The device proxy itself, given the proxy or its DXGI pointer; null for anything else.
void* device_proxy_primary(void* device);

// True when `heap` is a descriptor-heap proxy created by device slot 14.
bool is_heap_proxy(const void* heap);
ID3D12DescriptorHeap* heap_proxy_real_heap(const void* heap);

// Console D3D12 descriptor heaps allocate their backing through XMem (allocator ID 0x80 = eXALLOCAllocatorId_D3D in
// bits 16..23, object type 4 in the low 14 bits). A title that intercepts this allocation via __XMemAllocationHooks__
// returns its own virtual address and expects GetCPUDescriptorHandleForHeapStart() to return that address.
inline constexpr std::uint64_t kXmemDescriptorHeapAttributes = (0x80ULL << 16) | 4ULL;
using DescriptorHeapBackingAlloc = void* (*)(std::size_t byte_size, std::uint64_t attributes);
using DescriptorHeapBackingFree = void (*)(void* ptr, std::uint64_t attributes);
// Installs (or clears when both are null) the backing allocator consulted by CreateDescriptorHeap (slot 14).
// Committed resources take their memory through the same hooks. Measured locally at a title's call sites: around its
// first full-screen render targets (device slot 127) the title installs an allocation hook that takes only allocations
// whose page-size field (bits 14..15) is 2, bump-allocates them from an arena of a fixed size, and then asserts that
// little of the arena is left unused, so a resource the hook never sees fails the check. The object type of a resource
// allocation is not measured and is left 0; the allocator ID is the D3D one above.
inline constexpr std::uint64_t kXmemCommittedResourceAttributes = (0x80ULL << 16) | (2ULL << 14);
// Installs (or clears when both are null) the backing allocator consulted by CreateDescriptorHeap (slot 14) and by the
// committed resources (slots 27, 53, 76 and 127). A committed buffer is made on the memory the allocator gives when the
// device behind the proxy registered a placed-at-VA handler (the Vulkan layer); a texture, or a buffer that handler
// cannot make, stays on the PC device's memory with the allocator's memory as bookkeeping only.
// LEONIDA_XMEM_RESOURCE_BACKING=0 keeps every committed resource on the PC device's memory.
void set_descriptor_heap_allocator(DescriptorHeapBackingAlloc alloc, DescriptorHeapBackingFree free);
// Installs the default bridge (used by D3D12XboxCreateDevice) only when no explicit allocator has been set.
void set_default_descriptor_heap_allocator(DescriptorHeapBackingAlloc alloc, DescriptorHeapBackingFree free);
// Translates a CPU descriptor handle from a title-backed descriptor heap to the underlying PC/Vulkan heap handle.
// Returns `handle` unchanged when it does not fall inside any active custom-backed heap.
D3D12_CPU_DESCRIPTOR_HANDLE translate_cpu_descriptor_handle(const void* device, D3D12_CPU_DESCRIPTOR_HANDLE handle);

// Number of PC vtable slots the proxy forwards (detected from the interfaces the PC device supports).
unsigned device_proxy_slot_count(const void* device);

// The PC device behind a proxy. Null when `device` is not a proxy.
ID3D12Device* device_proxy_real_device(const void* device);

// Device slot 133 (the console's CreatePlacedResourceX) makes a buffer on the title's pages at `va`. The proxy does
// not create it itself: the device behind it does, through this handler, which the Vulkan layer registers
// (rt::d3d12::create_buffer_at_host_address). With none registered, or a PC device behind the proxy, the slot
// answers E_NOTIMPL.
// `layout` is the title's third argument, a D3D12_BARRIER_LAYOUT (0xFFFFFFFF = UNDEFINED), measured on a retail
// boot: 0x6 SHADER_RESOURCE, 0x3 UNORDERED_ACCESS, as PC CreatePlacedResource2 takes it.
using PlacedAtVaHandler = HRESULT (*)(ID3D12Device* real, void* va, const D3D12_RESOURCE_DESC& desc,
                                      std::uint32_t layout, REFIID riid, void** out);
void set_placed_at_va_handler(PlacedAtVaHandler handler);

// True for slots the proxy handles itself rather than forwarding verbatim: IUnknown (0-2), 8 and 75 (queue
// proxies), 9 (allocator proxies), 14 (descriptor heap proxy), 17-24 (CPU descriptor handle translation), 26 (the
// console's struct-return form), 27 and 53 (committed resources, memory through the title's XMem hook),
// 31/34/35/45/46/50 (proxies unwrapped), 32 and 36 (fence proxies), 48/73 and 78/79/80 (the Xbox slots above, at their
// configured indices), 55, 57 (d3d12_xs_native_pso.h), 62, 67, 70, 74, 76, 86, 101, 108 and 127. Slot 10 is
// intentionally false: the measured 9/10 handle conversion is on the heap proxy, and device slot 9 is
// CreateCommandAllocator, which the proxy handles for the allocator proxy.
bool device_slot_overridden(unsigned slot);

// Queue and fence proxies created by device slots 8/75 and 36. The real_* accessors return the wrapped object
// (no reference added), or null when the pointer is not such a proxy.
bool is_queue_proxy(const void* queue);
bool is_fence_proxy(const void* fence);
ID3D12CommandQueue* queue_proxy_real_queue(const void* queue);
ID3D12Fence* fence_proxy_real_fence(const void* fence);
// Public slots forwarded (19 for a queue; 11 or 12 for a fence), and the size of the proxy's vtable.
unsigned queue_proxy_slot_count(const void* queue);
unsigned fence_proxy_slot_count(const void* fence);
unsigned queue_proxy_table_size();
unsigned fence_proxy_table_size();
// True for queue slots handled by the proxy instead of forwarded verbatim past IUnknown: 0-2 (IUnknown),
// 7 (GetDevice), 14/15 (Signal/Wait unwrap fences), and 35 (Xbox ExecuteCommandLists variant -> PC slot 10).
bool queue_slot_overridden(unsigned slot);
// Live references on a queue, fence or allocator proxy (0 for anything else). Test helper.
unsigned long command_object_proxy_ref_count(const void* object);
// Per-slot call counters and first-call flags, shared by all queue proxies and all fence proxies.
std::uint64_t queue_slot_hits(unsigned slot);
bool queue_slot_first_call_seen(unsigned slot);
std::uint64_t fence_slot_hits(unsigned slot);
bool fence_slot_first_call_seen(unsigned slot);
// Vtable reads past a queue or fence proxy table that faulted in its guard region (counted by the fault logger).
std::uint64_t queue_table_overruns();
std::uint64_t fence_table_overruns();
// True when the queue, fence and allocator tables sit in front of their guard regions (false only if a reservation
// or the fault logger failed).
bool command_object_tables_guarded();

// Command allocator proxies (device slot 9): the public chain (9 slots: the base chain and Reset) is forwarded,
// slot 9 (console SetAllocatorCallbackX(callback, context, ...)) records the callback and context on the proxy and
// returns S_OK without invoking the callback on the PC/Vulkan path, slots 10..63 are logged and refused with
// E_NOTIMPL, and a read past the table faults in a guard region, as for queues and fences. Slot 9 wraps only an
// allocator that answers kLeonidaCommandAllocatorTag, Leonida's
// own Vulkan-layer allocator: its command lists resolve the proxy (CreateCommandList, Reset). A PC runtime
// allocator goes back unchanged, because the PC runtime's lists would take a proxy for their own allocator; a
// command-list proxy that unwraps at Reset is what lets the host backend wrap allocators too.
// {7d0e6b51-34a2-4f0c-9a61-0c5b2e8f4a10}: the Vulkan layer's CommandAllocator answers it with its implementation.
inline constexpr GUID kLeonidaCommandAllocatorTag{
    0x7d0e6b51, 0x34a2, 0x4f0c, {0x9a, 0x61, 0x0c, 0x5b, 0x2e, 0x8f, 0x4a, 0x10}};
bool is_allocator_proxy(const void* allocator);
// Command-list proxies (device slot 12): the console-shaped object. is_list_proxy / list_proxy_real_list give the
// same access as the queue, fence and allocator helpers; list_slot_hits counts calls per console slot.
bool is_list_proxy(const void* list);
ID3D12CommandList* list_proxy_real_list(const void* list);
std::uint64_t list_slot_hits(unsigned slot);
// The diagnostic name of a command-list slot on the list tail, where the newer console layout sits two slots later
// than the older one from CopyTextureRegionX on (docs/d3d12_xs_console_abi.md 5.3: newer->older pairs 85->83,
// 86->84, 87->85, 90->88, 91->89, 92->90, 95->93, 96->94). `console_layout` is the device-wide layout the queue
// proxy detects (kConsoleLayoutOlder or kConsoleLayoutNewer). Null when the layout is not known yet or the slot is
// not a measured tail slot of that layout, so that a slot number is never given the other layout's name. Naming
// only: no slot is forwarded because of it.
inline constexpr unsigned char kConsoleLayoutOlder = 1;
inline constexpr unsigned char kConsoleLayoutNewer = 2;
const char* list_tail_slot_name(unsigned slot, unsigned char console_layout);
// The wrapped allocator (no reference added), or null when `allocator` is not an allocator proxy.
ID3D12CommandAllocator* allocator_proxy_real_allocator(const void* allocator);
unsigned allocator_proxy_slot_count(const void* allocator);
unsigned allocator_proxy_table_size();
// Callback and context recorded by console allocator slot 9 (SetAllocatorCallbackX), or 0 if unset / not a proxy.
std::uintptr_t allocator_proxy_callback(const void* allocator);
std::uintptr_t allocator_proxy_callback_context(const void* allocator);
std::uint64_t allocator_slot_hits(unsigned slot);
bool allocator_slot_first_call_seen(unsigned slot);
std::uint64_t allocator_table_overruns();
// Heap slots 0-2, 7 (GetDevice -> device proxy), 8 (GetDesc console C-ABI) and 9/10 (handle conversion).
bool heap_slot_overridden(unsigned slot);

// Number of live references on a device proxy. Test helper for the heap's GetDevice/lifetime checks.
unsigned long device_proxy_ref_count(const void* device);

// Per-slot call counters and first-call flags for the device and heap proxies. Slot 0 also counts the
// QueryInterface call made while a proxy is created, so tests reset the counters after creation.
// reset_device_proxy_stats also resets the queue and fence counters.
// The hit counters (device, heap, queue, fence and allocator) are for tests, and they are off at the start.
// A test must read a counter (any *_slot_hits function) or call reset_device_proxy_stats BEFORE the calls that it
// counts. Calls made before that are not counted (the counter stays 0); they set only the first-call flags.
std::uint64_t device_slot_hits(unsigned slot);
bool device_slot_first_call_seen(unsigned slot);
std::uint64_t heap_slot_hits(unsigned slot);
bool heap_slot_first_call_seen(unsigned slot);
void reset_device_proxy_stats();

}  // namespace rt::d3d12_xs
