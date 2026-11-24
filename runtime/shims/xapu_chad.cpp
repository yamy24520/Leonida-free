// xapu.dll and chad.dll: the console's audio processing unit (APU) and audio DSP command queues. No PC has either,
// so these give the title connections whose calls succeed, a zeroed shared buffer, a result for each stream open and
// close, and zeroed DSP status: the audio the APU would render stays silent, and nothing the title submits is processed.
//
// Read from the executable's call sites (counts in docs/runtime_imports.md), statically, never run:
//   XApuConnect(block24*, out16*, handle*)    1 site; success is SUCCEEDED(hr). The first qword of the 16-byte
//                                             output is read later as the base of a CPU-visible buffer: per-stream
//                                             input at base + slot * 0x280 and output at base + 0x14000 +
//                                             slot * 0x1E00. The first argument is a 24-byte global of the title
//                                             that other title code also addresses; whether it is an input or an
//                                             output is not known, so it is left untouched.
//   XApuEnqueueCommand(handle, cmd*, 0)       4 sites; the return value is never read. cmd's first dword is a type
//                                             (1, 2 and 3 seen), the second a stream slot.
//   XApuGetResultCount(handle, 0)             1 site; 0 skips the loop that dequeues.
//   XApuDequeueResult(handle, out*)           1 site, called count times into a zeroed 0x50-byte local; a negative
//                                             HRESULT skips that result. The loop reads a type at +0, a stream slot
//                                             at +4 and a dword at +0x34. Its per-slot state machine takes a type 1
//                                             result as "opened" and returns the slot to the title's free list only
//                                             on a type 2 result, after the title enqueued a type 2 command.
//   XApuDisconnect(handle, 0x20)              1 site; the return value is never read. The constant second
//                                             argument is not declared here: nothing reads it.
//   XDspConnectWithMaximumStreamLimit(buffer, u32, u32, 0x80, 1, handle*)   1 site; success is a return of 0. The
//                                             buffer comes from the title's XMemAlloc and stays the title's.
//   XDspActivate(handle, desc*, out*, out*)   1 site; success is a return of 0, else the caller returns -1. The two
//                                             outputs are fields of the caller's object. The title later reads
//                                             dwords at +4 and +0xc through one of them with no null check, and
//                                             passes the other to XDspSubmitCommand.
//   XDspSubmitCommand(stream, cmd*, id*)      1 site; success is a return of 0. The title zeroes *id before the call
//                                             and afterwards waits until the status dword at +0xc is 0 and the one
//                                             at +4 equals *id.
//   XDspDeactivate, XDspDisconnect            1 site each, tail calls: the caller returns what they return.
//
// HYPOTHESIS: the shared buffer is 0x14000 + 128 * 0x1E00 bytes. 128 slots is what fits 0x14000 at the 0x280 input
// stride, and the DSP connect passes 0x80 as its stream limit; no call site shows a bound. The 16-byte output is
// written as {base, size}; only the base is measured.
// HYPOTHESIS: a type 1 (open) or type 2 (close) command produces one result of the same type for the same slot; the
// title's state machine fits that, the console is not measured. Without the close result the title's 256 stream ids
// run out. Only the two dwords are written; other command types are accepted with S_OK and produce no result.
// HYPOTHESIS: XDspActivate's outputs point at zeroed memory owned by the connection. XDspSubmitCommand leaves *id at
// the title's 0, so the zeroed status reads as "command 0 done, no error" and the title does not wait.
#include <windows.h>
#include <intrin.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <set>
#include <string>

#include "shims.h"
#include "startup_trace.h"

