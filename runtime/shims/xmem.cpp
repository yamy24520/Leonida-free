// xmem.dll: the GDK's tracked allocator. Mapped onto the CRT's aligned heap and VirtualQueryEx.
//
// The measured ABI (docs from the clean-room spec) shows four imports:
//   XMemAlloc(size, attributes)
//   XMemFree(ptr, attributes)
//   XMemVirtualQuery(process, address, out64)
//   XMemGetWorkingSetStatistics(kind, out)   (kind 0x20000001 at all seven FSR call sites)
//
// ASSUMPTION: the attribute word is not decoded by the measured call sites (one passes 0x5e50000). Until a call
// site shows the alignment/type fields, every block is 64-byte aligned (covers cache lines and SIMD) and a size of
// zero is rounded up to one byte so the result is a stable, non-null pointer that XMemFree can release.
//
// The default allocators, the physical-page functions and XMemIsToolsMemAvailable follow argument shapes measured
// locally at a title's call sites (each function says which). The page functions are built on the Windows placeholder
// APIs (VirtualAlloc2, MapViewOfFile3), whose flags the title's own XMemVirtualAlloc calls already use.
#include <windows.h>
#include <intrin.h>
#include <malloc.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shims.h"
#include "startup_trace.h"
#include "../src/title_va_alias.h"

namespace {

constexpr size_t kAlign = 64;
constexpr size_t kNoSize = static_cast<size_t>(-1);  // XMemFree has no size field

constinit std::set<std::pair<std::string, uint64_t>>* g_seen_attributes = nullptr;
constinit std::set<std::pair<uint32_t, uint64_t>>* g_seen_transfers = nullptr;

std::string caller_field(const void* caller) {
    return ",\"caller\":" + rt::startup_trace::quote(rt::startup_trace::where(reinterpret_cast<uint64_t>(caller)));
}

// gta6rt --trace FILE: the first XMemAlloc, XMemFree, XMemAllocDefault, XMemFreeDefault and XMemVirtualAlloc call for
// each distinct raw attribute word go to the startup trace as an "xmem_attributes" event (api, the word, the size for an
// allocation, the caller as module+RVA). The bits of the attribute word are not known yet; this records them from a
// real run without logging every allocation, which would fill the file. Off: one relaxed load per call.
void note_attributes(const char* api, uint64_t attributes, size_t size, const void* caller) {
    if (!rt::startup_trace::enabled()) return;
    static std::mutex mutex;
    {
        const std::lock_guard lock(mutex);
        if (!g_seen_attributes) g_seen_attributes = new std::remove_pointer_t<decltype(g_seen_attributes)>;
        if (!g_seen_attributes->insert({api, attributes}).second) return;
    }
    std::string fields = std::string("\"api\":\"") + api +
                         "\",\"attributes\":" + rt::startup_trace::quote(rt::startup_trace::hex(attributes));
    if (size != kNoSize) fields += ",\"size\":" + std::to_string(size);
    fields += caller_field(caller);
    rt::startup_trace::event("xmem_attributes", fields);
}

void* allocate_block(size_t size) { return _aligned_malloc(size ? size : 1, kAlign); }

// Every block and range the title gets here is recorded as the title's memory (src/title_va_alias.h): a shader
// address into it can be given a GPU copy, and an address anywhere else cannot.
void* title_block(size_t size) {
    void* p = allocate_block(size);
    leonida::title_va_aliases().add_title_memory(reinterpret_cast<uintptr_t>(p), size ? size : 1);
    return p;
}

void free_title_block(void* p) {
    if (p) leonida::title_va_aliases().remove_title_memory(reinterpret_cast<uintptr_t>(p));
    _aligned_free(p);
}

void* __stdcall XMemAlloc(size_t size, uint64_t attributes) {
    note_attributes("XMemAlloc", attributes, size, _ReturnAddress());
    return title_block(size);
}

void __stdcall XMemFree(void* p, uint64_t attributes) {
    note_attributes("XMemFree", attributes, kNoSize, _ReturnAddress());
    free_title_block(p);
}

// XMemAllocDefault / XMemFreeDefault: the GDK's own allocator behind XMemAlloc and XMemFree, which a title that hooks
// XMemAlloc forwards to. The measured title reaches each through one wrapper that is a bare tail jump to the import,
// so they take exactly XMemAlloc's and XMemFree's arguments. They share the heap: a block from either allocator may be
// released by either free.
void* __stdcall XMemAllocDefault(size_t size, uint64_t attributes) {
    note_attributes("XMemAllocDefault", attributes, size, _ReturnAddress());
    return title_block(size);
}

void __stdcall XMemFreeDefault(void* p, uint64_t attributes) {
    note_attributes("XMemFreeDefault", attributes, kNoSize, _ReturnAddress());
    free_title_block(p);
}

// BOOL XMemIsToolsMemAvailable(void). The measured title calls it from one helper, which six out-of-memory paths reach
// only after an allocation has failed; the helper breaks into the debugger when the result is zero. Tools memory is
// the extra memory of a development kit. A PC has none, so the answer is FALSE, and the title's own breakpoint marks
// where it ran out of memory.
BOOL WINAPI XMemIsToolsMemAvailable() {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr, "[rt] xmem: XMemIsToolsMemAvailable -> FALSE (no devkit tools memory; logged once)\n");
    }
    return FALSE;
}

// Measured call sites pass a zeroed 64-byte structure. Only the MEMORY_BASIC_INFORMATION-compatible prefix is
// known: callers read at least the base and size fields. Fill that prefix from VirtualQueryEx and zero the rest.
constexpr size_t kVirtualQueryOutSize = 64;
static_assert(sizeof(MEMORY_BASIC_INFORMATION) <= kVirtualQueryOutSize,
              "the 64-byte XMemVirtualQuery output must hold MEMORY_BASIC_INFORMATION");

// Returns the number of bytes written (the VirtualQuery convention) or 0 on failure. The exact return type and the
// meaning of every output field are not fully known; callers in the package only test for success.
SIZE_T __stdcall XMemVirtualQuery(HANDLE process, const void* address, void* out) {
    if (!out) return 0;
    std::memset(out, 0, kVirtualQueryOutSize);
    if (!address) return 0;

    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] xmem: XMemVirtualQuery: fields after MEMORY_BASIC_INFORMATION are unknown; "
                     "the 64-byte output is zero-filled there\n");
    }

    // A null handle means the current process; GetCurrentProcess() is the (HANDLE)-1 pseudo handle.
    HANDLE target = process ? process : GetCurrentProcess();
    MEMORY_BASIC_INFORMATION info{};
    SIZE_T written = VirtualQueryEx(target, address, &info, sizeof(info));
    if (!written) return 0;

    std::memcpy(out, &info, written < kVirtualQueryOutSize ? written : kVirtualQueryOutSize);
    return written;
}

