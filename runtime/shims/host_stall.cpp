// Sleep for mapped images: the host's Sleep, held back while the layer does host-only work (src/host_stall.h).
//
// The retail executable imports Sleep from api-ms-win-core-synch-l1-2-0.dll, and its startup hang check is a loop
// around Sleep(1000) (docs/retail_boot_chain.md, "The startup hang check"). Only Sleep is served: SleepEx, the
// condition-variable sleeps and the waits with a timeout stay the host's, and so does every clock.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>

#include "../src/host_stall.h"
#include "shims.h"

namespace {

void WINAPI guest_sleep(DWORD milliseconds) {
    // A yield and an endless sleep have no duration to stretch.
    if (milliseconds == 0 || milliseconds == INFINITE) {
        Sleep(milliseconds);
        return;
    }
    auto& gate = rt::host_stall::gate();
    const rt::host_stall::Nanoseconds before = gate.mark();
    Sleep(milliseconds);
    const rt::host_stall::Nanoseconds held = gate.hold(before);
    // One line for the first hold a title's own timing could notice; the count goes on in silence.
    static std::atomic<bool> logged{false};
    if (held >= std::chrono::seconds(1) && !logged.exchange(true))
        std::fprintf(stderr,
                     "[rt] host stall: a guest Sleep(%lu) returned %lld ms late, held while the layer did host-only "
                     "work (logged once)\n",
                     static_cast<unsigned long>(milliseconds),
                     static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(held).count()));
}

}  // namespace

#define HOST_STALL_SLEEP(dll) {dll, "Sleep", reinterpret_cast<void*>(&guest_sleep)}

SHIM_TABLE(host_stall,
    HOST_STALL_SLEEP("kernel32.dll"),
    HOST_STALL_SLEEP("kernelbase.dll"),
    HOST_STALL_SLEEP("api-ms-win-core-synch-l1-2-0.dll"),
    HOST_STALL_SLEEP("api-ms-win-core-synch-l1-2-1.dll"),
);
