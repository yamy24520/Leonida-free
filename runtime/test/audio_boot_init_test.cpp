// Replays the title's audio init in the order docs/audio_hal_exe_calls.md records, through the registered shims only:
// every call the title checks must succeed and every output it reads or writes through must be usable, because a
// failure there ends in an int3 with no path without audio. No game files.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shims.h"

namespace {

using CreateHeapFn = HRESULT (*)(uint32_t, uint32_t);
using AllocFn = HRESULT (*)(void**, uint32_t*, uint32_t, uint32_t, uint32_t);
using FreeFn = HRESULT (*)(void*);
using AllocShapeFn = HRESULT (*)(void*);
using ReleaseShapeFn = HRESULT (*)();
using CreateFn = HRESULT (*)(void**);
using Slot0Fn = HRESULT (*)(void*, uint32_t, uint32_t);
using Slot1Fn = HRESULT (*)(void*);
using Slot2Fn = HRESULT (*)(void*, uint32_t, uint32_t, uint32_t, const void*, uint32_t);
using XApuConnectFn = HRESULT(__stdcall*)(void*, void*, void**);
using XApuDisconnectFn = HRESULT(__stdcall*)(void*);
using XApuCountFn = uint32_t(__stdcall*)(void*, uint64_t);
using XDspConnectFn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, void**);
using XDspActivateFn = HRESULT(__stdcall*)(void*, const void*, void**, void**);
using XDspSubmitFn = HRESULT(__stdcall*)(void*, void*, uint32_t*);
using XDspDeactivateFn = HRESULT(__stdcall*)(void*, uint64_t, uint64_t, uint64_t);
using XDspDisconnectFn = HRESULT(__stdcall*)(void*);

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

template <typename T>
T get(const char* dll, const char* name) {
    return reinterpret_cast<T>(rt::shims::find(dll, name));
}

struct ShapeContexts {
    uint32_t count[6];
    void* contexts[6];
};

}  // namespace

int main() {
    const auto create_heap = get<CreateHeapFn>("acphal.dll", "ApuCreateHeap");
    const auto alloc = get<AllocFn>("acphal.dll", "ApuAlloc");
    const auto free_mem = get<FreeFn>("acphal.dll", "ApuFree");
    const auto alloc_shape = get<AllocShapeFn>("acphal.dll", "AcpHalAllocateShapeContexts");
    const auto release_shape = get<ReleaseShapeFn>("acphal.dll", "AcpHalReleaseShapeContexts");
    const auto create = get<CreateFn>("acphal.dll", "AcpHalCreate");
    const auto apu_connect = get<XApuConnectFn>("xapu.dll", "XApuConnect");
    const auto apu_disconnect = get<XApuDisconnectFn>("xapu.dll", "XApuDisconnect");
    const auto apu_count = get<XApuCountFn>("xapu.dll", "XApuGetResultCount");
    const auto dsp_connect = get<XDspConnectFn>("chad.dll", "XDspConnectWithMaximumStreamLimit");
    const auto dsp_activate = get<XDspActivateFn>("chad.dll", "XDspActivate");
    const auto dsp_submit = get<XDspSubmitFn>("chad.dll", "XDspSubmitCommand");
    const auto dsp_deactivate = get<XDspDeactivateFn>("chad.dll", "XDspDeactivate");
    const auto dsp_disconnect = get<XDspDisconnectFn>("chad.dll", "XDspDisconnect");
    if (!create_heap || !alloc || !free_mem || !alloc_shape || !release_shape || !create || !apu_connect ||
        !apu_disconnect || !apu_count || !dsp_connect || !dsp_activate || !dsp_submit || !dsp_deactivate ||
        !dsp_disconnect) {
        std::printf("FAIL an audio import is not registered\n");
        return 1;
    }

    check(create_heap(64u << 20, 0x226e00) >= 0, "ApuCreateHeap is non-negative");

    ShapeContexts shape{};
    shape.count[4] = 350;
    check(alloc_shape(&shape) >= 0, "AcpHalAllocateShapeContexts is non-negative");
    check(shape.contexts[4] != nullptr, "the fifth context pointer (+0x38) is filled");
    if (shape.contexts[4]) std::memset(shape.contexts[4], 0, size_t(shape.count[4]) * 64);

    void* heap_block = nullptr;
    alloc(&heap_block, nullptr, 0x222e00, 0x800, 2);
    check(heap_block != nullptr, "ApuAlloc returns an address the title writes through");
    if (heap_block) std::memset(heap_block, 0, 0x222e00);

    void* hal = nullptr;
    check(create(&hal) >= 0 && hal != nullptr, "AcpHalCreate gives an object");
    void** vtable = hal ? *static_cast<void***>(hal) : nullptr;
    if (vtable) {
        const uint32_t command_flags = 0xc0;
        check(reinterpret_cast<Slot0Fn>(vtable[0])(hal, 0x1000, 0x1000) >= 0, "object slot 0 is non-negative");
        check(reinterpret_cast<Slot2Fn>(vtable[2])(hal, 2, 0xf0f0f0f0, 0xffffffff, &command_flags, 0) >= 0,
              "object slot 2 is non-negative");
    }

    uint8_t global24[24] = {1, 2, 3};
    uint64_t out16[2] = {};
    void* apu = nullptr;
    check(apu_connect(global24, out16, &apu) >= 0 && apu != nullptr, "XApuConnect is non-negative");
    check(global24[0] == 1 && global24[1] == 2 && global24[2] == 3, "XApuConnect leaves the title's 24-byte block alone");
    if (out16[0]) {
        auto* base = reinterpret_cast<uint8_t*>(out16[0]);
        check(base[0x14000 + 127 * 0x1E00 + 0x1E00 - 1] == 0, "the last of 128 stream output slots is readable");
    }
    check(apu_count(apu, 0) == 0, "no result is queued before a command");

    void* dsp = nullptr;
    char caller_buffer[64] = {};
    check(dsp_connect(caller_buffer, 1, 2, 0x80, 1, &dsp) == 0, "XDspConnectWithMaximumStreamLimit is 0");
    void* status = nullptr;
    void* stream = nullptr;
    uint8_t description[32] = {};
    check(dsp_activate(dsp, description, &status, &stream) == 0, "XDspActivate is 0");
    check(status && stream, "both XDspActivate outputs are written");
    uint32_t id = 0;
    check(dsp_submit(stream, description, &id) == 0, "XDspSubmitCommand is 0");
    if (status) {
        uint32_t words[4];
        std::memcpy(words, status, sizeof words);
        check(words[3] == 0 && words[1] == id, "the status wait (dword +0xc is 0, dword +4 is the id) ends at once");
    }

    if (vtable) check(reinterpret_cast<Slot1Fn>(vtable[1])(hal) >= 0, "object slot 1 (shutdown) is non-negative");
    if (vtable) reinterpret_cast<Slot1Fn>(vtable[5])(hal);
    check(dsp_deactivate(dsp, 0, 0, 0) == 0, "XDspDeactivate is 0");
    check(dsp_disconnect(dsp) == 0, "XDspDisconnect is 0");
    check(apu_disconnect(apu) >= 0, "XApuDisconnect is non-negative");
    check(free_mem(heap_block) >= 0, "ApuFree of the ApuAlloc block is non-negative");
    check(release_shape() >= 0, "AcpHalReleaseShapeContexts is non-negative");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