// BOOL XMemGetWorkingSetStatistics(UINT32 kind, XMEM_WORKING_SET_STATISTICS* stats). Measured at all seven call sites
// in amd_fidelityfx_upscaler_xbox.dll (1.0.2.1; calls at RVAs 0x26b3, 0x2ea1, 0x39a9, 0x7452, 0x74d2, 0x7568,
// 0x75b1): ECX is the constant 0x20000001 (its meaning is not known) and RDX points at a stack buffer (0x80 bytes
// where the frame shows it). The caller reads a 64-bit size at +8 and treats a zero return as "no data". Taking the
// pointer from the first argument wrote to address 0x20000001.
// The measured FSR sites establish argument shape, kind and the 64-bit working-set read at +8; +0's title-total memory
// limit interpretation is inferred from other Series implementations (xcl, xbox-emulator) and title engine telemetry
// sizing memory pools (RT-220). Note that if GlobalMemoryStatusEx reports 0 physical memory, XMem falls back to the
// 12 GiB title limit, whereas XSystemGetAppMemoryUsageLimit remains 0.
BOOL __stdcall XMemGetWorkingSetStatistics(uint32_t /*kind*/, void* stats) {
    if (!stats) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr, "[rt] xmem: XMemGetWorkingSetStatistics(nullptr) -> FALSE\n");
        }
        return FALSE;
    }

    PROCESS_MEMORY_COUNTERS pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return FALSE;

    MEMORYSTATUSEX ms{sizeof(ms)};
    uint64_t total_memory = 12ULL * 1024 * 1024 * 1024;
    if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys != 0) {
        total_memory = ms.ullTotalPhys;
    }

    uint8_t* bytes = static_cast<uint8_t*>(stats);
    std::memset(bytes, 0, 16);
    std::memcpy(bytes + 0, &total_memory, sizeof(total_memory));
    uint64_t working_set = pmc.WorkingSetSize;
    std::memcpy(bytes + 8, &working_set, sizeof(working_set));
    return TRUE;
}

// The placeholder APIs (Windows 10 1803 and later) come from kernelbase.dll at run time, so neither gta6rt nor the
// tests link onecore.lib. A host without them fails the calls that need them with ERROR_NOT_SUPPORTED.
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*,
                                        ULONG);
using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

struct PlaceholderApi {
    VirtualAlloc2Fn virtual_alloc2 = nullptr;
    MapViewOfFile3Fn map_view3 = nullptr;
    UnmapViewOfFile2Fn unmap_view2 = nullptr;
    bool available() const { return virtual_alloc2 && map_view3 && unmap_view2; }
};

PlaceholderApi resolve_placeholder_api() {
    PlaceholderApi resolved;
    // kernel32 forwards these, but a host may have kernelbase unloaded at the point the loader reaches the first
    // shim call, so load it rather than assume.
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (!kernelbase) kernelbase = LoadLibraryW(L"kernelbase.dll");
    if (kernelbase) {
        resolved.virtual_alloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernelbase, "VirtualAlloc2"));
        resolved.map_view3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernelbase, "MapViewOfFile3"));
        resolved.unmap_view2 = reinterpret_cast<UnmapViewOfFile2Fn>(GetProcAddress(kernelbase, "UnmapViewOfFile2"));
    }
    return resolved;
}

// Namespace scope: this resolves during static initialisation, on the thread that starts the process. As a
// function-local static it was first evaluated on the title's thread, the initialiser did not run there, every
// member stayed null and available() answered no on a host that has VirtualAlloc2 -- so every console 64 KiB window
// and every 2 MiB/4 MiB reservation took the plain-VirtualAlloc fallback instead of the placeholder path.
const PlaceholderApi g_placeholder_api = resolve_placeholder_api();

const PlaceholderApi& placeholder_api() { return g_placeholder_api; }

// XMem allocation-type page bits select the guest page granularity and must not be forwarded as Win32 flags.
constexpr DWORD kXmemLargePages = 0x20000000;  // 2 MiB pages
constexpr DWORD kXmem64kPages = 0x20400000;    // 64 KiB pages
constexpr DWORD kXmem4MbPages = 0x80000000;    // 4 MiB pages (MEM_4MB_PAGES)
constexpr DWORD kXmemPageModeMask = kXmem64kPages | kXmem4MbPages;
constexpr SIZE_T kXmemPageSize64k = 0x10000;
constexpr SIZE_T kXmemPageSize2m = 0x200000;
constexpr SIZE_T kXmemPageSize4m = 0x400000;
constexpr DWORD kXmemHostAllocationTypeMask = MEM_COMMIT | MEM_RESERVE | MEM_REPLACE_PLACEHOLDER |
                                              MEM_RESERVE_PLACEHOLDER | MEM_RESET | MEM_TOP_DOWN | MEM_WRITE_WATCH |
                                              MEM_RESET_UNDO;

bool round_up_size(SIZE_T value, SIZE_T alignment, SIZE_T& rounded) {
    const SIZE_T max = (std::numeric_limits<SIZE_T>::max)();
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || value > max - (alignment - 1)) return false;
    rounded = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

// The console's PAGE_GRAPHICS_* protection bits (winnt.h: PAGE_GRAPHICS_NOACCESS 0x800 .. PAGE_GRAPHICS_NOCACHE
// 0x40000). The host refuses them with ERROR_INVALID_PARAMETER, and the D3D12 layer reads guest memory through the
// CPU mapping, so only the CPU protection is applied. The retail executable passes 0x801 and 0x22004.
constexpr DWORD kPageGraphicsMask = 0x0007F800;

DWORD host_protect(DWORD p) {
    const DWORD cpu = p & ~kPageGraphicsMask;
    return cpu ? cpu : PAGE_NOACCESS;
}

// Physical pages and the windows they are mapped into. One mutex covers both tables and every map and unmap.
//
// A physical-page window is a reserve-only XMemVirtualAlloc in a guest page mode, or a reserve-only replacement of
// part of a placeholder. On the host it is a placeholder, the only kind of reservation a section view can later
// replace, and its protection is kept here: the map calls take none, so a mapped page gets its window's protection, as
// AWE's MapUserPhysicalPages does.
//
// ASSUMPTION: a title does not commit memory inside such an XMem window with kernel32!VirtualAlloc. A placeholder
// refuses a plain commit; the measured windows are only ever passed to the map calls. TitleVirtualAlloc reservations
// stay ordinary reservations so they support plain host commits.
struct Window {
    SIZE_T size;
    DWORD protect;
};

