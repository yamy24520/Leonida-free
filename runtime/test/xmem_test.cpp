// Checks the registered xmem.dll shim against the measured Xbox ABI.
//
// Every function is resolved through rt::shims::find, so the test exercises the same shim table and calling
// convention the runtime installs for xmem.dll imports instead of a private copy of the implementation. The checks
// cover what the measured call sites use: 64-byte-aligned allocations, XMemFree, working-set statistics with the
// size at +8, and XMemVirtualQuery's MEMORY_BASIC_INFORMATION prefix inside a zeroed 64-byte output.
// XMemTransferMemory follows the call shape in Microsoft's public samples: TRUE for any direction and size, with
// nothing committed, and one trace event per distinct (type, size). XMemVirtualAlloc accepts attribute words and console
// graphics protection bits and commits 2 MiB and 4 MiB guest pages; the placeholder flags pass through; physical pages keep their
// contents across unmap and remap, alias when mapped twice, and scatter-map per address.
#include <windows.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>

#include "shims.h"
#include "startup_trace.h"
#include "host_wine.h"

#ifdef _MSC_VER
extern "C" __declspec(thread) int _Init_thread_epoch;
#endif

namespace {

using XMemAllocFn = void*(__stdcall*)(size_t, uint64_t);
using XMemFreeFn = void(__stdcall*)(void*, uint64_t);
using XMemVirtualQueryFn = SIZE_T(__stdcall*)(HANDLE, const void*, void*);
// BOOL XMemGetWorkingSetStatistics(UINT32 kind, stats*): the FSR DLL passes kind 0x20000001 at every call site.
using XMemGetWorkingSetStatisticsFn = BOOL(__stdcall*)(uint32_t, void*);
using XMemVirtualAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, ULONGLONG, DWORD);
using XMemGetWriteWatchFn = UINT(WINAPI*)(DWORD, void*, SIZE_T, void**, ULONG_PTR*, ULONG*);
using XMemResetWriteWatchFn = UINT(WINAPI*)(void*, SIZE_T);
// BOOL XMemTransferMemory(XMEM_TRANSFER_TYPE, SIZE_T): the shape Microsoft's public NetRumble and NativeAOT-Xbox
// samples call (see runtime/shims/xmem.cpp).
using XMemTransferMemoryFn = BOOL(WINAPI*)(uint32_t, SIZE_T);
// The shapes measured at a title's call sites (see runtime/shims/xmem.cpp).
using XMemIsToolsMemAvailableFn = BOOL(WINAPI*)();
using XMemAllocatePhysicalPagesFn = BOOL(WINAPI*)(uint64_t, uint64_t*, uint64_t*);
using XMemFreePhysicalPagesFn = BOOL(WINAPI*)(uint64_t, const uint64_t*);
using XMemMapPhysicalPagesFn = void*(WINAPI*)(void*, uint64_t, const uint64_t*);
using XMemMapPhysicalPagesScatterFn = BOOL(WINAPI*)(void* const*, uint32_t, const uint64_t*);
constexpr uint32_t kMeasuredKind = 0x20000001;
constexpr DWORD kXmemLargePages = 0x20000000;
constexpr DWORD kXmem64kPages = 0x20400000;
constexpr DWORD kXmem4MbPages = 0x80000000;
constexpr SIZE_T kPage64k = 0x10000;
constexpr SIZE_T kPage2m = 0x200000;
constexpr SIZE_T kPage4m = 0x400000;

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

bool aligned_64(const void* p) { return p && (reinterpret_cast<uintptr_t>(p) & 63u) == 0; }

void test_alloc(XMemAllocFn alloc, XMemFreeFn free_mem) {
    void* const zero = alloc(0, 0);
    check(zero != nullptr, "XMemAlloc(0, 0) returns non-null");
    check(aligned_64(zero), "XMemAlloc(0, 0) returns a 64-byte aligned block");

    bool zero_readable = false;
    if (zero) {
        auto* bytes = static_cast<uint8_t*>(zero);
        bytes[0] = 0xa5;
        zero_readable = bytes[0] == 0xa5;
    }
    check(zero_readable, "XMemAlloc(0, 0) block is writable and readable");

    void* const big = alloc(4096, 0x5e50000);
    check(big != nullptr, "XMemAlloc(4096, 0x5e50000) returns non-null");
    check(aligned_64(big), "XMemAlloc(4096, 0x5e50000) returns a 64-byte aligned block");

    bool pattern_ok = big != nullptr;
    if (big) {
        auto* bytes = static_cast<uint8_t*>(big);
        for (size_t i = 0; i < 4096; ++i) bytes[i] = static_cast<uint8_t>((i * 37u + 11u) & 0xffu);
        for (size_t i = 0; i < 4096; ++i) {
            if (bytes[i] != static_cast<uint8_t>((i * 37u + 11u) & 0xffu)) {
                pattern_ok = false;
                break;
            }
        }
    }
    check(pattern_ok, "XMemAlloc(4096, 0x5e50000) block holds a write/read pattern");

    free_mem(zero, 0);
    free_mem(big, 0x5e50000);
    check(true, "XMemFree accepts both blocks (no crash)");
}

void test_working_set(XMemGetWorkingSetStatisticsFn get_stats) {
    alignas(64) uint8_t stats[64];
    std::memset(stats, 0xa5, sizeof(stats));
    // The pointer is the second argument: the first (0x20000001 here, as the FSR DLL passes it) must never be written
    // through, which is what the one-argument form did.
    const BOOL ok = get_stats(kMeasuredKind, stats);
    check(ok != FALSE, "XMemGetWorkingSetStatistics(kind, 64-byte out) returns TRUE");

    uint64_t total_memory = 0;
    std::memcpy(&total_memory, stats + 0, sizeof(total_memory));
    check(ok != FALSE && total_memory != 0, "XMemGetWorkingSetStatistics writes a non-zero total memory at +0");

    MEMORYSTATUSEX ms{sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys != 0) {
        check(ok != FALSE && total_memory == ms.ullTotalPhys,
              "XMemGetWorkingSetStatistics total memory (+0) matches GlobalMemoryStatusEx ullTotalPhys");
    } else {
        check(ok != FALSE && total_memory == 12ULL * 1024 * 1024 * 1024,
              "XMemGetWorkingSetStatistics total memory (+0) falls back to 12 GiB title limit");
    }

    uint64_t working_set = 0;
    std::memcpy(&working_set, stats + 8, sizeof(working_set));
    check(ok != FALSE && working_set != 0, "XMemGetWorkingSetStatistics writes a non-zero size at +8");

    check(ok != FALSE && total_memory >= working_set,
          "XMemGetWorkingSetStatistics total memory (+0) >= working set (+8)");

    bool tail_untouched = true;
    for (size_t i = 16; i < sizeof(stats); ++i) {
        if (stats[i] != 0xa5) {
            tail_untouched = false;
            break;
        }
    }
    check(ok != FALSE && tail_untouched, "XMemGetWorkingSetStatistics leaves bytes 16..63 untouched");

    check(get_stats(kMeasuredKind, nullptr) == FALSE,
          "XMemGetWorkingSetStatistics(kind, nullptr) returns FALSE without crashing");
}

void test_virtual_query(XMemVirtualQueryFn query) {
    uint8_t local = 0;
    const void* const local_address = &local;

    alignas(64) uint8_t out[64];
    std::memset(out, 0xa5, sizeof(out));
    const SIZE_T written = query(GetCurrentProcess(), local_address, out);
    check(written != 0, "XMemVirtualQuery(current process, stack local) returns non-zero");

    MEMORY_BASIC_INFORMATION info{};
    std::memcpy(&info, out, sizeof(info));
    const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
    const uintptr_t address = reinterpret_cast<uintptr_t>(local_address);
    const bool contains_local =
        base != 0 && info.RegionSize != 0 && address >= base && address - base < info.RegionSize;
    check(written != 0 && contains_local,
          "XMemVirtualQuery fills MEMORY_BASIC_INFORMATION with a region containing the local");

    bool tail_zero = true;
    for (size_t i = sizeof(info); i < sizeof(out); ++i) {
        if (out[i] != 0) {
            tail_zero = false;
            break;
        }
    }
    check(written != 0 && tail_zero, "XMemVirtualQuery leaves the 64-byte output tail zero");

    alignas(64) uint8_t bad_out[64];
    std::memset(bad_out, 0xa5, sizeof(bad_out));
    // Address 1 is queryable (it falls in the free region starting at 0), so use a non-canonical address that
    // VirtualQueryEx rejects.
    const void* const unmappable = reinterpret_cast<const void*>(~static_cast<uintptr_t>(0));
    const SIZE_T bad = query(GetCurrentProcess(), unmappable, bad_out);
    check(bad == 0, "XMemVirtualQuery on an invalid address returns 0 without crashing");
}

