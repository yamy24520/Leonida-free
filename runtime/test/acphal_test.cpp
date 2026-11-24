// Checks the registered AcpHal.dll shim in the shapes the game's call sites use (runtime/shims/acphal.cpp).
// Every function is resolved through rt::shims::find, and the AcpHal object is called through its raw vtable slots,
// as the title does.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shims.h"

namespace {

using CreateHeapFn = HRESULT (*)(uint32_t, uint32_t);
using AllocFn = HRESULT (*)(void**, uint32_t*, uint32_t, uint32_t, uint32_t);
using FreeFn = HRESULT (*)(void*);
using ValidFn = BOOL (*)(const void*, uint32_t);
using MapVaFn = uint32_t (*)(const void*);
using MapApuFn = void* (*)(uint32_t);
using AllocShapeFn = HRESULT (*)(void*);
using ReleaseShapeFn = HRESULT (*)();
using CreateFn = HRESULT (*)(void**);
using Slot0Fn = HRESULT (*)(void*, uint32_t, uint32_t);
using Slot1Fn = HRESULT (*)(void*);
using Slot2Fn = HRESULT (*)(void*, uint32_t, uint32_t, uint32_t, const void*, uint32_t);

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

template <typename T>
T get(const char* name) {
    return reinterpret_cast<T>(rt::shims::find("acphal.dll", name));
}

}  // namespace