struct Section {
    HANDLE handle;
    explicit Section(HANDLE h) : handle(h) {}
    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;
    ~Section() { CloseHandle(handle); }
};

struct PhysicalPage {
    std::shared_ptr<Section> section;  // a mapped view holds its own reference to the section
    uint64_t offset;
};

constexpr uint64_t kPhysicalPageSize = 0x10000;
// Page numbers are 4 KiB frame numbers of 64 KiB pages: the measured title stores them packed and assumes each is a
// non-zero multiple of 16 below 2^25, with 0 for an entry it unmaps. So a page number is slot * 16 with slot in
// [1, 2^21), and freed slots are reused so a long run never runs out.
constexpr uint64_t kFramesPerPage = kPhysicalPageSize / 0x1000;
constexpr uint64_t kPageSlotLimit = uint64_t{1} << 21;

std::mutex g_phys_mutex;
std::map<uintptr_t, Window> g_windows;              // base -> window
std::unordered_map<uint64_t, PhysicalPage> g_pages;  // page number -> backing
std::vector<uint64_t> g_free_slots;
uint64_t g_next_slot = 1;
// A mapped run: `count` pages of one section at consecutive offsets, mapped as one view at its base. One view per run
// rather than per page: under Wine each view costs a placeholder split and a section map whose time grows with the
// number of views, and the retail title maps about 100,000 pages in one call.
struct View {
    uint64_t count;
    std::shared_ptr<Section> section;  // kept for remapping part of the run after its pages are freed
    uint64_t offset;                   // section offset of the first page
    DWORD protect;
};
std::map<uintptr_t, View> g_views;  // view base -> view
std::map<uintptr_t, SIZE_T> g_reservations;  // base -> size of each placeholder reservation XMemVirtualAlloc made

// With g_phys_mutex held. Removes [base, base + size) from g_windows, preserving any prefix or suffix of an existing
// window that extends outside the range, and drops stale g_views entries overlapping it. Callers invoke this only for a
// range the host just freshly reserved (previously MEM_FREE, e.g. after kernel32!UnmapViewOfFile / VirtualFree outside
// the shim) or confirmed MEM_RESERVE via isolate_placeholder, so no live mapped view exists in [base, base + size).
void clear_window_range(void* base, SIZE_T size) {
    if (!base || size == 0) return;
    const uintptr_t start = reinterpret_cast<uintptr_t>(base);
    const uintptr_t max_addr = (std::numeric_limits<uintptr_t>::max)();
    const uintptr_t end = size > max_addr - start ? max_addr : start + size;
    auto it = g_windows.lower_bound(start);
    if (it != g_windows.begin()) {
        const auto prev = std::prev(it);
        if (prev->first + prev->second.size > start) it = prev;
    }
    while (it != g_windows.end() && it->first < end) {
        const uintptr_t win_start = it->first;
        const uintptr_t win_end = win_start + it->second.size;
        const DWORD win_protect = it->second.protect;
        it = g_windows.erase(it);
        if (win_start < start) g_windows[win_start] = Window{start - win_start, win_protect};
        if (win_end > end) g_windows[end] = Window{win_end - end, win_protect};
    }
    // A recorded view overlapping a fresh reservation was released outside the shim, so the whole record is stale.
    auto vit = g_views.lower_bound(start);
    if (vit != g_views.begin()) {
        const auto prev = std::prev(vit);
        if (prev->first + prev->second.count * kPhysicalPageSize > start) vit = prev;
    }
    while (vit != g_views.end() && vit->first < end) vit = g_views.erase(vit);
}

// With g_phys_mutex held. Records [base, base + size) as a physical-page window, trimming any overlapping window and
// clearing stale mapped-page records in the replaced range.
void record_window(void* base, SIZE_T size, DWORD protect) {
    clear_window_range(base, size);
    g_windows[reinterpret_cast<uintptr_t>(base)] = Window{size, protect};
}

// With g_phys_mutex held: the window holding [address, address + bytes), if one does.
const Window* find_window(uintptr_t address, uint64_t bytes) {
    auto it = g_windows.upper_bound(address);
    if (it == g_windows.begin()) return nullptr;
    --it;
    const uint64_t offset = address - it->first;
    if (offset >= it->second.size || bytes > it->second.size - offset) return nullptr;
    return &it->second;
}

// With g_phys_mutex held: true when `address` lies inside a recorded reservation that starts before it.
bool inside_reservation(uintptr_t address) {
    auto it = g_reservations.lower_bound(address);
    if (it == g_reservations.begin()) return false;
    --it;
    return address - it->first < it->second;
}

// A new host reservation (`result`, asked for at `address` with `size`) that lands in a hole the title released inside
// a recorded reservation is recorded too, so that releasing the outer reservation leaves it alone (release_range).
void note_nested_allocation(void* result, void* address, SIZE_T size) {
    if (!result) return;
    const uintptr_t start = reinterpret_cast<uintptr_t>(result);
    const uintptr_t asked = address ? reinterpret_cast<uintptr_t>(address) : start;
    const uintptr_t end = (asked + size + 0xfff) & ~uintptr_t{0xfff};
    const std::lock_guard lock(g_phys_mutex);
    if (end > start && inside_reservation(start)) g_reservations[start] = static_cast<SIZE_T>(end - start);
}

// [address, address + size) as one placeholder of its own: split it out of the placeholder that holds it. Fails when
// the range is not inside one placeholder.
bool isolate_placeholder(void* address, SIZE_T size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_RESERVE) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return false;
    }
    if (info.AllocationBase == address && info.RegionSize == size) return true;
    return VirtualFree(address, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != FALSE;
}

PVOID reserve_placeholder(void* address, SIZE_T size, SIZE_T alignment) {
    MEM_ADDRESS_REQUIREMENTS requirements{};
    MEM_EXTENDED_PARAMETER parameter{};
    ULONG parameter_count = 0;
    if (!address && alignment > kXmemPageSize64k) {
        requirements.Alignment = alignment;
        parameter.Type = MemExtendedParameterAddressRequirements;
        parameter.Pointer = &requirements;
        parameter_count = 1;
    }
    return placeholder_api().virtual_alloc2(GetCurrentProcess(), address, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                            PAGE_NOACCESS, parameter_count ? &parameter : nullptr, parameter_count);
}