void test_virtual_alloc(XMemVirtualAllocFn virtual_alloc, XMemGetWriteWatchFn get_write_watch,
                       XMemResetWriteWatchFn reset_write_watch) {
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const SIZE_T page_size = system_info.dwPageSize;
    check(page_size != 0, "GetSystemInfo reports a non-zero page size");
    if (page_size == 0) return;

    const SIZE_T region_size = page_size * 3;
    void* const base = virtual_alloc(nullptr, region_size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, 0,
                                     PAGE_READWRITE);
    check(base != nullptr, "XMemVirtualAlloc creates a synthetic write-watch region");
    if (!base) return;

    void* watched[4]{};
    ULONG_PTR count = 4;
    ULONG granularity = 0;
    UINT status = get_write_watch(0, base, region_size, watched, &count, &granularity);
    check(status == NO_ERROR && count == 0, "new write-watch region starts with no dirty pages");
    check(status == NO_ERROR && granularity == page_size, "write-watch reports the host page granularity");

    auto* const bytes = static_cast<volatile uint8_t*>(base);
    bytes[0] = 0xa1;
    bytes[page_size * 2] = 0xb2;

    count = 4;
    status = get_write_watch(0, base, region_size, watched, &count, &granularity);
    bool saw_first_page = false;
    bool saw_third_page = false;
    bool all_addresses_are_page_aligned = count <= 4;
    for (ULONG_PTR i = 0; i < count && i < 4; ++i) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(watched[i]);
        const uintptr_t start = reinterpret_cast<uintptr_t>(base);
        all_addresses_are_page_aligned &= address >= start && (address - start) % page_size == 0;
        saw_first_page |= address == start;
        saw_third_page |= address == start + page_size * 2;
    }
    check(status == NO_ERROR && count == 2 && saw_first_page && saw_third_page,
          "write-watch reports each written page by its relative address");
    check(all_addresses_are_page_aligned, "write-watch addresses are page aligned and within the region");

    count = 4;
    status = get_write_watch(0, base, region_size, watched, &count, &granularity);
    check(status == NO_ERROR && count == 2, "query-only write-watch leaves dirty pages recorded");

    status = reset_write_watch(base, region_size);
    check(status == NO_ERROR, "XMemResetWriteWatch clears the synthetic region");
    count = 4;
    status = get_write_watch(0, base, region_size, watched, &count, &granularity);
    check(status == NO_ERROR && count == 0, "reset write-watch query returns no dirty pages");

    bytes[page_size] = 0xc3;
    count = 4;
    status = get_write_watch(0, base, region_size, watched, &count, &granularity);
    check(status == NO_ERROR && count == 1 &&
              reinterpret_cast<uintptr_t>(watched[0]) == reinterpret_cast<uintptr_t>(base) + page_size,
          "write-watch records writes again after reset");

    check(VirtualFree(base, 0, MEM_RELEASE), "release the synthetic write-watch region");

    // 2 MiB guest pages: the region is 2 MiB-aligned, rounded up to whole 2 MiB pages and committed.
    void* const large_page = virtual_alloc(nullptr, kPage2m + page_size, MEM_RESERVE | MEM_COMMIT | kXmemLargePages,
                                           0x183, PAGE_READWRITE | PAGE_GRAPHICS_READWRITE | PAGE_GRAPHICS_COHERENT);
    check(large_page != nullptr, "XMemVirtualAlloc commits 2 MiB guest pages");
    if (large_page) {
        check((reinterpret_cast<uintptr_t>(large_page) & (kPage2m - 1)) == 0, "a 2 MiB-page region is 2 MiB-aligned");
        MEMORY_BASIC_INFORMATION info{};
        VirtualQuery(large_page, &info, sizeof(info));
        check(info.State == MEM_COMMIT && info.RegionSize >= 2 * kPage2m && info.Protect == PAGE_READWRITE,
              "a 2 MiB-page region is rounded up to whole pages, committed read-write without the graphics bits");
        static_cast<volatile uint8_t*>(large_page)[2 * kPage2m - 1] = 0x77;
        check(VirtualFree(large_page, 0, MEM_RELEASE), "release the 2 MiB-page region");
    }

    // 4 MiB uses the existing large-page path, including reserve-only placeholder windows.
    for (const DWORD bits : {kXmem4MbPages, kXmem4MbPages | kXmemLargePages}) {
        void* const region = virtual_alloc(nullptr, kPage4m + 1, MEM_RESERVE | MEM_COMMIT | bits, 0, PAGE_READWRITE);
        check(region != nullptr, "XMemVirtualAlloc commits 4 MiB guest pages");
        if (region) {
            MEMORY_BASIC_INFORMATION info{};
            check((reinterpret_cast<uintptr_t>(region) & (kPage4m - 1)) == 0,
                  "a 4 MiB-page region is 4 MiB-aligned");
            check(VirtualQuery(region, &info, sizeof(info)) && info.State == MEM_COMMIT &&
                      info.RegionSize == 2 * kPage4m && info.Protect == PAGE_READWRITE,
                  "a 4 MiB-page region is rounded up to two writable pages");
            auto* const pages4m = static_cast<volatile uint8_t*>(region);
            pages4m[0] = 0x41;
            pages4m[2 * kPage4m - 1] = 0x42;
            check(pages4m[0] == 0x41 && pages4m[2 * kPage4m - 1] == 0x42, "the 4 MiB pages are writable");
            check(VirtualFree(region, 0, MEM_RELEASE), "release the 4 MiB-page region");
        }

        void* const window = virtual_alloc(nullptr, 1, MEM_RESERVE | bits, 0, PAGE_READWRITE);
        check(window != nullptr, "XMemVirtualAlloc reserves a 4 MiB-page window");
        if (window) {
            MEMORY_BASIC_INFORMATION info{};
            check((reinterpret_cast<uintptr_t>(window) & (kPage4m - 1)) == 0 &&
                      VirtualQuery(window, &info, sizeof(info)) && info.State == MEM_RESERVE &&
                      info.RegionSize == kPage4m,
                  "a reserve-only 4 MiB window is aligned and rounded to one page");
            void* const committed = virtual_alloc(static_cast<uint8_t*>(window) + 1, 1,
                MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | bits, 0, PAGE_READWRITE);
            check(committed == window, "a commit replacement rounds to the 4 MiB window boundaries");
            if (committed) {
                static_cast<volatile uint8_t*>(committed)[kPage4m - 1] = 0x43;
                check(VirtualQuery(committed, &info, sizeof(info)) && info.State == MEM_COMMIT &&
                          info.RegionSize == kPage4m,
                      "the whole 4 MiB placeholder is committed");
            }
            check(VirtualFree(window, 0, MEM_RELEASE), "release the 4 MiB window");
        }

        auto* const placeholder = static_cast<uint8_t*>(virtual_alloc(
            nullptr, 2 * kPage4m, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER | bits, 0, PAGE_NOACCESS));
        check(placeholder != nullptr, "reserve an explicit two-page 4 MiB placeholder");
        if (placeholder) {
            auto* const part = static_cast<uint8_t*>(virtual_alloc(
                placeholder + kPage4m + 1, 1, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | bits,
                0, PAGE_READWRITE));
            check(part == placeholder + kPage4m, "replace a subrange of the two-page 4 MiB placeholder");
            MEMORY_BASIC_INFORMATION info{};
            check(VirtualQuery(placeholder, &info, sizeof(info)) && info.State == MEM_RESERVE &&
                      info.RegionSize == kPage4m,
                  "the first 4 MiB page remains reserved after splitting");
            if (part) {
                check(VirtualQuery(part, &info, sizeof(info)) && info.State == MEM_COMMIT &&
                          info.RegionSize == kPage4m,
                      "the replacement commits exactly the second 4 MiB page");
                static_cast<volatile uint8_t*>(part)[kPage4m - 1] = 0x44;
                check(static_cast<volatile uint8_t*>(part)[kPage4m - 1] == 0x44,
                      "the split 4 MiB replacement is writable");
                check(VirtualFree(part, 0, MEM_RELEASE), "release the split 4 MiB replacement");
            }
            check(VirtualFree(placeholder, 0, MEM_RELEASE), "release the remaining 4 MiB placeholder");
        }

        SetLastError(ERROR_SUCCESS);
        void* const low = virtual_alloc(reinterpret_cast<void*>(kPage4m - 1), 1,
                                        MEM_RESERVE | MEM_COMMIT | bits, 0, PAGE_READWRITE);
        check(low == nullptr && GetLastError() == ERROR_INVALID_ADDRESS,
              "XMem rejects an address that rounds down to zero");
        if (low) VirtualFree(low, 0, MEM_RELEASE);
    }

    for (const DWORD bits : {kXmem4MbPages | kXmem64kPages, kXmem4MbPages | DWORD{0x00400000}}) {
        SetLastError(ERROR_SUCCESS);
        void* const invalid = virtual_alloc(nullptr, kPage4m, MEM_RESERVE | MEM_COMMIT | bits, 0, PAGE_READWRITE);
        check(invalid == nullptr && GetLastError() == ERROR_INVALID_PARAMETER,
              "4 MiB bits combined with the physical-page bit remain invalid");
        if (invalid) VirtualFree(invalid, 0, MEM_RELEASE);
    }

    // The attribute word is accepted (every measured call passes a nonzero one) and the protection loses its graphics
    // bits, in the 64 KiB page mode as well.
    void* const attributed = virtual_alloc(nullptr, kPage64k, MEM_RESERVE | MEM_COMMIT | kXmem64kPages, 0x181,
                                           PAGE_READWRITE | PAGE_GRAPHICS_READWRITE);
    check(attributed != nullptr, "XMemVirtualAlloc accepts a nonzero attribute word and console protection bits");
    if (attributed) {
        MEMORY_BASIC_INFORMATION info{};
        VirtualQuery(attributed, &info, sizeof(info));
        check(info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE,
              "the region is committed PAGE_READWRITE (graphics bits dropped)");
        static_cast<volatile uint8_t*>(attributed)[0] = 0x42;
        check(VirtualFree(attributed, 0, MEM_RELEASE), "release the attributed region");
    }

    // The retail executable's GPU pools: the attribute word is recorded, and the PAGE_GRAPHICS_* bits are dropped.
    MEMORY_BASIC_INFORMATION mbi{};
    void* const pool = virtual_alloc(nullptr, SIZE_T{8} << 30, MEM_RESERVE | 0x20400000, 0x21, PAGE_READWRITE);
    check(pool != nullptr, "attributes 0x21 with 64 KiB pages reserves 8 GiB");
    if (pool) {
        check(VirtualQuery(pool, &mbi, sizeof mbi) && mbi.State == MEM_RESERVE, "the 8 GiB pool is reserve-only");
        check(VirtualFree(pool, 0, MEM_RELEASE), "release the 8 GiB pool");
    }

    // The measured reserve/commit pair (docs/xmem_exe_calls.md): a 64 KiB-page range is reserved and committed later at
    // the same address. The reserve is a placeholder, which the host refuses to commit in place.
    void* const committed_pool = virtual_alloc(nullptr, 0x40000, MEM_RESERVE | 0x20400000, 0x21, PAGE_READWRITE);
    check(committed_pool != nullptr, "a 64 KiB-page range reserves");
    if (committed_pool) {
        void* const committed = virtual_alloc(committed_pool, 0x40000, MEM_COMMIT | 0x20400000, 0x283, 0x42404);
        check(committed == committed_pool, "a later commit of that range succeeds at its address");
        if (committed) {
            MEMORY_BASIC_INFORMATION committed_info{};
            // Wine accepts PAGE_WRITECOMBINE here but reports the region as plain PAGE_READWRITE.
            const bool committed_query =
                VirtualQuery(committed, &committed_info, sizeof committed_info) && committed_info.State == MEM_COMMIT;
            check(committed_query && (committed_info.Protect == (PAGE_READWRITE | PAGE_WRITECOMBINE) ||
                                      (hostwine::host_is_wine() && committed_info.Protect == PAGE_READWRITE)),
                  "the committed range is committed PAGE_READWRITE without the graphics bits");
            static_cast<volatile uint8_t*>(committed)[0] = 0x5c;
            check(static_cast<volatile uint8_t*>(committed)[0] == 0x5c, "the committed range is writable");
        }
        check(VirtualFree(committed_pool, 0, MEM_RELEASE), "release the committed range");
    }

    void* const noaccess = virtual_alloc(nullptr, 0x10000, MEM_RESERVE | 0x20400000, 0x3, 0x801);
    check(noaccess != nullptr, "attributes 0x3 with protect PAGE_GRAPHICS_NOACCESS|PAGE_NOACCESS succeeds");
    if (noaccess) {
        check(VirtualQuery(noaccess, &mbi, sizeof mbi) && mbi.AllocationProtect == PAGE_NOACCESS,
              "protect 0x801 becomes PAGE_NOACCESS");
        check(VirtualFree(noaccess, 0, MEM_RELEASE), "release the PAGE_NOACCESS pool");
    }

    void* const gpu = virtual_alloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, 0x183, 0x22004);
    check(gpu != nullptr, "attributes 0x183 with protect 0x22004 succeeds");
    if (gpu) {
        check(VirtualQuery(gpu, &mbi, sizeof mbi) && mbi.Protect == PAGE_READWRITE, "protect 0x22004 becomes PAGE_READWRITE");
        static_cast<volatile uint8_t*>(gpu)[0] = 0x5a;
        check(static_cast<volatile uint8_t*>(gpu)[0] == 0x5a, "the 0x183 allocation is writable");
        check(VirtualFree(gpu, 0, MEM_RELEASE), "release the 0x183 allocation");
    }

    // The two 64 KiB reserve-only forms reached at startup: 64 KiB-aligned, rounded to whole pages, nothing committed.
    constexpr ULONGLONG kReserveAttributes[] = {0x21, 0x03};
    constexpr DWORD kReserveProtect[] = {PAGE_READWRITE, 0x801};
    for (size_t i = 0; i < 2; ++i) {
        void* const reservation =
            virtual_alloc(nullptr, kPage64k + 1, MEM_RESERVE | kXmem64kPages, kReserveAttributes[i], kReserveProtect[i]);
        check(reservation != nullptr, "an attributed 64 KiB reserve returns an address");
        if (!reservation) continue;
        MEMORY_BASIC_INFORMATION region{};
        check(VirtualQuery(reservation, &region, sizeof(region)) == sizeof(region) &&
                  region.AllocationBase == reservation && region.RegionSize == 2 * kPage64k &&
                  reinterpret_cast<uintptr_t>(reservation) % kPage64k == 0 && region.State == MEM_RESERVE,
              "the attributed 64 KiB reserve is aligned, rounded up to whole pages and not committed");
        check(VirtualFree(reservation, 0, MEM_RELEASE), "release the attributed 64 KiB reserve");
    }

    SetLastError(ERROR_SUCCESS);
    void* const replace_without_address =
        virtual_alloc(nullptr, kPage64k, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | kXmem64kPages, 0, PAGE_READWRITE);
    check(replace_without_address == nullptr && GetLastError() == ERROR_INVALID_PARAMETER,
          "MEM_REPLACE_PLACEHOLDER without an address is rejected");
}

