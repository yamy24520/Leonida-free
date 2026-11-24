// Tiny fixture DLL for loader_tls_index0_test (threads): calls a function of the test EXE from its
// DllMain(DLL_THREAD_ATTACH). Loaded before the runtime's TLS helper, so on a new thread this runs before the helper's
// DLL_THREAD_ATTACH, the window in which only the OS has set up the thread's TLS. Ordinary test code, no game data.
#include <windows.h>

namespace {
using Hook = void(__cdecl*)();
Hook volatile g_hook = nullptr;
}  // namespace

extern "C" __declspec(dllexport) void __cdecl tiny_thread_hook_set(Hook hook) { g_hook = hook; }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_THREAD_ATTACH && g_hook) g_hook();
    return TRUE;
}