// A reserve-only page-mode range is a placeholder here, and Windows refuses a commit inside a placeholder. The title
// reserves such a range and commits it later: the measured shapes 0x20402000 (reserve, attributes 0x21) and 0x20401000
// (commit, attributes 0x283) at the same address (docs/xmem_exe_calls.md). So the committed range is split out of its
// placeholder and replaced by a committed allocation at the same address, and the window record loses that part.
//
// shortcut: a commit that spans more than one reserved window is not replaced, upgrade when a call site shows one.
// With g_phys_mutex held.
void* commit_window(void* address, SIZE_T size, DWORD host_type, DWORD protect) {
    if (!isolate_placeholder(address, size)) return nullptr;
    const DWORD type = (host_type | MEM_REPLACE_PLACEHOLDER | MEM_RESERVE) & ~MEM_RESERVE_PLACEHOLDER;
    void* const result =
        placeholder_api().virtual_alloc2(GetCurrentProcess(), address, size, type, protect, nullptr, 0);
    if (!result) return nullptr;

    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = start + size;
    for (auto it = g_windows.begin(); it != g_windows.end();) {
        const uintptr_t window_start = it->first;
        const uintptr_t window_end = window_start + it->second.size;
        if (window_end <= start || window_start >= end) {
            ++it;
            continue;
        }
        const Window window = it->second;
        it = g_windows.erase(it);
        if (window_start < start) g_windows.emplace(window_start, Window{start - window_start, window.protect});
        if (window_end > end) g_windows.emplace(end, Window{window_end - end, window.protect});
    }
    return result;
}