// XMemAllocDefault / XMemFreeDefault share XMemAlloc's heap: a block from either is freed by either.
void test_default_alloc(XMemAllocFn alloc, XMemFreeFn free_mem, XMemAllocFn alloc_default,
                        XMemFreeFn free_default) {
    void* const a = alloc_default(100, 0x5e50000);
    check(aligned_64(a), "XMemAllocDefault returns a 64-byte aligned block");
    void* const zero = alloc_default(0, 0);
    check(zero != nullptr, "XMemAllocDefault(0) returns non-null");
    void* const b = alloc(100, 0);
    if (a) std::memset(a, 0x11, 100);
    if (b) std::memset(b, 0x22, 100);
    free_mem(a, 0x5e50000);
    free_default(b, 0);
    free_default(zero, 0);
    check(true, "XMemFree releases an XMemAllocDefault block and XMemFreeDefault an XMemAlloc block");
}

uint32_t read_u32(const void* base, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
    return value;
}

void fill_page(void* page, uint32_t tag) {
    auto* words = static_cast<uint32_t*>(page);
    for (size_t i = 0; i < kPage64k / sizeof(uint32_t); i += 1024) words[i] = tag + static_cast<uint32_t>(i);
}

bool page_holds(const void* page, uint32_t tag) {
    for (size_t i = 0; i < kPage64k / sizeof(uint32_t); i += 1024) {
        if (read_u32(page, i * sizeof(uint32_t)) != tag + static_cast<uint32_t>(i)) return false;
    }
    return true;
}

bool reserved_only(const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(address, &info, sizeof(info)) && info.State == MEM_RESERVE;
}

struct PhysicalApi {
    XMemVirtualAllocFn virtual_alloc;
    XMemAllocatePhysicalPagesFn allocate;
    XMemFreePhysicalPagesFn free_pages;
    XMemMapPhysicalPagesFn map;
    XMemMapPhysicalPagesScatterFn scatter;
};

