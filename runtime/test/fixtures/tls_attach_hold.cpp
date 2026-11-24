// Watches one thread's module-TLS slot from outside the loader, for loader_tls_test. Ordinary test code, not game data.
//
// Windows calls DllMain in load order for DLL_THREAD_ATTACH and in the reverse order for DLL_THREAD_DETACH, and
// loader_tls_test loads this DLL before the loader's gta6rt_tls_host.dll. So this DllMain runs on a new thread after
// Windows has built its TLS array and before the loader's thread event, and on an ending thread after the loader's
// thread event and before Windows frees the array. It records what the slot holds at both points, and it can hold the
// new thread in between (inside its DLL_THREAD_ATTACH) until the test lets it go.
//
// Built without the CRT, so its DllMain does nothing else.
#include <windows.h>
#include <intrin.h>

#ifdef __MSVC_RUNTIME_CHECKS
// Debug builds compile with /RTC1 (the Visual Studio generator's default), whose checks call into the CRT; /NODEFAULTLIB
// leaves these unresolved (LNK1120). The checks have nothing to check here, so they do nothing.
extern "C" {
void __cdecl _RTC_InitBase() {}
void __cdecl _RTC_Shutdown() {}
}
#endif

namespace {

volatile DWORD g_thread = 0;  // the watched thread's id; 0 when there is none
DWORD g_index = 0;            // which entry of TEB->ThreadLocalStoragePointer
HANDLE g_reached = nullptr;   // set when the watched thread is in its DLL_THREAD_ATTACH here; null for no hold
HANDLE g_release = nullptr;   // the hold ends when this is set, or after g_timeout_ms
DWORD g_timeout_ms = 0;
void* volatile g_at_attach = nullptr;
void* volatile g_after_hold = nullptr;
void* volatile g_at_detach = nullptr;

void** module_tls_slots() { return reinterpret_cast<void**>(__readgsqword(0x58)); }

}  // namespace

// Call before the thread runs (create it suspended). One thread at a time.
extern "C" __declspec(dllexport) void __cdecl tls_attach_hold_watch(DWORD thread, DWORD index, HANDLE reached,
                                                                    HANDLE release, DWORD timeout_ms) {
    g_index = index;
    g_reached = reached;
    g_release = release;
    g_timeout_ms = timeout_ms;
    g_at_attach = nullptr;
    g_after_hold = nullptr;
    g_at_detach = nullptr;
    g_thread = thread;
}

// The slot as Windows built it, before the loader's DLL_THREAD_ATTACH event.
extern "C" __declspec(dllexport) void* __cdecl tls_attach_hold_at_attach() { return g_at_attach; }
// The slot when the hold ended, still before the loader's event: what a load() during the hold put there.
extern "C" __declspec(dllexport) void* __cdecl tls_attach_hold_after_hold() { return g_after_hold; }
// The slot after the loader's DLL_THREAD_DETACH event: what Windows was about to free.
extern "C" __declspec(dllexport) void* __cdecl tls_attach_hold_at_detach() { return g_at_detach; }

extern "C" BOOL WINAPI tls_attach_hold_main(HINSTANCE, DWORD reason, LPVOID) {
    if ((reason != DLL_THREAD_ATTACH && reason != DLL_THREAD_DETACH) || GetCurrentThreadId() != g_thread) return TRUE;
    if (!module_tls_slots()) return TRUE;
    if (reason == DLL_THREAD_ATTACH) {
        g_at_attach = module_tls_slots()[g_index];
        if (g_reached) {
            SetEvent(g_reached);
            WaitForSingleObject(g_release, g_timeout_ms);
        }
        g_after_hold = module_tls_slots()[g_index];
    } else {
        void** slots = module_tls_slots();
        g_at_detach = slots[g_index];
        // Windows frees this entry next. If the loader left something else in it, that free would take the process
        // down; the test reports the difference instead, so what Windows built goes back first.
        if (g_at_detach != g_at_attach) slots[g_index] = g_at_attach;
        g_thread = 0;
    }
    return TRUE;
}
