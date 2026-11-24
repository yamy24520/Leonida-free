// Minimal TLS anchor DLL for the native loader.
//
// Loading this image makes the Windows loader assign it a per-module TLS index and allocate a block for that index in
// every thread's TEB. Loader::initialize_tls reuses that reserved index for the manually mapped image, so
// __declspec(thread) access in mapped code reads a valid TEB slot. The anchor itself is never used; the loader saves
// and restores the helper's own block, so the helper's CRT TLS state is not disturbed.
//
// One more clone, leonida_tls_host_<pid>_home.dll, is made at process start by reserve_tls_index0: its TLS directory
// is pointed at gta6rt_tls_home_template below, filled with a copy of the runtime EXE's template, and the runtime EXE
// moves its TLS from index 0 (which Windows gave it) to that clone's index, so a mapped executable can take index 0.
// That clone stays loaded until the process ends, and its slot then holds the runtime's live TLS, so nothing in this
// DLL may read or write its own TLS (gta6rt_tls_host_keep is never called).
//
// The Windows loader also calls this DLL's DllMain with DLL_THREAD_ATTACH on every new thread (after it has built that
// thread's TLS array) and with DLL_THREAD_DETACH when a thread exits (before it frees the array). The loader registers
// a callback here so it can give each new thread its own copy of the mapped image's TLS, and put this DLL's block back
// before the thread exits. DllMain itself must not touch TLS: this DLL's slot is the one being swapped.
#include <windows.h>

__declspec(thread) unsigned char g_tls_host_anchor = 0;

namespace {

using ThreadEventFn = void(__cdecl*)(DWORD reason);

// A plain global, not TLS (see above). Written once by the loader, read on every thread event.
void* volatile g_thread_event = nullptr;

}  // namespace

// Never called. The export keeps the TLS variable (and therefore the image's TLS directory) from being dropped by
// the linker without making DllMain touch TLS during thread attach/detach.
extern "C" __declspec(dllexport) void gta6rt_tls_host_keep() { g_tls_host_anchor = 1; }

// The home clone's TLS template (pe_loader.cpp, make_home_clone_image): zeros here, the runtime EXE's template in the
// clone's file. 16 KiB: gta6rt.exe's own TLS template was 1398 bytes when measured; the loader refuses the move when
// the EXE's TLS does not fit. Each thread gets a block of the EXE's size, not of this array's. Clones of this DLL for
// mapped DLLs keep the 514-byte TLS directory and do not use the array.
extern "C" __declspec(dllexport) const unsigned char gta6rt_tls_home_template[16384] = {};
extern "C" __declspec(dllexport) const unsigned int gta6rt_tls_home_template_size = sizeof(gta6rt_tls_home_template);

// Called by the loader. `fn` runs inside DllMain (under the Windows loader lock) for DLL_THREAD_ATTACH and
// DLL_THREAD_DETACH; nullptr stops the calls.
extern "C" __declspec(dllexport) void gta6rt_tls_host_set_thread_event(ThreadEventFn fn) {
    InterlockedExchangePointer(&g_thread_event, reinterpret_cast<void*>(fn));
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_THREAD_ATTACH || reason == DLL_THREAD_DETACH) {
        void* fn = InterlockedCompareExchangePointer(&g_thread_event, nullptr, nullptr);
        if (fn) reinterpret_cast<ThreadEventFn>(fn)(reason);
    }
    return TRUE;
}