// Physical pages in a 64 KiB-page window, as the measured callers use them: reserve a window, allocate pages, map them,
// remap them elsewhere and in another order, unmap, free.
void test_physical_pages(const PhysicalApi& api) {
    constexpr uint64_t kPages = 4;
    auto* const window = static_cast<uint8_t*>(api.virtual_alloc(
        nullptr, kPages * kPage64k, MEM_RESERVE | kXmem64kPages, 0x23, PAGE_READWRITE | PAGE_GRAPHICS_READWRITE));
    check(window != nullptr, "XMemVirtualAlloc reserves a 64 KiB-page window");
    auto* const second = static_cast<uint8_t*>(api.virtual_alloc(
        nullptr, kPages * kPage64k, MEM_RESERVE | kXmem64kPages, 0x21, PAGE_READWRITE));
    check(second != nullptr, "XMemVirtualAlloc reserves a second window");
    if (!window || !second) return;
    check(reserved_only(window), "the window is reserved, not committed");

    uint64_t pages[kPages]{};
    uint64_t count = kPages;
    check(api.allocate(0x280, &count, pages) == TRUE && count == kPages,
          "XMemAllocatePhysicalPages allocates every page asked for and keeps the count");
    bool numbers_ok = true;
    for (uint64_t i = 0; i < kPages; ++i) {
        numbers_ok &= pages[i] != 0 && pages[i] % 16 == 0 && pages[i] < (uint64_t{1} << 25);
        for (uint64_t j = 0; j < i; ++j) numbers_ok &= pages[i] != pages[j];
    }
    check(numbers_ok, "page numbers are distinct non-zero multiples of 16 below 2^25");

    check(api.map(window, kPages, pages) == window, "XMemMapPhysicalPages returns the address it mapped");
    for (uint64_t i = 0; i < kPages; ++i) fill_page(window + i * kPage64k, 0x1000u * static_cast<uint32_t>(i + 1));
    {
        MEMORY_BASIC_INFORMATION info{};
        VirtualQuery(window, &info, sizeof(info));
        check(info.State == MEM_COMMIT && info.Type == MEM_MAPPED && info.Protect == PAGE_READWRITE,
              "a mapped page is a read-write view with the window's protection");
        check(info.AllocationBase == window && info.RegionSize == kPages * kPage64k,
              "consecutive pages of one allocation are mapped as one view");
    }

    // A page unmapped or remapped inside that view splits it; its neighbours keep their pages.
    void* const middle = window + kPage64k;
    check(api.scatter(&middle, 1, nullptr) == TRUE && reserved_only(window + kPage64k),
          "a page inside a view unmaps on its own");
    check(page_holds(window, 0x1000u) && page_holds(window + 2 * kPage64k, 0x3000u) &&
              page_holds(window + 3 * kPage64k, 0x4000u),
          "the pages around it stay mapped with their contents");
    check(api.map(middle, 1, &pages[1]) == middle && page_holds(window + kPage64k, 0x2000u),
          "a page maps again into the gap");

    check(api.map(window, kPages, nullptr) == window, "XMemMapPhysicalPages with no page array unmaps");
    check(reserved_only(window) && reserved_only(window + 3 * kPage64k), "unmapped pages are reserved again");
    // The split above left the range as several host placeholders; the whole run maps again over them.
    check(api.map(window, kPages, pages) == window && page_holds(window, 0x1000u) &&
              page_holds(window + 3 * kPage64k, 0x4000u),
          "a run maps again over a range an earlier partial unmap split");
    check(api.map(window, kPages, nullptr) == window && reserved_only(window), "and unmaps again");

    // Remap in reverse order into the other window: each page keeps its contents.
    uint64_t reversed[kPages];
    for (uint64_t i = 0; i < kPages; ++i) reversed[i] = pages[kPages - 1 - i];
    check(api.map(second, kPages, reversed) == second, "the pages map into another window");
    bool kept = true;
    for (uint64_t i = 0; i < kPages; ++i) {
        kept &= page_holds(second + i * kPage64k, 0x1000u * static_cast<uint32_t>(kPages - i));
    }
    check(kept, "a physical page keeps its contents across unmap and remap");

    // One page mapped at two addresses is one memory.
    check(api.map(window, 1, &pages[0]) == window, "a page already mapped elsewhere maps again");
    static_cast<volatile uint32_t*>(static_cast<void*>(window))[1] = 0xfeedf00d;
    check(read_u32(second + 3 * kPage64k, 4) == 0xfeedf00d, "both mappings of a page see the same memory");

    // Mapping over a mapped page replaces it; a 0 entry unmaps one page.
    const uint64_t replace[2] = {pages[1], 0};
    check(api.map(window, 2, replace) == window, "mapping over mapped pages replaces them");
    check(page_holds(window, 0x2000u) && reserved_only(window + kPage64k),
          "the first page now shows page 1 and a 0 entry left the second unmapped");

    // Scatter: entry i maps page i at address i, out of order; 0 unmaps.
    void* const addresses[3] = {window + 3 * kPage64k, window + 2 * kPage64k, window};
    const uint64_t scattered[3] = {pages[2], pages[3], 0};
    check(api.scatter(addresses, 3, scattered) == TRUE, "XMemMapPhysicalPagesScatter maps each page at its address");
    check(page_holds(window + 3 * kPage64k, 0x3000u) && page_holds(window + 2 * kPage64k, 0x4000u) &&
              reserved_only(window),
          "scattered pages land at their own addresses and a 0 entry unmaps");
    check(api.scatter(addresses, 3, nullptr) == TRUE && reserved_only(window + 3 * kPage64k),
          "XMemMapPhysicalPagesScatter with no page array unmaps");

    uint8_t outside = 0;
    SetLastError(ERROR_SUCCESS);
    check(api.map(reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(&outside) + 0xffff) & ~uintptr_t{0xffff}), 1,
                  pages) == nullptr,
          "mapping outside any window fails");
    check(api.map(window + 1, 1, pages) == nullptr && GetLastError() == ERROR_INVALID_PARAMETER,
          "mapping at an address that is not 64 KiB-aligned fails");
    const uint64_t bogus = 0x1fffff0;  // the highest page number, far above the few this test allocates
    check(api.map(window, 1, &bogus) == nullptr, "mapping a page number that was never allocated fails");

    // A window with a cache modifier, as the retail title reserves one (PAGE_READWRITE | PAGE_WRITECOMBINE plus a
    // graphics bit). The view gets plain read-write: MapViewOfFile3 refuses the modifier on Wine.
    auto* const combined = static_cast<uint8_t*>(api.virtual_alloc(
        nullptr, kPage64k, MEM_RESERVE | kXmem64kPages, 0x27,
        PAGE_READWRITE | PAGE_WRITECOMBINE | PAGE_GRAPHICS_READWRITE));
    check(combined != nullptr, "XMemVirtualAlloc reserves a write-combined window");
    if (combined) {
        check(api.map(combined, 1, &pages[1]) == combined, "a page maps into a write-combined window");
        MEMORY_BASIC_INFORMATION info{};
        VirtualQuery(combined, &info, sizeof(info));
        check(info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE && page_holds(combined, 0x2000u),
              "the write-combined window's view is plain read-write and shows the page");
        check(api.map(combined, 1, nullptr) == combined, "unmap the write-combined window");
    }

    check(api.map(second, kPages, nullptr) == second, "unmap the second window");
    check(api.free_pages(kPages, pages) == TRUE, "XMemFreePhysicalPages frees the pages");
    check(api.free_pages(1, pages) == FALSE, "freeing a page twice fails");

    // Freed numbers are reused.
    uint64_t again[kPages]{};
    count = kPages;
    check(api.allocate(0x180, &count, again) == TRUE && count == kPages, "pages can be allocated again");
    bool reused = true;
    for (uint64_t i = 0; i < kPages; ++i) {
        bool found = false;
        for (uint64_t j = 0; j < kPages; ++j) found |= again[i] == pages[j];
        reused &= found;
    }
    check(reused, "freed page numbers are reused");
    check(api.map(window, 1, again) == window && read_u32(window, 0) == 0, "a fresh page maps and reads as zero");
    api.map(window, 1, nullptr);
    api.free_pages(kPages, again);

    uint64_t zero_count = 0;
    check(api.allocate(0x280, &zero_count, nullptr) == TRUE, "allocating zero pages succeeds");
    check(api.allocate(0x280, nullptr, pages) == FALSE, "a null count fails");
    // The windows were split into one placeholder per page; the process exit releases them.
}

DWORD protection_of(const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(address, &info, sizeof info) ? info.Protect : 0;
}

void test_mapping_protection(const PhysicalApi& api) {
    constexpr uint64_t kPages = 2;
    auto* const rw = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, kPages * kPage64k, MEM_RESERVE | kXmem64kPages, 0x21, PAGE_READWRITE));
    auto* const ro = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, kPages * kPage64k, MEM_RESERVE | kXmem64kPages, 0x21, PAGE_READONLY));
    auto* const none = static_cast<uint8_t*>(api.virtual_alloc(nullptr, kPages * kPage64k, MEM_RESERVE | kXmem64kPages,
                                                               0x3, PAGE_NOACCESS | PAGE_GRAPHICS_NOACCESS));
    check(rw && ro && none, "reserve PAGE_READWRITE, PAGE_READONLY and 0x801 windows");
    uint64_t pages[kPages]{};
    uint64_t count = kPages;
    check(api.allocate(0x280, &count, pages) == TRUE && count == kPages, "allocate the pages they share");
    if (!rw || !ro || !none || count != kPages) return;
    check(api.map(rw, kPages, pages) == rw && api.map(ro, kPages, pages) == ro && api.map(none, kPages, pages) == none,
          "the same pages map into all three windows");
    std::memset(rw, 0x3c, kPages * kPage64k);
    check(std::memcmp(rw, ro, kPages * kPage64k) == 0, "the read-only mapping reads what the read-write one wrote");
    check(protection_of(ro) == PAGE_READONLY, "a page mapped in a PAGE_READONLY window is read-only");
    check(protection_of(none) == PAGE_NOACCESS, "a page mapped in a 0x801 window is PAGE_NOACCESS");
    check(api.map(rw, kPages, nullptr) == rw && api.map(ro, kPages, nullptr) == ro &&
              api.map(none, kPages, nullptr) == none,
          "unmap all three");
    check(api.free_pages(kPages, pages) == TRUE, "free the shared pages");
}

// The placeholder flags pass through: reserve a placeholder, carve windows and committed memory out of it.
void test_placeholders(const PhysicalApi& api) {
    auto* const base = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, 6 * kPage64k, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, 7, PAGE_NOACCESS));
    check(base != nullptr, "XMemVirtualAlloc reserves a MEM_RESERVE_PLACEHOLDER region");
    if (!base) return;

    void* const window = api.virtual_alloc(base + kPage64k, 2 * kPage64k,
                                           MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | kXmem64kPages, 0x27,
                                           PAGE_READWRITE | PAGE_GRAPHICS_READONLY);
    check(window == base + kPage64k, "a reserve-only MEM_REPLACE_PLACEHOLDER returns the address it replaced");

    auto* const committed = static_cast<uint8_t*>(
        api.virtual_alloc(base + 4 * kPage64k, kPage64k,
                          MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | kXmem64kPages, 0x27, PAGE_READWRITE));
    check(committed == base + 4 * kPage64k, "a committing MEM_REPLACE_PLACEHOLDER returns the address it replaced");
    if (committed) {
        committed[0] = 0x5a;
        check(committed[0] == 0x5a, "the committed replacement is writable");
    }

    uint64_t pages[2]{};
    uint64_t count = 2;
    check(api.allocate(0x380, &count, pages) == TRUE && count == 2, "allocate two pages for the carved window");
    check(api.map(window, 2, pages) == window, "pages map into a window carved out of a placeholder");
    if (window) {
        fill_page(static_cast<uint8_t*>(window) + kPage64k, 0xabc0000u);
        check(page_holds(static_cast<uint8_t*>(window) + kPage64k, 0xabc0000u), "the carved window holds data");
    }
    check(api.map(window, 2, nullptr) == window, "unmap the carved window");
    check(api.free_pages(2, pages) == TRUE, "free the carved window's pages");
    check(reserved_only(base), "the rest of the placeholder is still reserved");
}

