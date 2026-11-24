// Checks the registered xapu.dll and chad.dll shims in the call shapes the executable uses (runtime/shims/xapu_chad.cpp):
// the connections succeed, the APU's shared buffer is zeroed and writable at the offsets the title reaches, commands
// are accepted, an open and a close each give one result and nothing else does, XDspActivate hands out zeroed status
// and stream memory, and both disconnect.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shims.h"

namespace {

using XApuConnectFn = HRESULT(__stdcall*)(void*, void*, void**);
using XApuDisconnectFn = HRESULT(__stdcall*)(void*);
using XApuEnqueueCommandFn = HRESULT(__stdcall*)(void*, const uint32_t*, uint64_t);
using XApuGetResultCountFn = uint32_t(__stdcall*)(void*, uint64_t);
using XApuDequeueResultFn = HRESULT(__stdcall*)(void*, void*);
using XDspConnectFn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, void**);
using XDspActivateFn = HRESULT(__stdcall*)(void*, const void*, void**, void**);
using XDspDeactivateFn = HRESULT(__stdcall*)(void*, uint64_t, uint64_t, uint64_t);
using XDspDisconnectFn = HRESULT(__stdcall*)(void*);
using XDspSubmitCommandFn = HRESULT(__stdcall*)(void*, void*, uint32_t*);

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

template <typename T>
T get(const char* dll, const char* name) {
    void* fn = rt::shims::find(dll, name);
    check(fn != nullptr, name);
    return reinterpret_cast<T>(fn);
}

}  // namespace

int main() {
    std::printf("xapu_chad_test: registered xapu.dll and chad.dll shims\n");
    const auto connect = get<XApuConnectFn>("xapu.dll", "XApuConnect");
    const auto disconnect = get<XApuDisconnectFn>("xapu.dll", "XApuDisconnect");
    const auto enqueue = get<XApuEnqueueCommandFn>("xapu.dll", "XApuEnqueueCommand");
    const auto count = get<XApuGetResultCountFn>("xapu.dll", "XApuGetResultCount");
    const auto dequeue = get<XApuDequeueResultFn>("xapu.dll", "XApuDequeueResult");
    const auto dsp_connect = get<XDspConnectFn>("chad.dll", "XDspConnectWithMaximumStreamLimit");
    const auto dsp_activate = get<XDspActivateFn>("chad.dll", "XDspActivate");
    const auto dsp_deactivate = get<XDspDeactivateFn>("chad.dll", "XDspDeactivate");
    const auto dsp_disconnect = get<XDspDisconnectFn>("chad.dll", "XDspDisconnect");
    const auto dsp_submit = get<XDspSubmitCommandFn>("chad.dll", "XDspSubmitCommand");
    check(rt::shims::owns_dll("xapu.dll") && rt::shims::owns_dll("chad.dll"), "the shims own xapu.dll and chad.dll");
    if (g_failures) return 1;

    // XApuConnect(block24*, out16*, handle*), success read as SUCCEEDED.
    uint8_t out24[24];
    uint64_t out16[2] = {};
    std::memset(out24, 0xcc, sizeof out24);
    void* apu = nullptr;
    check(SUCCEEDED(connect(out24, out16, &apu)) && apu, "XApuConnect succeeds with a handle");
    check(out24[0] == 0xcc && out24[23] == 0xcc, "XApuConnect leaves the title's 24-byte block alone");
    auto* base = reinterpret_cast<uint8_t*>(out16[0]);
    check(base != nullptr && out16[1] >= 0x14000 + 128 * 0x1E00, "XApuConnect returns the shared buffer and its size");
    if (base) {
        const size_t last_output = 0x14000 + 127 * 0x1E00 + 0x1E00 - 1;  // the end of slot 127's output
        check(base[0] == 0 && base[last_output] == 0, "the shared buffer is zeroed");
        base[127 * 0x280] = 1;
        base[last_output] = 1;
        check(base[127 * 0x280] == 1 && base[last_output] == 1, "the shared buffer is writable at slot 127");
    }
    check(connect(out24, out16, nullptr) == E_INVALIDARG, "XApuConnect without a handle pointer fails");

    // Command types 1, 2 and 3 as the title builds them; the third argument is 0 at every site.
    for (uint32_t type = 1; type <= 3; ++type) {
        uint32_t command[16] = {type, 5};
        check(enqueue(apu, command, 0) == S_OK, "XApuEnqueueCommand accepts a command");
    }
    check(count(apu, 0) == 2, "the open and the close each queued a result, the type 3 command none");
    for (uint32_t type = 1; type <= 2; ++type) {
        uint32_t queued[0x50 / 4] = {};
        check(dequeue(apu, queued) == S_OK && queued[0] == type && queued[1] == 5 && queued[2] == 0 && queued[13] == 0,
              "XApuDequeueResult returns the type and slot and leaves the rest zeroed");
    }
    check(count(apu, 0) == 0, "XApuGetResultCount is 0 again");
    uint8_t result[0x50] = {};
    check(FAILED(dequeue(apu, result)), "XApuDequeueResult on the empty queue fails");
    check(disconnect(apu) == S_OK, "XApuDisconnect succeeds");

    // XDspConnectWithMaximumStreamLimit(buffer, u32, u32, 0x80, 1, handle*), success read as a return of 0.
    alignas(64) static uint8_t dsp_buffer[4096];
    void* dsp = nullptr;
    check(dsp_connect(dsp_buffer, 0, 0, 0x80, 1, &dsp) == 0 && dsp, "XDspConnectWithMaximumStreamLimit returns 0 and a handle");
    const uint8_t description[0x1c] = {};
    uint32_t* status = nullptr;
    void* stream = nullptr;
    check(dsp_activate(dsp, description, reinterpret_cast<void**>(&status), &stream) == 0, "XDspActivate returns 0");
    check(status && stream && static_cast<void*>(status) != stream, "XDspActivate writes both outputs");
    uint32_t id = 0;
    check(dsp_submit(stream, dsp_buffer, &id) == 0, "XDspSubmitCommand returns 0");
    // What the title's wait reads: no error at +0xc and the submitted id at +4.
    check(status && status[3] == 0 && status[1] == id, "the status block reports the submitted command as done");
    check(dsp_deactivate(dsp, 0, 0, 0) == 0, "XDspDeactivate returns 0");
    check(dsp_disconnect(dsp) == 0, "XDspDisconnect returns 0");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