// PVOID XMemVirtualAlloc(address, size, allocationType, attributes, protect). All 23 calls of the measured title pass a
// nonzero attribute word; its bits are not decoded and nothing on the host depends on them, so it is accepted and only
// recorded (xmem_attributes). Its types cover committed 2 MiB and 64 KiB pages, reserve-only 64 KiB windows for physical pages, one
// MEM_RESERVE_PLACEHOLDER reservation and five MEM_REPLACE_PLACEHOLDER windows carved out of it, each then filled with
// XMemMapPhysicalPages. The guest page size sets the alignment and rounding of the region; the host's own pages stay
// 4 KiB (host large pages need SeLockMemoryPrivilege). The 4 MiB mode uses the same path as 2 MiB, with
// 4 MiB granularity. Regions the host places are aligned to the guest page size through VirtualAlloc2.
void* xmem_virtual_alloc(void* address, SIZE_T size, DWORD allocation_type, DWORD protect, bool console_windows) {
    if (size == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    DWORD page_mode = allocation_type & kXmemPageModeMask;
    if (page_mode == (kXmem4MbPages | kXmemLargePages)) page_mode = kXmem4MbPages;
    if (page_mode != 0 && page_mode != kXmem64kPages && page_mode != kXmemLargePages && page_mode != kXmem4MbPages) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    const DWORD unsupported = allocation_type & ~(kXmemHostAllocationTypeMask | kXmemPageModeMask);
    if (unsupported != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    DWORD host_type = allocation_type & kXmemHostAllocationTypeMask;
    if (host_type == 0) host_type = MEM_RESERVE | MEM_COMMIT;
    const DWORD host_prot = host_protect(protect);

    const bool commit = (host_type & MEM_COMMIT) != 0;
    const bool new_placeholder = (host_type & MEM_RESERVE_PLACEHOLDER) != 0;
    const bool replace = (host_type & MEM_REPLACE_PLACEHOLDER) != 0;
    const bool window = console_windows && page_mode != 0 && !commit && !new_placeholder;
    const bool large_pages = page_mode == kXmemLargePages || page_mode == kXmem4MbPages;
    const bool aligned_reservation = !console_windows && page_mode != 0 && !address && (host_type & MEM_RESERVE);
    if ((new_placeholder || replace || window || large_pages || aligned_reservation) && !placeholder_api().available()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    SIZE_T page_granularity = system_info.dwPageSize;
    SIZE_T base_granularity = (host_type & MEM_RESERVE) ? system_info.dwAllocationGranularity : system_info.dwPageSize;
    if (page_mode == kXmem64kPages) page_granularity = base_granularity = kXmemPageSize64k;
    if (page_mode == kXmemLargePages) page_granularity = base_granularity = kXmemPageSize2m;
    if (page_mode == kXmem4MbPages) page_granularity = base_granularity = kXmemPageSize4m;
    if (new_placeholder || replace) {
        // Placeholders are split and replaced on allocation-granularity boundaries only.
        if (page_granularity < system_info.dwAllocationGranularity) page_granularity = system_info.dwAllocationGranularity;
        if (base_granularity < system_info.dwAllocationGranularity) base_granularity = system_info.dwAllocationGranularity;
    }
    if (page_granularity == 0 || (page_granularity & (page_granularity - 1)) != 0 || base_granularity == 0 ||
        (base_granularity & (base_granularity - 1)) != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    if (address != nullptr) {
        const uintptr_t requested = reinterpret_cast<uintptr_t>(address);
        const uintptr_t max = (std::numeric_limits<uintptr_t>::max)();
        if (size > max - requested) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        const uintptr_t requested_end = requested + size;
        const uintptr_t base = requested & ~(static_cast<uintptr_t>(base_granularity) - 1);
        if (base == 0) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return nullptr;
        }
        if (requested_end > max - (static_cast<uintptr_t>(page_granularity) - 1)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        const uintptr_t end = (requested_end + static_cast<uintptr_t>(page_granularity) - 1) &
                              ~(static_cast<uintptr_t>(page_granularity) - 1);
        address = reinterpret_cast<void*>(base);
        size = static_cast<SIZE_T>(end - base);
    } else if (replace) {
        SetLastError(ERROR_INVALID_PARAMETER);  // a replacement names the placeholder it replaces
        return nullptr;
    } else if (page_granularity > system_info.dwPageSize && !round_up_size(size, page_granularity, size)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    if (replace) {
        const std::lock_guard lock(g_phys_mutex);
        if (!isolate_placeholder(address, size)) return nullptr;
        if (window) {
            // A reserve-only replacement stays a placeholder: the physical pages are mapped into it.
            record_window(address, size, host_prot);
            return address;
        }
        void* const result = placeholder_api().virtual_alloc2(GetCurrentProcess(), address, size,
                                                              host_type | MEM_RESERVE, host_prot, nullptr, 0);
        if (result) clear_window_range(result, size);
        return result;
    }
    if (new_placeholder || window) {
        void* const result = reserve_placeholder(address, size, base_granularity);
        if (result) {
            const std::lock_guard lock(g_phys_mutex);
            g_reservations[reinterpret_cast<uintptr_t>(result)] = size;
            if (window) record_window(result, size, host_prot);
            else clear_window_range(result, size);
        }
        return result;
    }
    if (commit && address != nullptr) {
        const std::lock_guard lock(g_phys_mutex);
        if (find_window(reinterpret_cast<uintptr_t>(address), size) != nullptr) {
            if (!placeholder_api().available()) {
                SetLastError(ERROR_NOT_SUPPORTED);
                return nullptr;
            }
            return commit_window(address, size, host_type, host_prot);
        }
    }
    if ((large_pages || aligned_reservation) && address == nullptr) {
        MEM_ADDRESS_REQUIREMENTS requirements{};
        requirements.Alignment = base_granularity;
        MEM_EXTENDED_PARAMETER parameter{};
        parameter.Type = MemExtendedParameterAddressRequirements;
        parameter.Pointer = &requirements;
        // Title reservations retain their host flags, including MEM_TOP_DOWN and MEM_WRITE_WATCH.
        void* const result = placeholder_api().virtual_alloc2(GetCurrentProcess(), nullptr, size,
                                                              console_windows ? host_type | MEM_RESERVE : host_type,
                                                              host_prot, &parameter, 1);
        if (result) {
            const std::lock_guard lock(g_phys_mutex);
            clear_window_range(result, size);
        }
        note_nested_allocation(result, nullptr, size);
        return result;
    }
    void* const result = VirtualAlloc(address, size, host_type, host_prot);
    if (result && (host_type & MEM_RESERVE)) {
        const std::lock_guard lock(g_phys_mutex);
        clear_window_range(result, size);
    }
    if (host_type & MEM_RESERVE) note_nested_allocation(result, address, size);
    return result;
}

void* WINAPI XMemVirtualAlloc(void* address, SIZE_T size, DWORD allocation_type, ULONGLONG attributes, DWORD protect) {
    note_attributes("XMemVirtualAlloc", attributes, size, _ReturnAddress());
    void* result = xmem_virtual_alloc(address, size, allocation_type, protect, true);
    // The title's memory (src/title_va_alias.h). Never forgotten: a released range is no longer committed, which
    // the copy also checks.
    if (result) leonida::title_va_aliases().add_title_memory(reinterpret_cast<uintptr_t>(result), size);
    return result;
}

// With g_phys_mutex held. Maps `view` at the 64 KiB-aligned `address`, which holds no mapped page. MEM_REPLACE_PLACEHOLDER
// needs the view to fill one placeholder exactly, and earlier partial unmaps can leave several host placeholders in one
// run, so the run is mapped as one view per placeholder it spans (one view for a fresh window).
bool map_view(uintptr_t address, const View& view) {
    for (uint64_t mapped = 0; mapped < view.count;) {
        const uintptr_t base = address + mapped * kPhysicalPageSize;
        void* const target = reinterpret_cast<void*>(base);
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(target, &info, sizeof(info)) || info.State != MEM_RESERVE ||
            info.RegionSize < kPhysicalPageSize) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return false;
        }
        const uint64_t available = info.RegionSize / kPhysicalPageSize;
        const uint64_t remaining = view.count - mapped;
        const uint64_t count = available < remaining ? available : remaining;
        const SIZE_T size = static_cast<SIZE_T>(count * kPhysicalPageSize);
        const uint64_t offset = view.offset + mapped * kPhysicalPageSize;
        if (!isolate_placeholder(target, size)) return false;
        if (!placeholder_api().map_view3(view.section->handle, GetCurrentProcess(), target, offset, size,
                                         MEM_REPLACE_PLACEHOLDER, view.protect, nullptr, 0)) {
            return false;
        }
        g_views.emplace(base, View{count, view.section, offset, view.protect});
        mapped += count;
    }
    return true;
}

// With g_phys_mutex held. Unmaps every page in [start, end); the addresses become placeholders again. A view that
// reaches outside the range is unmapped whole, and its pages outside the range are mapped again, so neighbours keep
// their pages.
bool unmap_range(uintptr_t start, uintptr_t end) {
    auto it = g_views.upper_bound(start);
    if (it != g_views.begin()) {
        const auto prev = std::prev(it);
        if (prev->first + prev->second.count * kPhysicalPageSize > start) it = prev;
    }
    while (it != g_views.end() && it->first < end) {
        const uintptr_t base = it->first;
        const View view = it->second;
        const uintptr_t view_end = base + view.count * kPhysicalPageSize;
        if (!placeholder_api().unmap_view2(GetCurrentProcess(), reinterpret_cast<void*>(base),
                                           MEM_PRESERVE_PLACEHOLDER)) {
            return false;
        }
        it = g_views.erase(it);
        if (base < start) {
            View left = view;
            left.count = (start - base) / kPhysicalPageSize;
            if (!map_view(base, left)) return false;
        }
        if (view_end > end) {
            const uint64_t skipped = (end - base) / kPhysicalPageSize;
            View right = view;
            right.count = view.count - skipped;
            right.offset += skipped * kPhysicalPageSize;
            return map_view(end, right);  // the last view the range touches
        }
    }
    return true;
}

bool unmap_page(uintptr_t address) { return unmap_range(address, address + kPhysicalPageSize); }

// With g_phys_mutex held. Maps physical page `page` (0: unmap) at the 64 KiB-aligned `address`, replacing what was
// mapped there. The caller has found the window.
bool map_page(uintptr_t address, uint64_t page, DWORD protect) {
    if (!unmap_page(address)) return false;
    if (page == 0) return true;
    const auto it = g_pages.find(page);
    if (it == g_pages.end()) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    return map_view(address, View{1, it->second.section, it->second.offset, protect});
}

// The window's protection without a cache modifier, which MapViewOfFile3 refuses on Wine.
DWORD view_protect(const Window& window) { return window.protect & ~(PAGE_WRITECOMBINE | PAGE_NOCACHE); }

bool aligned_page_address(uintptr_t address) { return (address & (kPhysicalPageSize - 1)) == 0; }

// The first traced allocation can come from the TLS host's thread. Keep the set lazy without a function-local
// CRT initialization guard on that thread.
std::mutex g_physical_trace_mutex;
constinit std::optional<std::set<uint64_t>> g_seen_physical_attributes;

// gta6rt --trace FILE: the first XMemAllocatePhysicalPages for each distinct attribute word goes to the startup trace
// as an "xmem_physical_pages" event with the page count and the caller.
void note_physical_attributes(uint64_t attributes, uint64_t count, const void* caller) {
    if (!rt::startup_trace::enabled()) return;
    {
        const std::lock_guard lock(g_physical_trace_mutex);
        if (!g_seen_physical_attributes) g_seen_physical_attributes.emplace();
        if (!g_seen_physical_attributes->insert(attributes).second) return;
    }
    rt::startup_trace::event("xmem_physical_pages",
                             "\"api\":\"XMemAllocatePhysicalPages\",\"attributes\":" +
                                 rt::startup_trace::quote(rt::startup_trace::hex(attributes)) +
                                 ",\"pages\":" + std::to_string(count) + caller_field(caller));
}

// BOOL XMemAllocatePhysicalPages(attributes, UINT64* pageCount, UINT64* pageArray). Measured at four call sites: RCX is
// a constant attribute word, RDX points at the number of 64 KiB pages (every caller computes it as the byte size
// rounded up and shifted right by 16), R8 at an array that receives one page number per page. The result is tested as
// a 32-bit BOOL. After success every caller compares the count with the one it asked for and frees the pages if they
// differ, so the count is in/out.
//
// Each call is backed by one pagefile-backed section of count * 64 KiB: committed (charged) at once, given physical
// memory as it is touched, zero-filled. Pages keep their contents while unmapped or mapped elsewhere, and one page can
// be mapped at several addresses. ASSUMPTION: the attribute word (memory type, perhaps the page size) is not decoded;
// every page is 64 KiB, which all four callers' arithmetic agrees with.
BOOL WINAPI XMemAllocatePhysicalPages(uint64_t attributes, uint64_t* page_count, uint64_t* page_array) {
    note_physical_attributes(attributes, page_count ? *page_count : 0, _ReturnAddress());
    if (!page_count || (*page_count && !page_array)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const uint64_t count = *page_count;
    if (count == 0) return TRUE;

    const std::lock_guard lock(g_phys_mutex);
    if (count > g_free_slots.size() + (kPageSlotLimit - g_next_slot)) {
        *page_count = 0;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    const uint64_t bytes = count * kPhysicalPageSize;  // below 2^37: count is below 2^21
    const HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                             static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
    if (!handle) {
        *page_count = 0;
        return FALSE;
    }
    const auto section = std::make_shared<Section>(handle);
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t slot;
        if (!g_free_slots.empty()) {
            slot = g_free_slots.back();
            g_free_slots.pop_back();
        } else {
            slot = g_next_slot++;
        }
        const uint64_t page = slot * kFramesPerPage;
        g_pages[page] = PhysicalPage{section, i * kPhysicalPageSize};
        page_array[i] = page;
    }
    return TRUE;
}

// BOOL XMemFreePhysicalPages(UINT64 pageCount, UINT64* pageArray). Measured at three call sites: RCX is the count (the
// one XMemAllocatePhysicalPages returned, or the sum of several allocations), RDX the array. The callers unmap the
// pages first; a page that is still mapped keeps its memory until it is unmapped. FALSE, with the entries before it
// freed, for a number that is not a live page.
BOOL WINAPI XMemFreePhysicalPages(uint64_t page_count, const uint64_t* page_array) {
    if (page_count && !page_array) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const std::lock_guard lock(g_phys_mutex);
    for (uint64_t i = 0; i < page_count; ++i) {
        const auto it = g_pages.find(page_array[i]);
        if (it == g_pages.end()) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        g_pages.erase(it);
        g_free_slots.push_back(page_array[i] / kFramesPerPage);
    }
    return TRUE;
}

// PVOID XMemMapPhysicalPages(PVOID address, UINT64 pageCount, UINT64* pageArray). Measured at twelve call sites: RCX
// is an address in a window XMemVirtualAlloc reserved, RDX a 64-bit count, R8 the pages to map there one after another,
// or null to unmap that many (four calls at shutdown). Callers test the 64-bit result for zero, so it is the address on
// success. A page number of 0 in the array unmaps that one page.
//
// Consecutive pages of one allocation are mapped as one view (MapViewOfFile3 into a placeholder of their size); a
// page remapped or unmapped later splits its view, and its neighbours stay mapped. The range is unmapped first, so a
// page number that is not a live page fails with the pages before it mapped and the rest of the range unmapped.
void* WINAPI XMemMapPhysicalPages(void* address, uint64_t page_count, const uint64_t* page_array) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    if (!aligned_page_address(start) || page_count > (std::numeric_limits<uintptr_t>::max)() / kPhysicalPageSize ||
        start > (std::numeric_limits<uintptr_t>::max)() - page_count * kPhysicalPageSize) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (!placeholder_api().available()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    const std::lock_guard lock(g_phys_mutex);
    const uintptr_t end = start + page_count * kPhysicalPageSize;
    if (!page_array) return unmap_range(start, end) ? address : nullptr;
    const Window* const window = find_window(start, page_count * kPhysicalPageSize);
    if (!window) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return nullptr;
    }
    const DWORD protect = view_protect(*window);
    if (!unmap_range(start, end)) return nullptr;
    for (uint64_t i = 0; i < page_count;) {
        if (page_array[i] == 0) {
            ++i;
            continue;
        }
        const auto it = g_pages.find(page_array[i]);
        if (it == g_pages.end()) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        View view{1, it->second.section, it->second.offset, protect};
        while (i + view.count < page_count) {
            const auto next = g_pages.find(page_array[i + view.count]);
            if (next == g_pages.end() || next->second.section != view.section ||
                next->second.offset != view.offset + view.count * kPhysicalPageSize) {
                break;
            }
            ++view.count;
        }
        if (!map_view(start + i * kPhysicalPageSize, view)) return nullptr;
        i += view.count;
    }
    return address;
}

// BOOL XMemMapPhysicalPagesScatter(PVOID* addresses, UINT32 pageCount, UINT64* pageArray). Measured at two call sites:
// RCX is an array of addresses, EDX a 32-bit count, R8 the page for each address (an entry of 0, or a null array,
// unmaps). The result is tested as a 32-bit BOOL. Like MapUserPhysicalPagesScatter: entry i maps pageArray[i] at
// addresses[i].
BOOL WINAPI XMemMapPhysicalPagesScatter(void* const* addresses, uint32_t page_count, const uint64_t* page_array) {
    if (page_count && !addresses) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!placeholder_api().available()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    const std::lock_guard lock(g_phys_mutex);
    for (uint32_t i = 0; i < page_count; ++i) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(addresses[i]);
        if (!aligned_page_address(address)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        const uint64_t page = page_array ? page_array[i] : 0;
        if (page == 0) {
            if (!unmap_page(address)) return FALSE;
            continue;
        }
        const Window* const window = find_window(address, kPhysicalPageSize);
        if (!window) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
        if (!map_page(address, page, view_protect(*window))) return FALSE;
    }
    return TRUE;
}

UINT WINAPI XMemGetWriteWatch(DWORD flags, void* base_address, SIZE_T region_size, void** addresses, ULONG_PTR* count,
                              ULONG* granularity) {
    return GetWriteWatch(flags, base_address, region_size, addresses, count, granularity);
}

UINT WINAPI XMemResetWriteWatch(void* base_address, SIZE_T region_size) {
    return ResetWriteWatch(base_address, region_size);
}

// BOOL XMemTransferMemory(XMEM_TRANSFER_TYPE type, SIZE_T size). Its reference page and the XMEM_TRANSFER_TYPE enum
// page (gaming/gdk/docs/reference/system/xmem/{functions/xmemtransfermemory,enums/xmem_transfer_type}) are NDA
// topics on Microsoft Learn, so the shape comes from public code built against the GDK's xmem.h: Microsoft's
// XBOX-MonoGame-NetRumble sample (console/NetRumble.Bootstrap/Main.cpp) and FNA-XNA/NativeAOT-Xbox (Bootstrap/Main.cpp)
// both call `if (!XMemTransferMemory(XMemTransferTitleToSystem, additionalKernelPoolMemory))` with a `const size_t`,
// commented "Transfer additional memory to kernel pool" and "Must be multiples of 2MB sizes". So: two arguments, the
// enum (32 bits) in ECX and the size in RDX, and a zero return is failure. The public XLaunchNewGame page adds that
// "Memory transferred to the system using XMemTransferMemory (NDA topic) is persisted", and the game-save debugging
// page suggests it for E_OUTOFMEMORY "to transfer some memory to the system partition".
//
// On the console it moves budget between the title's partition and the system's. A PC has one pool and no budget
// split, so there is nothing to move: every direction and size returns TRUE and nothing is allocated or reserved. Not
// known (the pages are NDA): the numeric XMEM_TRANSFER_TYPE values, and whether the console refuses or rounds a size
// that is not a multiple of 2 MiB. Refusing would stop a title at startup (both samples return -1), while succeeding
// changes nothing here. TRUE is 1 in EAX, so it also reads as true if the real return type is a one-byte bool.
//
// gta6rt --trace FILE: the first call for each distinct (type, size) pair goes to the startup trace as an
// "xmem_transfer" event with the caller as module+RVA, so a run with the executable records the values it passes.
BOOL WINAPI XMemTransferMemory(uint32_t type, SIZE_T size) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] xmem: XMemTransferMemory(type %u, %zu bytes): a PC has no title/system memory partition; "
                     "returning TRUE without moving anything (logged once)\n",
                     type, static_cast<size_t>(size));
    }
    if (rt::startup_trace::enabled()) {
        static std::mutex mutex;
        bool first = false;
        {
            const std::lock_guard lock(mutex);
            if (!g_seen_transfers) g_seen_transfers = new std::remove_pointer_t<decltype(g_seen_transfers)>;
            first = g_seen_transfers->insert({type, static_cast<uint64_t>(size)}).second;
        }
        if (first) {
            rt::startup_trace::event(
                "xmem_transfer",
                "\"api\":\"XMemTransferMemory\",\"type\":" + std::to_string(type) +
                    ",\"size\":" + std::to_string(static_cast<uint64_t>(size)) + ",\"caller\":" +
                    rt::startup_trace::quote(
                        rt::startup_trace::where(reinterpret_cast<uint64_t>(_ReturnAddress()))));
        }
    }
    return TRUE;
}