// Carving or committing a sub-range of an existing window keeps the untouched prefix and suffix mappable, and
// reusing an address released outside the shim (UnmapViewOfFile) drops stale mapped-page entries.
void test_window_carve_and_reuse(const PhysicalApi& api) {
    // 1. Carving a sub-window out of an existing window preserves its untouched prefix and suffix.
    auto* const carved = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, 4 * kPage64k, MEM_RESERVE | kXmem64kPages, 0x21, PAGE_READWRITE));
    check(carved != nullptr, "reserve a 4-page window to carve a sub-window from");
    if (carved) {
        void* const mid = api.virtual_alloc(carved + kPage64k, kPage64k,
                                            MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | kXmem64kPages, 0x21, PAGE_READONLY);
        check(mid == carved + kPage64k, "carve a middle sub-window with MEM_REPLACE_PLACEHOLDER");
        uint64_t pages[3]{};
        uint64_t count = 3;
        check(api.allocate(0x280, &count, pages) == TRUE && count == 3, "allocate three pages for carved-window test");
        check(api.map(carved, 1, &pages[0]) == carved, "prefix before a carved sub-window stays mappable");
        check(api.map(carved + kPage64k, 1, &pages[1]) == carved + kPage64k, "carved sub-window is mappable");
        check(api.map(carved + 2 * kPage64k, 1, &pages[2]) == carved + 2 * kPage64k,
              "suffix after a carved sub-window stays mappable");
        if (count == 3) {
            MEMORY_BASIC_INFORMATION mbi{};
            check(VirtualQuery(carved, &mbi, sizeof mbi) && mbi.Protect == PAGE_READWRITE,
                  "prefix keeps the original window protection");
            check(VirtualQuery(carved + kPage64k, &mbi, sizeof mbi) && mbi.Protect == PAGE_READONLY,
                  "carved sub-window uses its replacement protection");
            check(VirtualQuery(carved + 2 * kPage64k, &mbi, sizeof mbi) && mbi.Protect == PAGE_READWRITE,
                  "suffix keeps the original window protection");
        }
        api.map(carved, 1, nullptr);
        api.map(carved + kPage64k, 1, nullptr);
        api.map(carved + 2 * kPage64k, 1, nullptr);
        api.free_pages(3, pages);
    }

    // 2. Committing a sub-range of a window removes that sub-range from g_windows while keeping its neighbours.
    auto* const split = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, 4 * kPage64k, MEM_RESERVE | kXmem64kPages, 0x21, PAGE_READWRITE));
    check(split != nullptr, "reserve a 4-page window to commit a sub-range from");
    if (split) {
        auto* const mid = static_cast<uint8_t*>(api.virtual_alloc(
            split + kPage64k, kPage64k, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | kXmem64kPages, 0x21,
            PAGE_READWRITE));
        check(mid == split + kPage64k, "commit a middle sub-range of a physical-page window");
        if (mid) mid[0] = 0x5b;
        uint64_t pages[2]{};
        uint64_t count = 2;
        check(api.allocate(0x280, &count, pages) == TRUE && count == 2, "allocate two pages for split-window test");
        // Spanning into the committed sub-range must fail upfront without mapping page 0.
        SetLastError(ERROR_SUCCESS);
        check(api.map(split, 2, pages) == nullptr && GetLastError() == ERROR_INVALID_ADDRESS && reserved_only(split),
              "mapping across a committed sub-range fails upfront and leaves prefix unmapped");
        check(api.map(split, 1, &pages[0]) == split, "prefix before a committed sub-range stays mappable");
        check(api.map(split + 2 * kPage64k, 1, &pages[1]) == split + 2 * kPage64k,
              "suffix after a committed sub-range stays mappable");
        check(mid && mid[0] == 0x5b, "committed sub-range keeps its contents");
        api.map(split, 1, nullptr);
        api.map(split + 2 * kPage64k, 1, nullptr);
        api.free_pages(2, pages);
    }

    // 3. Reusing a window address released externally via UnmapViewOfFile clears stale mapped-page tracking.
    auto* const reused = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, kPage64k, MEM_RESERVE | kXmem64kPages, 0x21, PAGE_READWRITE));
    check(reused != nullptr, "reserve a window to test external unmap and address reuse");
    if (reused) {
        uint64_t pages[2]{};
        uint64_t count = 2;
        check(api.allocate(0x280, &count, pages) == TRUE && count == 2, "allocate two pages for reuse test");
        check(api.map(reused, 1, &pages[0]) == reused, "map first page before external UnmapViewOfFile");
        fill_page(reused, 0xdead0000u);
        check(UnmapViewOfFile(reused) != FALSE, "host UnmapViewOfFile releases the view outside the shim");
        void* const again =
            api.virtual_alloc(reused, kPage64k, MEM_RESERVE | kXmem64kPages, 0x21, PAGE_READWRITE);
        check(again == reused, "re-reserve a physical-page window at the externally freed address");
        if (again) {
            const bool remapped = api.map(again, 1, &pages[1]) == again;
            check(remapped, "XMemMapPhysicalPages succeeds at a reused address without failing on stale g_mapped");
            if (remapped) {
                check(read_u32(again, 0) == 0, "fresh page mapped at reused address reads as zero");
                // Also verify a plain MEM_RESERVE_PLACEHOLDER at an externally freed address clears ghost windows.
                check(UnmapViewOfFile(again) != FALSE, "host UnmapViewOfFile releases the second view outside the shim");
                void* const plain =
                    api.virtual_alloc(again, kPage64k, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, 7, PAGE_NOACCESS);
                check(plain == again, "re-reserve a plain placeholder at the externally freed address");
                if (plain) {
                    SetLastError(ERROR_SUCCESS);
                    check(api.map(plain, 1, &pages[0]) == nullptr && GetLastError() == ERROR_INVALID_ADDRESS,
                          "plain placeholder at a reused window address does not inherit the old window entry");
                    VirtualFree(plain, 0, MEM_RELEASE);
                }
            }
        }
        api.free_pages(2, pages);
    }
}

using HostVirtualFreeFn = BOOL(WINAPI*)(void*, SIZE_T, DWORD);
using TitleVirtualAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, DWORD);

bool released(const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(address, &info, sizeof(info)) && info.State == MEM_FREE;
}

bool all_released(const uint8_t* base, size_t pages) {
    for (size_t i = 0; i < pages; ++i) {
        if (!released(base + i * kPage64k)) return false;
    }
    return true;
}