int main() {
    check(rt::shims::owns_dll("acphal.dll"), "the registry owns acphal.dll");
    const auto create_heap = get<CreateHeapFn>("ApuCreateHeap");
    const auto alloc = get<AllocFn>("ApuAlloc");
    const auto free_mem = get<FreeFn>("ApuFree");
    const auto valid = get<ValidFn>("ApuIsVirtualAddressValid");
    const auto map_va = get<MapVaFn>("ApuMapVirtualAddress");
    const auto map_apu = get<MapApuFn>("ApuMapApuAddress");
    const auto alloc_shape = get<AllocShapeFn>("AcpHalAllocateShapeContexts");
    const auto release_shape = get<ReleaseShapeFn>("AcpHalReleaseShapeContexts");
    const auto create = get<CreateFn>("AcpHalCreate");
    if (!create_heap || !alloc || !free_mem || !valid || !map_va || !map_apu || !alloc_shape || !release_shape ||
        !create) {
        std::printf("FAIL an AcpHal.dll export is not registered\n");
        return 1;
    }

    void* none = nullptr;
    check(alloc(&none, nullptr, 64, 64, 0) < 0 && !none, "ApuAlloc before ApuCreateHeap fails with *va NULL");

    // Boot order at the call sites: the heap (MiB << 20, then a byte count), then one aligned block, flags 2.
    check(create_heap(1u << 20, 0x226E00) == S_OK, "ApuCreateHeap(1 MiB, 0x226E00) succeeds");
    check(create_heap(1u << 20, 0) < 0, "a second ApuCreateHeap fails");
    void* big = nullptr;
    uint32_t big_apu = 0;
    check(alloc(&big, &big_apu, 0x222E00, 0x800, 2) == S_OK && big, "ApuAlloc(0x222E00, align 0x800) succeeds");
    check((reinterpret_cast<uintptr_t>(big) & 0x7FF) == 0 && (big_apu & 0x7FF) == 0, "the block is 0x800-aligned");
    const auto* bytes = static_cast<const uint8_t*>(big);
    check(bytes && bytes[0] == 0 && bytes[0x222E00 - 1] == 0, "the block is zeroed");

    // Context setup: validity with the three measured alignments, then the 32-bit mapping and back.
    uint8_t* inner = static_cast<uint8_t*>(big) + 0x1900;
    check(valid(inner, 0x40) && valid(inner, 0x80) && valid(inner, 0x800) == FALSE,
          "ApuIsVirtualAddressValid checks the alignment");
    check(valid(big, 3) == FALSE, "ApuIsVirtualAddressValid rejects non-power-of-two alignment");
    check(valid(big, 0) && valid(big, 0x800), "zero and power-of-two alignments remain valid");
    check(valid(static_cast<uint8_t*>(big) + 0x222E00, 0x40) == FALSE, "an address past the block is not valid");
    int local = 0;
    check(valid(&local, 1) == FALSE && map_va(&local) == 0, "memory outside the heap is not valid and maps to 0");
    check(map_va(nullptr) == 0 && map_apu(0) == nullptr, "NULL maps to 0 and 0 to NULL");
    const uint32_t apu = map_va(inner);
    check(apu == big_apu + 0x1900, "ApuMapVirtualAddress agrees with ApuAlloc's APU address");
    check(map_apu(apu) == inner, "ApuMapApuAddress inverts ApuMapVirtualAddress");

    void* small = nullptr;
    check(alloc(&small, nullptr, 0x100, 0x40, 0) == S_OK && small && small != big, "a second block fits after it");
    check(alloc(&none, nullptr, 0x400000, 0x40, 0) < 0, "a block larger than the heap fails");
    check(alloc(&none, nullptr, 0x100, 3, 0) < 0, "a non-power-of-two alignment fails");
    check(free_mem(big) == S_OK && free_mem(big) < 0, "ApuFree releases a block once");
    void* again = nullptr;
    check(alloc(&again, nullptr, 0x1000, 0x800, 2) == S_OK && again == big, "freed space is reused");
    check(free_mem(again) == S_OK && free_mem(small) == S_OK, "ApuFree of the remaining blocks");

    void* highly_aligned = nullptr;
    check(alloc(&highly_aligned, nullptr, 0x100, 0x80000000u, 0) == S_OK && highly_aligned,
          "ApuAlloc accepts alignment above 64 KiB");
    check(valid(highly_aligned, 0x80000000u), "validity uses the same offset alignment as ApuAlloc");
    check(free_mem(highly_aligned) == S_OK, "ApuFree releases the highly aligned block");

    // Shape contexts: 6 counts then 6 pointers; the caller sets the fifth count and clears count * 64 bytes.
    struct {
        uint32_t count[6];
        void* contexts[6];
    } shape{};
    shape.count[4] = 350;
    check(alloc_shape(&shape) == S_OK, "AcpHalAllocateShapeContexts succeeds");
    check(reinterpret_cast<uint8_t*>(&shape.contexts[4]) - reinterpret_cast<uint8_t*>(&shape) == 0x38 &&
              shape.contexts[4] && !shape.contexts[0] && !shape.contexts[5],
          "the fifth pointer at +0x38 is set, zero counts stay NULL");
    if (shape.contexts[4]) std::memset(shape.contexts[4], 0xA5, 350 * 64);
    check(release_shape() == S_OK, "AcpHalReleaseShapeContexts succeeds");
    // A caller whose block ends after the fifth pointer (+0x40): the sixth pointer's bytes are not written.
    shape = {};
    shape.count[4] = 1;
    std::memset(&shape.contexts[5], 0x5A, sizeof(void*));
    void* guard;
    std::memset(&guard, 0x5A, sizeof guard);
    check(alloc_shape(&shape) == S_OK && shape.contexts[4] && shape.contexts[5] == guard,
          "a zero count leaves its pointer untouched");
    release_shape();

    // The object: slot 0 (0x1000, 0x1000), slot 2 as the boot call passes it, slot 1, slot 5 last.
    void* hal = nullptr;
    check(create(&hal) == S_OK && hal, "AcpHalCreate returns an object");
    if (hal) {
        void** vtbl = *static_cast<void***>(hal);
        const uint32_t arg = 0xC0;
        check(reinterpret_cast<Slot0Fn>(vtbl[0])(hal, 0x1000, 0x1000) == S_OK, "slot 0 succeeds");
        check(reinterpret_cast<Slot2Fn>(vtbl[2])(hal, 2, 0xF0F0F0F0, 0xFFFFFFFF, &arg, 0) == S_OK,
              "slot 2 accepts a command");
        check(reinterpret_cast<Slot1Fn>(vtbl[3])(hal) == 0 && reinterpret_cast<Slot1Fn>(vtbl[4])(hal) == 0,
              "slots 3 and 4 answer 0 (no message waiting)");
        check(reinterpret_cast<Slot1Fn>(vtbl[1])(hal) == S_OK, "slot 1 succeeds");
        check(reinterpret_cast<Slot1Fn>(vtbl[5])(hal) == S_OK, "slot 5 releases the object");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