// kernel32!VirtualAlloc as a title calls it. The console's allocation type has two bits of its own, MEM_GRAPHICS
// (0x10000000, memory the GPU reads) and MEM_TITLE (0x40000000, the title's own heap). On a PC the host rejects both
// with ERROR_INVALID_PARAMETER (measured on Windows 11 26200: VirtualAlloc(NULL, 64 KiB, MEM_COMMIT | MEM_RESERVE |
// 0x10000000, PAGE_READWRITE) returns NULL; so does 0x40000000), so an engine allocating its heaps this way gets
// NULL at boot. Both are hints about where the memory lives; the host has no such split, so they are dropped and
// page-size bits use ordinary aligned host allocations instead of privileged host large pages or console windows.
// An operation is still required, as on Win32. The PAGE_GRAPHICS_* protection bits are dropped (host_protect).
// Not done: whether graphics memory has to come from an arena the D3D12 layer can see (#313).
constexpr DWORD kMemGraphics = 0x10000000;
constexpr DWORD kMemTitle = 0x40000000;

LPVOID title_virtual_alloc(LPVOID address, SIZE_T size, DWORD allocation_type, DWORD protect);

LPVOID WINAPI TitleVirtualAlloc(LPVOID address, SIZE_T size, DWORD allocation_type, DWORD protect) {
    LPVOID result = title_virtual_alloc(address, size, allocation_type, protect);
    if (result) leonida::title_va_aliases().add_title_memory(reinterpret_cast<uintptr_t>(result), size);
    return result;
}