// The title releases its physical-page reservations with kernel32!VirtualFree(base, 0, MEM_RELEASE). Once pages are
// mapped, a reservation is one placeholder per page plus views, which the host would release only the first of.
void test_title_virtual_free(const PhysicalApi& api) {
    const auto title_free = reinterpret_cast<HostVirtualFreeFn>(rt::shims::find("kernel32.dll", "VirtualFree"));
    check(title_free != nullptr, "registry exposes kernel32.dll!VirtualFree");
    if (!title_free) return;
    bool same = true;
    for (const char* dll : {"kernelbase.dll", "api-ms-win-core-memory-l1-1-0.dll", "api-ms-win-core-memory-l1-1-6.dll"}) {
        same &= rt::shims::find(dll, "VirtualFree") == reinterpret_cast<void*>(title_free);
    }
    check(same, "the memory api sets serve the same VirtualFree");

    // A window with pages mapped at its first and third page.
    constexpr size_t kPages = 4;
    auto* const window = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, kPages * kPage64k, MEM_RESERVE | kXmem64kPages, 0x23, PAGE_READWRITE));
    uint64_t pages[2]{};
    uint64_t count = 2;
    check(window && api.allocate(0x280, &count, pages) == TRUE && count == 2, "a window and two pages");
    if (!window || count != 2) return;
    check(api.map(window, 1, &pages[0]) == window && api.map(window + 2 * kPage64k, 1, &pages[1]) != nullptr,
          "map the pages at the window's first and third page");
    check(title_free(window, 0, MEM_RELEASE) == TRUE, "VirtualFree(window, 0, MEM_RELEASE) succeeds");
    check(all_released(window, kPages), "the whole window is free afterwards, views and split placeholders included");
    check(api.map(window, 1, &pages[0]) == nullptr, "nothing maps into a released window");
    check(api.free_pages(2, pages) == TRUE, "the pages that were mapped there can still be freed");

    // A placeholder reservation with a carved window (one page mapped) and a committed replacement.
    auto* const base = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, 6 * kPage64k, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, 7, PAGE_NOACCESS));
    check(base != nullptr, "a placeholder reservation");
    if (!base) return;
    auto* const carved = static_cast<uint8_t*>(api.virtual_alloc(
        base + kPage64k, 2 * kPage64k, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | kXmem64kPages, 0x27, PAGE_READWRITE));
    auto* const committed = static_cast<uint8_t*>(
        api.virtual_alloc(base + 4 * kPage64k, kPage64k,
                          MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | kXmem64kPages, 0x27, PAGE_READWRITE));
    count = 1;
    check(carved && committed && api.allocate(0x280, &count, pages) == TRUE &&
              api.map(carved + kPage64k, 1, &pages[0]) != nullptr,
          "carve a window and a committed range out of it, and map a page in the window");
    check(title_free(base, 0, MEM_RELEASE) == TRUE, "VirtualFree(base, 0, MEM_RELEASE) releases the reservation");
    check(all_released(base, 6), "every page of it is free: placeholders, the window's view and the committed range");
    check(api.map(carved, 1, &pages[0]) == nullptr, "the carved window is gone too");
    api.free_pages(1, pages);

    // A carved window released on its own leaves the rest of its reservation.
    auto* const outer = static_cast<uint8_t*>(
        api.virtual_alloc(nullptr, 4 * kPage64k, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, 7, PAGE_NOACCESS));
    auto* const inner = outer ? static_cast<uint8_t*>(api.virtual_alloc(
                                    outer + kPage64k, 2 * kPage64k,
                                    MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | kXmem64kPages, 0x27, PAGE_READWRITE))
                              : nullptr;
    count = 1;
    check(inner && api.allocate(0x280, &count, pages) == TRUE && api.map(inner, 1, &pages[0]) == inner,
          "a window carved out of a reservation, one page mapped");
    if (!inner) return;
    check(title_free(inner, 0, MEM_RELEASE) == TRUE && all_released(inner, 2),
          "VirtualFree of the carved window releases its range");
    check(reserved_only(outer) && reserved_only(outer + 3 * kPage64k), "the rest of the reservation stays reserved");
    check(title_free(outer, 0, MEM_RELEASE) == TRUE && all_released(outer, 4), "and is released with its base");
    api.free_pages(1, pages);

    // A hole released inside a reservation and allocated again belongs to whoever allocated it: releasing the outer
    // reservation leaves it, whether the title's VirtualAlloc or XMemVirtualAlloc made it.
    const auto title_alloc = reinterpret_cast<TitleVirtualAllocFn>(rt::shims::find("kernel32.dll", "VirtualAlloc"));
    for (const bool by_xmem : {false, true}) {
        auto* const ring = static_cast<uint8_t*>(
            api.virtual_alloc(nullptr, 4 * kPage64k, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, 7, PAGE_NOACCESS));
        auto* const hole = ring ? static_cast<uint8_t*>(api.virtual_alloc(
                                      ring + kPage64k, 2 * kPage64k,
                                      MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | kXmem64kPages, 0x27, PAGE_READWRITE))
                                : nullptr;
        check(hole && title_free(hole, 0, MEM_RELEASE) == TRUE, "carve a window out of a reservation and release it");
        if (!hole || !title_alloc) return;
        void* const again =
            by_xmem ? api.virtual_alloc(hole, 2 * kPage64k, MEM_RESERVE | kXmem64kPages, 0x23, PAGE_READWRITE)
                    : title_alloc(hole, 2 * kPage64k, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        check(again == hole, by_xmem ? "XMemVirtualAlloc reserves the hole again" : "VirtualAlloc takes the hole again");
        if (!by_xmem && again) static_cast<volatile uint8_t*>(again)[0] = 0x6b;
        check(title_free(ring, 0, MEM_RELEASE) == TRUE && released(ring) && released(ring + 3 * kPage64k),
              "releasing the outer reservation frees its own pages");
        check(!released(hole) && !released(hole + kPage64k), "and leaves the allocation in the hole");
        if (by_xmem) {
            uint64_t page = 0;
            count = 1;
            check(api.allocate(0x280, &count, &page) == TRUE && api.map(hole, 1, &page) == hole,
                  "the window in the hole still takes pages");
            api.map(hole, 1, nullptr);
            api.free_pages(1, &page);
        } else {
            check(static_cast<volatile uint8_t*>(again)[0] == 0x6b, "and its contents");
        }
        check(title_free(hole, 0, MEM_RELEASE) == TRUE && all_released(hole, 2), "the hole's allocation releases");
    }

    // Everything else is the host's.
    void* const plain = VirtualAlloc(nullptr, kPage64k, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(plain && title_free(plain, 0, MEM_RELEASE) == TRUE && released(plain), "VirtualFree of ordinary memory");
}

// gta6rt --trace: one "xmem_attributes" event per distinct raw attribute word and API, none with the trace off.
void test_attribute_trace(XMemAllocFn alloc, XMemFreeFn free_mem, XMemVirtualAllocFn virtual_alloc) {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring path =
        std::wstring(temp) + L"xmem_trace_test_" + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    DeleteFileW(path.c_str());

    void* untraced = alloc(64, 0x1111);  // trace not open yet: nothing to write, nothing to crash
    check(untraced != nullptr, "XMemAlloc with the trace off still allocates");
    free_mem(untraced, 0x1111);

    check(rt::startup_trace::open(path.c_str()), "open the startup trace file");
    void* a = alloc(4096, 0x5e50000);
    void* b = alloc(8192, 0x5e50000);  // same word again: no second event
    void* c = alloc(16, 0x99);
    free_mem(a, 0x5e50000);
    free_mem(b, 0x5e50000);  // same word again: no second event
    free_mem(c, 0x99);
    free_mem(nullptr, 0x77);  // a free-only word is recorded too
    void* const v = virtual_alloc ? virtual_alloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, 0x183, PAGE_READWRITE)
                                  : nullptr;
    if (v) VirtualFree(v, 0, MEM_RELEASE);
    const auto title_alloc = reinterpret_cast<decltype(&VirtualAlloc)>(rt::shims::find("kernel32.dll", "VirtualAlloc"));
    if (title_alloc) {
        void* const title = title_alloc(nullptr, kPage4m, MEM_RESERVE | MEM_COMMIT | kXmem4MbPages, PAGE_READWRITE);
        check(title != nullptr, "title page allocation succeeds with tracing enabled");
        if (title) VirtualFree(title, 0, MEM_RELEASE);
    }
    rt::startup_trace::close();

    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    const std::string log = text.str();
    auto count = [&](const std::string& needle) {
        int n = 0;
        for (size_t at = log.find(needle); at != std::string::npos; at = log.find(needle, at + 1)) ++n;
        return n;
    };
    check(count("\"kind\":\"xmem_attributes\"") == 6, "six events: two XMemAlloc words, three XMemFree words and one XMemVirtualAlloc word");
    check(count("\"api\":\"XMemVirtualAlloc\",\"attributes\":\"0x183\",\"size\":65536") == 1,
          "XMemVirtualAlloc 0x183 is recorded once, with its size");
    check(count("\"api\":\"XMemVirtualAlloc\",\"attributes\":\"0x0\"") == 0,
          "title VirtualAlloc emits no xmem_attributes event");
    check(count("\"api\":\"XMemAlloc\",\"attributes\":\"0x5e50000\",\"size\":4096") == 1,
          "XMemAlloc 0x5e50000 is recorded once, with the size of the first call");
    check(count("\"api\":\"XMemAlloc\",\"attributes\":\"0x99\",\"size\":16") == 1, "XMemAlloc 0x99 is recorded once");
    check(count("\"api\":\"XMemFree\",\"attributes\":\"0x5e50000\"") == 1, "XMemFree 0x5e50000 is recorded once");
    check(count("\"api\":\"XMemFree\",\"attributes\":\"0x77\"") == 1, "XMemFree 0x77 is recorded");
    check(count("\"attributes\":\"0x1111\"") == 0, "nothing was recorded while the trace was off");
    check(count("\"caller\":\"") == 6, "every event names its caller");
    DeleteFileW(path.c_str());
}

#ifdef _MSC_VER
struct PhysicalTraceProbe {
    XMemAllocatePhysicalPagesFn allocate;
    XMemFreePhysicalPagesFn free_pages;
    bool traced;
};

DWORD WINAPI physical_trace_worker(void* argument) {
    const auto& probe = *static_cast<const PhysicalTraceProbe*>(argument);
    const auto request = [&](uint64_t attributes, uint64_t requested) {
        uint64_t count = requested;
        uint64_t pages[3]{};
        if (!probe.allocate(attributes, &count, pages) || count > std::size(pages)) return false;
        const bool freed = probe.free_pages(count, pages) != FALSE;
        return count == requested && freed;
    };

    // The TLS host can enter the shim before the runtime's CRT has initialized this thread's epoch.
    const int saved_epoch = _Init_thread_epoch;
    _Init_thread_epoch = 0;
    const bool ok = probe.traced ? request(0x1111, 2) && request(0x1111, 1) && request(0x2222, 3)
                                 : request(0x1111, 1);
    _Init_thread_epoch = saved_epoch;
    return ok ? 0 : 1;
}

void test_physical_trace_first_call() {
    PhysicalTraceProbe probe{
        reinterpret_cast<XMemAllocatePhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemAllocatePhysicalPages")),
        reinterpret_cast<XMemFreePhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemFreePhysicalPages")),
        false,
    };
    check(probe.allocate && probe.free_pages, "physical page exports are registered");
    if (!probe.allocate || !probe.free_pages) return;

    const auto run_worker = [&] {
        HANDLE thread = CreateThread(nullptr, 0, physical_trace_worker, &probe, 0, nullptr);
        if (!thread) return false;
        DWORD result = 1;
        const bool done = WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0 &&
                          GetExitCodeThread(thread, &result);
        CloseHandle(thread);
        return done && result == 0;
    };
    check(run_worker(), "physical pages allocate and free without tracing on the zero-epoch thread");

    wchar_t temp[MAX_PATH];
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    check(length > 0 && length < MAX_PATH, "find a directory for the physical-page trace");
    if (!length || length >= MAX_PATH) return;
    const std::wstring path =
        std::wstring(temp) + L"xmem_physical_trace_" + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    const bool opened = rt::startup_trace::open(path.c_str());
    check(opened, "open the startup trace before entering the worker");
    if (!opened) return;
    probe.traced = true;
    check(run_worker(), "physical pages allocate and free with tracing on the zero-epoch thread");
    rt::startup_trace::close();

    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    in.close();
    DeleteFileW(path.c_str());
    const std::string log = text.str();
    const auto count = [&](const char* needle) {
        std::size_t hits = 0;
        for (auto at = log.find(needle); at != std::string::npos; at = log.find(needle, at + 1)) ++hits;
        return hits;
    };
    check(count("\"kind\":\"xmem_physical_pages\"") == 2, "one event per distinct physical-page attribute");
    check(count("\"api\":\"XMemAllocatePhysicalPages\",\"attributes\":\"0x1111\",\"pages\":2,\"caller\":\"") == 1,
          "trace keeps the page count from the first traced call for each attribute");
    check(count("\"api\":\"XMemAllocatePhysicalPages\",\"attributes\":\"0x2222\",\"pages\":3,\"caller\":\"") == 1,
          "another physical-page attribute gets its own event with caller");
}
#endif