namespace {

constexpr size_t kApuBufferSize = 0x14000 + 128 * 0x1E00;

// shortcut: results past this cap are dropped (each leaks one stream id); raise it if a title that never dequeues
// must still work.
constexpr size_t kMaxResults = 4096;

struct ApuResult {
    uint32_t type;
    uint32_t slot;
};

struct ApuConnection {
    void* buffer;
    std::mutex mutex;
    std::deque<ApuResult> results;
};

struct DspConnection {
    uint8_t status[256];
    uint8_t stream[256];
};

// gta6rt --trace FILE: the first call of each function and the first XApuEnqueueCommand of each command type, with
// the caller, so the first boot shows what the title sends without a line per call.
void note(const char* api, uint64_t value, const void* caller) {
    if (!rt::startup_trace::enabled()) return;
    static std::mutex mutex;
    static std::set<std::pair<std::string, uint64_t>> seen;
    {
        const std::lock_guard lock(mutex);
        if (!seen.insert({api, value}).second) return;
    }
    rt::startup_trace::event(
        "audio_queue", std::string("\"api\":") + rt::startup_trace::quote(api) +
                           ",\"value\":" + rt::startup_trace::quote(rt::startup_trace::hex(value)) + ",\"caller\":" +
                           rt::startup_trace::quote(rt::startup_trace::where(reinterpret_cast<uint64_t>(caller))));
}

HRESULT __stdcall XApuConnect(void* block24, void* out16, void** handle) {
    note("XApuConnect", 0, _ReturnAddress());
    if (!handle) return E_INVALIDARG;
    *handle = nullptr;
    void* buffer = VirtualAlloc(nullptr, kApuBufferSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);  // zeroed
    if (!buffer) return E_OUTOFMEMORY;
    auto* c = new (std::nothrow) ApuConnection;  // a throwing new would unwind into the title
    if (!c) {
        VirtualFree(buffer, 0, MEM_RELEASE);
        return E_OUTOFMEMORY;
    }
    c->buffer = buffer;
    *handle = c;
    (void)block24;
    if (out16) {
        const uint64_t info[2] = {reinterpret_cast<uint64_t>(buffer), kApuBufferSize};
        std::memcpy(out16, info, sizeof info);
    }
    return S_OK;
}

HRESULT __stdcall XApuDisconnect(void* handle) {
    note("XApuDisconnect", 0, _ReturnAddress());
    auto* c = static_cast<ApuConnection*>(handle);
    if (!c) return E_INVALIDARG;
    VirtualFree(c->buffer, 0, MEM_RELEASE);
    delete c;
    return S_OK;
}

HRESULT __stdcall XApuEnqueueCommand(void* handle, const uint32_t* command, uint64_t flags) {
    note("XApuEnqueueCommand", command ? command[0] : ~0ull, _ReturnAddress());
    (void)flags;
    auto* c = static_cast<ApuConnection*>(handle);
    if (!c || !command) return E_INVALIDARG;
    if (command[0] == 1 || command[0] == 2) {
        try {
            const std::lock_guard lock(c->mutex);
            if (c->results.size() < kMaxResults) c->results.push_back({command[0], command[1]});
        } catch (...) {  // out of memory: the result is lost, nothing may unwind into the title
        }
    }
    return S_OK;
}

uint32_t __stdcall XApuGetResultCount(void* handle, uint64_t flags) {
    note("XApuGetResultCount", 0, _ReturnAddress());
    (void)flags;
    auto* c = static_cast<ApuConnection*>(handle);
    if (!c) return 0;
    const std::lock_guard lock(c->mutex);
    return static_cast<uint32_t>(c->results.size());
}

HRESULT __stdcall XApuDequeueResult(void* handle, void* result) {
    note("XApuDequeueResult", 0, _ReturnAddress());
    auto* c = static_cast<ApuConnection*>(handle);
    if (!c || !result) return E_INVALIDARG;
    const std::lock_guard lock(c->mutex);
    if (c->results.empty()) return E_FAIL;
    std::memcpy(result, &c->results.front(), sizeof(ApuResult));  // the rest stays as the caller zeroed it
    c->results.pop_front();
    return S_OK;
}

HRESULT __stdcall XDspConnectWithMaximumStreamLimit(void* buffer, uint32_t a, uint32_t b, uint32_t max_streams,
                                                    uint32_t flags, void** handle) {
    note("XDspConnectWithMaximumStreamLimit", max_streams, _ReturnAddress());
    (void)buffer;
    (void)a;
    (void)b;
    (void)flags;
    if (!handle) return E_INVALIDARG;
    *handle = new (std::nothrow) DspConnection{};
    return *handle ? S_OK : E_OUTOFMEMORY;
}

HRESULT __stdcall XDspDisconnect(void* handle) {
    note("XDspDisconnect", 0, _ReturnAddress());
    delete static_cast<DspConnection*>(handle);
    return S_OK;
}

HRESULT __stdcall XDspActivate(void* handle, const void* description, void** status, void** stream) {
    note("XDspActivate", 0, _ReturnAddress());
    (void)description;
    auto* c = static_cast<DspConnection*>(handle);
    if (!c) return E_INVALIDARG;
    if (status) *status = c->status;
    if (stream) *stream = c->stream;
    return S_OK;
}

HRESULT __stdcall XDspDeactivate(void* handle, uint64_t, uint64_t, uint64_t) {
    note("XDspDeactivate", 0, _ReturnAddress());
    (void)handle;
    return S_OK;
}

// *id stays the 0 the title wrote, which the zeroed status block already reports as done.
HRESULT __stdcall XDspSubmitCommand(void*, void*, uint32_t*) {
    note("XDspSubmitCommand", 0, _ReturnAddress());
    return S_OK;
}

}  // namespace

SHIM_TABLE(xapu,
    {"xapu.dll", "XApuConnect", reinterpret_cast<void*>(&XApuConnect)},
    {"xapu.dll", "XApuDequeueResult", reinterpret_cast<void*>(&XApuDequeueResult)},
    {"xapu.dll", "XApuDisconnect", reinterpret_cast<void*>(&XApuDisconnect)},
    {"xapu.dll", "XApuEnqueueCommand", reinterpret_cast<void*>(&XApuEnqueueCommand)},
    {"xapu.dll", "XApuGetResultCount", reinterpret_cast<void*>(&XApuGetResultCount)},
);

SHIM_TABLE(chad,
    {"chad.dll", "XDspActivate", reinterpret_cast<void*>(&XDspActivate)},
    {"chad.dll", "XDspConnectWithMaximumStreamLimit", reinterpret_cast<void*>(&XDspConnectWithMaximumStreamLimit)},
    {"chad.dll", "XDspDeactivate", reinterpret_cast<void*>(&XDspDeactivate)},
    {"chad.dll", "XDspDisconnect", reinterpret_cast<void*>(&XDspDisconnect)},
    {"chad.dll", "XDspSubmitCommand", reinterpret_cast<void*>(&XDspSubmitCommand)},
);