LPVOID title_virtual_alloc(LPVOID address, SIZE_T size, DWORD allocation_type, DWORD protect) {
    allocation_type &= ~(kMemGraphics | kMemTitle);
    if (allocation_type & (MEM_RESET | MEM_RESET_UNDO)) {
        return VirtualAlloc(address, size, allocation_type & ~kXmemPageModeMask, host_protect(protect));
    }
    if (!address && (allocation_type & MEM_COMMIT) && !(allocation_type & MEM_RESERVE)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (allocation_type & (kXmemLargePages | kXmem4MbPages)) {
        // VirtualAlloc refuses the VirtualAlloc2-only placeholder flags; keep that with page-size bits too.
        if (!(allocation_type & (MEM_RESERVE | MEM_COMMIT)) ||
            (allocation_type & (MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER))) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        return xmem_virtual_alloc(address, size, allocation_type, protect, false);
    }
    void* const result = VirtualAlloc(address, size, allocation_type, host_protect(protect));
    if (allocation_type & MEM_RESERVE) note_nested_allocation(result, address, size);
    return result;
}

// With g_phys_mutex held: where the host allocation that starts at `owner` ends.
uintptr_t allocation_end(uintptr_t owner) {
    uintptr_t a = owner;
    for (;;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &info, sizeof(info)) || info.State == MEM_FREE ||
            reinterpret_cast<uintptr_t>(info.AllocationBase) != owner) {
            return a;
        }
        a = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
}