// XMemTransferMemory moves budget between the console's title and system partitions; a PC has neither, so the shim
// succeeds for every direction and size and moves (allocates) nothing. The XMEM_TRANSFER_TYPE values are not public,
// so the directions here are just the first few enum values and one far outside any plausible range.
void test_transfer_memory(XMemTransferMemoryFn transfer) {
    constexpr SIZE_T kMiB = 1024 * 1024;
    const uint32_t types[] = {0u, 1u, 2u, 0xffffffffu};
    // 2 MiB and 40 MiB are the sizes the samples pass ("Must be multiples of 2MB sizes"); 0, an odd size and SIZE_MAX
    // are the boundaries.
    const SIZE_T sizes[] = {0, 2 * kMiB, 40 * kMiB, 2 * kMiB + 1, (std::numeric_limits<SIZE_T>::max)()};

    PROCESS_MEMORY_COUNTERS_EX before{};
    const bool have_before = GetProcessMemoryInfo(GetCurrentProcess(),
                                                  reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof(before));
    bool all_true = true;
    bool last_error_kept = true;
    for (const uint32_t type : types) {
        for (const SIZE_T size : sizes) {
            SetLastError(0x2bad);
            const BOOL result = transfer(type, size);
            if (result != TRUE) {
                all_true = false;
                std::printf("  XMemTransferMemory(%u, %zu) -> %d\n", type, static_cast<size_t>(size), result);
            }
            last_error_kept &= GetLastError() == 0x2bad;
        }
    }
    check(all_true, "XMemTransferMemory returns TRUE (1) for every direction and size, 0 and SIZE_MAX included");
    check(last_error_kept, "XMemTransferMemory leaves the thread's last error alone");

    PROCESS_MEMORY_COUNTERS_EX after{};
    const bool have_after = GetProcessMemoryInfo(GetCurrentProcess(),
                                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof(after));
    // 20 calls asked to move up to 40 MiB each; nothing may be committed for them. 1 MiB of slack covers the CRT's
    // own buffers (the one-time stderr line).
    check(have_before && have_after && after.PrivateUsage < before.PrivateUsage + kMiB,
          "XMemTransferMemory commits no memory for the transferred sizes");

    // A bool-returning declaration reads only AL: TRUE must be non-zero there as well.
    const auto as_bool = reinterpret_cast<bool(WINAPI*)(uint32_t, SIZE_T)>(reinterpret_cast<void*>(transfer));
    check(as_bool(0, 2 * kMiB), "XMemTransferMemory's result also reads as true through a bool return");

    // The enum is 32 bits: garbage in the upper half of RCX must not change anything.
    const auto wide = reinterpret_cast<BOOL(WINAPI*)(uint64_t, SIZE_T)>(reinterpret_cast<void*>(transfer));
    check(wide(0xdead000000000000ull, 2 * kMiB) == TRUE, "XMemTransferMemory ignores the upper half of RCX");
}

// gta6rt --trace: one "xmem_transfer" event per distinct (type, size), none with the trace off.
void test_transfer_trace(XMemTransferMemoryFn transfer) {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring path =
        std::wstring(temp) + L"xmem_transfer_trace_test_" + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    DeleteFileW(path.c_str());

    check(transfer(7, 12345) == TRUE, "XMemTransferMemory with the trace off still returns TRUE");

    check(rt::startup_trace::open(path.c_str()), "open the startup trace file");
    transfer(0, 2097152);
    transfer(0, 2097152);  // same pair again: no second event
    transfer(0, 41943040);
    transfer(1, 2097152);
    rt::startup_trace::close();

    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    const std::string log = text.str();
    auto count = [&](const std::string& needle) {
        int n = 0;
        for (size_t at = log.find(needle); at != std::string::npos; at = log.find(needle, at + 1)) ++n;
        return n;
    };
    check(count("\"kind\":\"xmem_transfer\"") == 3, "three xmem_transfer events: one per distinct (type, size)");
    check(count("\"api\":\"XMemTransferMemory\",\"type\":0,\"size\":2097152") == 1,
          "type 0 / 2 MiB is recorded once although it was called twice");
    check(count("\"api\":\"XMemTransferMemory\",\"type\":0,\"size\":41943040") == 1, "type 0 / 40 MiB is recorded");
    check(count("\"api\":\"XMemTransferMemory\",\"type\":1,\"size\":2097152") == 1, "type 1 / 2 MiB is recorded");
    check(count("\"size\":12345") == 0, "nothing was recorded while the trace was off");
    check(count("\"caller\":\"") == 3, "every transfer event names its caller");
    DeleteFileW(path.c_str());
}

using HostVirtualAllocFn = void*(WINAPI*)(void*, SIZE_T, DWORD, DWORD);

// RT-217: a title's kernel32!VirtualAlloc carries the console's MEM_GRAPHICS / MEM_TITLE type bits, which the host
// rejects. The registered shim has to drop them and still allocate.
void test_title_virtual_alloc() {
    constexpr DWORD kMemGraphics = 0x10000000;
    constexpr DWORD kMemTitle = 0x40000000;
    const char* const dlls[] = {"kernel32.dll", "kernelbase.dll", "api-ms-win-core-memory-l1-1-0.dll"};
    for (const char* dll : dlls) {
        const auto alloc = reinterpret_cast<HostVirtualAllocFn>(rt::shims::find(dll, "VirtualAlloc"));
        char what[160];
        std::snprintf(what, sizeof what, "registry exposes %s!VirtualAlloc", dll);
        check(alloc != nullptr, what);
        if (!alloc) continue;

        const struct { DWORD bits; SIZE_T page; } modes[] = {
            {kXmemLargePages, kPage2m}, {kXmem4MbPages, kPage4m},
            {kXmem4MbPages | kXmemLargePages, kPage4m}, {kXmem64kPages, kPage64k},
        };
        for (const auto& mode : modes) {
            for (const DWORD placeholder : {DWORD{MEM_RESERVE_PLACEHOLDER}, DWORD{MEM_REPLACE_PLACEHOLDER}}) {
                SetLastError(0);
                void* const refused = alloc(nullptr, mode.page, MEM_RESERVE | placeholder | mode.bits, PAGE_NOACCESS);
                std::snprintf(what, sizeof what, "%s!VirtualAlloc refuses placeholder flag %#lx with page bits %#lx", dll,
                              static_cast<unsigned long>(placeholder), static_cast<unsigned long>(mode.bits));
                check(refused == nullptr && GetLastError() == ERROR_INVALID_PARAMETER, what);
                if (refused) VirtualFree(refused, 0, MEM_RELEASE);
            }
            for (const DWORD tags : {DWORD{0}, kMemGraphics | kMemTitle}) {
                void* const p = alloc(nullptr, mode.page + 1, MEM_RESERVE | MEM_COMMIT | mode.bits | tags,
                                      PAGE_READWRITE | PAGE_GRAPHICS_READWRITE);
                std::snprintf(what, sizeof what, "%s!VirtualAlloc page bits %#lx tags %#lx returns aligned memory",
                              dll, static_cast<unsigned long>(mode.bits), static_cast<unsigned long>(tags));
                check(p && (reinterpret_cast<uintptr_t>(p) & (mode.page - 1)) == 0, what);
                if (p) {
                    MEMORY_BASIC_INFORMATION info{};
                    check(VirtualQuery(p, &info, sizeof(info)) && info.State == MEM_COMMIT &&
                              info.RegionSize == 2 * mode.page && info.Protect == PAGE_READWRITE,
                          "title page modes round to whole pages and strip graphics protection");
                    auto* const bytes = static_cast<volatile uint8_t*>(p);
                    bytes[0] = 0x51;
                    bytes[2 * mode.page - 1] = 0x52;
                    check(bytes[0] == 0x51 && bytes[2 * mode.page - 1] == 0x52, "title guest pages are writable");
                    check(VirtualFree(p, 0, MEM_RELEASE), "release the title guest-page region");
                }
                SetLastError(ERROR_SUCCESS);
                void* const no_operation = alloc(nullptr, mode.page, mode.bits | tags, PAGE_READWRITE);
                check(no_operation == nullptr && GetLastError() == ERROR_INVALID_PARAMETER,
                      "title guest pages require MEM_RESERVE or MEM_COMMIT");
                if (no_operation) VirtualFree(no_operation, 0, MEM_RELEASE);

                SetLastError(ERROR_SUCCESS);
                void* const commit_only = alloc(nullptr, mode.page, MEM_COMMIT | mode.bits | tags, PAGE_READWRITE);
                check(commit_only == nullptr && GetLastError() == ERROR_INVALID_PARAMETER,
                      "title commit-only with a null address fails for every guest page mode");
                if (commit_only) VirtualFree(commit_only, 0, MEM_RELEASE);

                SetLastError(ERROR_SUCCESS);
                void* const low = alloc(reinterpret_cast<void*>(mode.page - 1), 1,
                                        MEM_RESERVE | MEM_COMMIT | mode.bits | tags, PAGE_READWRITE);
                check(low == nullptr && GetLastError() == ERROR_INVALID_ADDRESS,
                      "title rejects an address below the guest page granularity");
                if (low) VirtualFree(low, 0, MEM_RELEASE);

                auto* const reserved = static_cast<uint8_t*>(
                    alloc(nullptr, mode.page + 1, MEM_RESERVE | mode.bits | tags, PAGE_READWRITE));
                check(reserved && (reinterpret_cast<uintptr_t>(reserved) & (mode.page - 1)) == 0,
                      "title reserve-only returns an aligned ordinary reservation");
                if (reserved) {
                    SYSTEM_INFO system_info{};
                    GetSystemInfo(&system_info);
                    const SIZE_T part_size = system_info.dwPageSize;
                    MEMORY_BASIC_INFORMATION info{};
                    check(VirtualQuery(reserved, &info, sizeof(info)) && info.State == MEM_RESERVE &&
                              info.RegionSize == 2 * mode.page,
                          "title reserve-only rounds to whole guest pages");
                    auto* const part = static_cast<volatile uint8_t*>(
                        VirtualAlloc(reserved + part_size, part_size, MEM_COMMIT, PAGE_READWRITE));
                    check(part == reserved + part_size, "plain host VirtualAlloc commits a subrange of the title reservation");
                    if (part) {
                        part[0] = 0x61;
                        part[part_size - 1] = 0x62;
                        check(part[0] == 0x61 && part[part_size - 1] == 0x62,
                              "the title reservation's committed subrange is writable");
                        check(alloc(reserved + part_size, part_size, MEM_RESET | mode.bits | tags, PAGE_READWRITE) ==
                                  reserved + part_size,
                              "title MEM_RESET strips page bits and preserves the host subrange");
                    }
                    check(VirtualQuery(reserved, &info, sizeof(info)) && info.State == MEM_RESERVE &&
                              info.RegionSize == part_size,
                          "committing a subrange leaves the first host page reserved");
                    check(VirtualFree(reserved, 0, MEM_RELEASE), "release the title reservation after subrange commit");
                }
            }
        }

        SetLastError(ERROR_SUCCESS);
        void* const commit_only = alloc(nullptr, kPage64k, MEM_COMMIT, PAGE_READWRITE);
        check(commit_only == nullptr && GetLastError() == ERROR_INVALID_PARAMETER,
              "title commit-only with a null address fails without guest page bits");
        if (commit_only) VirtualFree(commit_only, 0, MEM_RELEASE);

        void* const gfx = alloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, 0x2004);  // PAGE_GRAPHICS_READWRITE
        check(gfx != nullptr, "title VirtualAlloc with protect 0x2004 succeeds");
        if (gfx) {
            MEMORY_BASIC_INFORMATION mbi{};
            check(VirtualQuery(gfx, &mbi, sizeof mbi) && mbi.Protect == PAGE_READWRITE,
                  "protect 0x2004 commits PAGE_READWRITE");
            VirtualFree(gfx, 0, MEM_RELEASE);
        }
        for (const DWORD bits : {kMemGraphics, kMemTitle, kMemGraphics | kMemTitle, DWORD{0}}) {
            void* const p = alloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT | bits, PAGE_READWRITE);
            std::snprintf(what, sizeof what, "%s!VirtualAlloc with type bits %#lx returns memory", dll,
                          static_cast<unsigned long>(bits));
            check(p != nullptr, what);
            if (!p) continue;
            static_cast<volatile uint8_t*>(p)[0] = 0x5a;
            check(static_cast<volatile uint8_t*>(p)[0] == 0x5a, "the allocation is committed and writable");
            check(VirtualFree(p, 0, MEM_RELEASE), "release the allocation");
        }
    }
}

void print_result() {
    if (g_failures == 0) {
        std::printf("xmem_test: OK (%d checks)\n", g_checks);
    } else {
        std::printf("xmem_test: FAILED (%d checks, %d failure%s)\n", g_checks, g_failures,
                    g_failures == 1 ? "" : "s");
    }
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _MSC_VER
    if (argc == 2 && std::strcmp(argv[1], "--physical-trace-first-call") == 0) {
        test_physical_trace_first_call();
        print_result();
        return g_failures ? 1 : 0;
    }
#endif
    if (argc != 1) return 2;
    std::printf("xmem_test: registered xmem.dll shim ABI\n");

    const auto alloc = reinterpret_cast<XMemAllocFn>(rt::shims::find("xmem.dll", "XMemAlloc"));
    const auto free_mem = reinterpret_cast<XMemFreeFn>(rt::shims::find("xmem.dll", "XMemFree"));
    const auto query = reinterpret_cast<XMemVirtualQueryFn>(rt::shims::find("xmem.dll", "XMemVirtualQuery"));
    const auto get_stats =
        reinterpret_cast<XMemGetWorkingSetStatisticsFn>(rt::shims::find("xmem.dll", "XMemGetWorkingSetStatistics"));
    const auto virtual_alloc =
        reinterpret_cast<XMemVirtualAllocFn>(rt::shims::find("xmem.dll", "XMemVirtualAlloc"));
    const auto get_write_watch =
        reinterpret_cast<XMemGetWriteWatchFn>(rt::shims::find("xmem.dll", "XMemGetWriteWatch"));
    const auto reset_write_watch =
        reinterpret_cast<XMemResetWriteWatchFn>(rt::shims::find("xmem.dll", "XMemResetWriteWatch"));
    const auto transfer =
        reinterpret_cast<XMemTransferMemoryFn>(rt::shims::find("xmem.dll", "XMemTransferMemory"));

    check(alloc != nullptr, "registry exposes xmem.dll!XMemAlloc");
    check(free_mem != nullptr, "registry exposes xmem.dll!XMemFree");
    check(query != nullptr, "registry exposes xmem.dll!XMemVirtualQuery");
    check(get_stats != nullptr, "registry exposes xmem.dll!XMemGetWorkingSetStatistics");
    check(virtual_alloc != nullptr, "registry exposes xmem.dll!XMemVirtualAlloc");
    check(get_write_watch != nullptr, "registry exposes xmem.dll!XMemGetWriteWatch");
    check(reset_write_watch != nullptr, "registry exposes xmem.dll!XMemResetWriteWatch");
    check(transfer != nullptr, "registry exposes xmem.dll!XMemTransferMemory");

    const auto alloc_default = reinterpret_cast<XMemAllocFn>(rt::shims::find("xmem.dll", "XMemAllocDefault"));
    const auto free_default = reinterpret_cast<XMemFreeFn>(rt::shims::find("xmem.dll", "XMemFreeDefault"));
    const auto tools_mem =
        reinterpret_cast<XMemIsToolsMemAvailableFn>(rt::shims::find("xmem.dll", "XMemIsToolsMemAvailable"));
    const PhysicalApi physical{
        virtual_alloc,
        reinterpret_cast<XMemAllocatePhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemAllocatePhysicalPages")),
        reinterpret_cast<XMemFreePhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemFreePhysicalPages")),
        reinterpret_cast<XMemMapPhysicalPagesFn>(rt::shims::find("xmem.dll", "XMemMapPhysicalPages")),
        reinterpret_cast<XMemMapPhysicalPagesScatterFn>(rt::shims::find("xmem.dll", "XMemMapPhysicalPagesScatter")),
    };
    check(alloc_default != nullptr, "registry exposes xmem.dll!XMemAllocDefault");
    check(free_default != nullptr, "registry exposes xmem.dll!XMemFreeDefault");
    check(tools_mem != nullptr, "registry exposes xmem.dll!XMemIsToolsMemAvailable");
    check(physical.allocate != nullptr, "registry exposes xmem.dll!XMemAllocatePhysicalPages");
    check(physical.free_pages != nullptr, "registry exposes xmem.dll!XMemFreePhysicalPages");
    check(physical.map != nullptr, "registry exposes xmem.dll!XMemMapPhysicalPages");
    check(physical.scatter != nullptr, "registry exposes xmem.dll!XMemMapPhysicalPagesScatter");

    if (!alloc || !free_mem || !query || !get_stats) {
        print_result();
        return 1;
    }

    test_alloc(alloc, free_mem);
    test_working_set(get_stats);
    test_virtual_query(query);
    if (virtual_alloc && get_write_watch && reset_write_watch) {
        test_virtual_alloc(virtual_alloc, get_write_watch, reset_write_watch);
    }
    test_attribute_trace(alloc, free_mem, virtual_alloc);
    if (transfer) {
        test_transfer_memory(transfer);
        test_transfer_trace(transfer);
    }

    test_title_virtual_alloc();

    if (alloc_default && free_default) test_default_alloc(alloc, free_mem, alloc_default, free_default);
    if (tools_mem) check(tools_mem() == FALSE, "XMemIsToolsMemAvailable returns FALSE (no devkit tools memory)");
    if (virtual_alloc && physical.allocate && physical.free_pages && physical.map && physical.scatter) {
        test_physical_pages(physical);
        test_mapping_protection(physical);
        test_placeholders(physical);
        test_window_carve_and_reuse(physical);
        test_title_virtual_free(physical);
    }

    print_result();
    return g_failures ? 1 : 0;
}