// With g_phys_mutex held. Unmaps every physical page mapped in [base, base + size), releases every host allocation in
// it (the placeholders the map calls split it into, and any committed replacement), and forgets the windows and the
// reservation. A reservation recorded inside this one was made later, in a hole the title had released, and is left
// alone with everything in it; so is an allocation that does not end inside the range. Keeps going past a failure,
// so as much as possible is released.
bool release_range(uintptr_t base, SIZE_T size) {
    const uintptr_t end = base + size;
    std::vector<std::pair<uintptr_t, uintptr_t>> nested;
    for (auto it = g_reservations.upper_bound(base); it != g_reservations.end() && it->first < end; ++it) {
        nested.emplace_back(it->first, it->first + it->second);
    }
    const auto is_nested = [&nested](uintptr_t address) {
        for (const auto& [first, last] : nested) {
            if (address >= first && address < last) return true;
        }
        return false;
    };
    bool ok = true;
    std::vector<std::pair<uintptr_t, uintptr_t>> mapped;
    for (const auto& [view_base, view] : g_views) {
        if (view_base >= base && view_base < end && !is_nested(view_base))
            mapped.emplace_back(view_base, view_base + view.count * kPhysicalPageSize);
    }
    for (const auto& [first, last] : mapped) ok &= unmap_range(first, last);
    for (uintptr_t a = base; a < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &info, sizeof(info))) return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        const uintptr_t owner = reinterpret_cast<uintptr_t>(info.AllocationBase);
        if (info.State != MEM_FREE && !is_nested(owner)) {
            if (owner < base || allocation_end(owner) > end || !VirtualFree(info.AllocationBase, 0, MEM_RELEASE)) {
                ok = false;
            }
        }
        a = next > a ? next : a + kPhysicalPageSize;
    }
    for (auto it = g_windows.lower_bound(base); it != g_windows.end() && it->first < end;) {
        it = is_nested(it->first) ? std::next(it) : g_windows.erase(it);
    }
    g_reservations.erase(base);
    return ok;
}

// kernel32!VirtualFree as a title calls it. The retail executable releases its physical-page reservations this way.
// Once pages have been mapped, such a reservation is one placeholder per 64 KiB page plus the views in it, and the
// host's VirtualFree(base, 0, MEM_RELEASE) would release only the first of them. A MEM_RELEASE at the base of a
// reservation or window XMemVirtualAlloc made therefore releases all of it here; every other call goes to the host.
BOOL WINAPI TitleVirtualFree(LPVOID address, SIZE_T size, DWORD free_type) {
    if (free_type == MEM_RELEASE) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(address);
        const std::lock_guard lock(g_phys_mutex);
        SIZE_T recorded = 0;
        if (const auto it = g_reservations.find(base); it != g_reservations.end()) {
            recorded = it->second;
        } else if (const auto window = g_windows.find(base); window != g_windows.end()) {
            recorded = window->second.size;
        }
        if (recorded != 0 && (size == 0 || size == recorded)) {
            if (release_range(base, recorded)) return TRUE;
            SetLastError(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
    }
    return VirtualFree(address, size, free_type);
}

}  // namespace

#define TITLE_MEMORY(dll) {dll, "VirtualAlloc", reinterpret_cast<void*>(&TitleVirtualAlloc)}, \
                                 {dll, "VirtualFree", reinterpret_cast<void*>(&TitleVirtualFree)}

// The classic libraries and the memory api sets a title's imports name.
SHIM_TABLE(title_memory,
    TITLE_MEMORY("kernel32.dll"),
    TITLE_MEMORY("kernelbase.dll"),
    TITLE_MEMORY("api-ms-win-core-memory-l1-1-0.dll"),
    TITLE_MEMORY("api-ms-win-core-memory-l1-1-1.dll"),
    TITLE_MEMORY("api-ms-win-core-memory-l1-1-2.dll"),
    TITLE_MEMORY("api-ms-win-core-memory-l1-1-3.dll"),
    TITLE_MEMORY("api-ms-win-core-memory-l1-1-4.dll"),
    TITLE_MEMORY("api-ms-win-core-memory-l1-1-5.dll"),
    TITLE_MEMORY("api-ms-win-core-memory-l1-1-6.dll"),
);

SHIM_TABLE(xmem,
    {"xmem.dll", "XMemAlloc", reinterpret_cast<void*>(&XMemAlloc)},
    {"xmem.dll", "XMemFree", reinterpret_cast<void*>(&XMemFree)},
    {"xmem.dll", "XMemVirtualQuery", reinterpret_cast<void*>(&XMemVirtualQuery)},
    {"xmem.dll", "XMemGetWorkingSetStatistics", reinterpret_cast<void*>(&XMemGetWorkingSetStatistics)},
    {"xmem.dll", "XMemVirtualAlloc", reinterpret_cast<void*>(&XMemVirtualAlloc)},
    {"xmem.dll", "XMemGetWriteWatch", reinterpret_cast<void*>(&XMemGetWriteWatch)},
    {"xmem.dll", "XMemResetWriteWatch", reinterpret_cast<void*>(&XMemResetWriteWatch)},
    {"xmem.dll", "XMemTransferMemory", reinterpret_cast<void*>(&XMemTransferMemory)},
    {"xmem.dll", "XMemAllocDefault", reinterpret_cast<void*>(&XMemAllocDefault)},
    {"xmem.dll", "XMemFreeDefault", reinterpret_cast<void*>(&XMemFreeDefault)},
    {"xmem.dll", "XMemIsToolsMemAvailable", reinterpret_cast<void*>(&XMemIsToolsMemAvailable)},
    {"xmem.dll", "XMemAllocatePhysicalPages", reinterpret_cast<void*>(&XMemAllocatePhysicalPages)},
    {"xmem.dll", "XMemFreePhysicalPages", reinterpret_cast<void*>(&XMemFreePhysicalPages)},
    {"xmem.dll", "XMemMapPhysicalPages", reinterpret_cast<void*>(&XMemMapPhysicalPages)},
    {"xmem.dll", "XMemMapPhysicalPagesScatter", reinterpret_cast<void*>(&XMemMapPhysicalPagesScatter)},
);
