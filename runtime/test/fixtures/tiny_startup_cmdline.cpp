// Synthetic startup fixture: process-attach callbacks see the command line before CRT argv is configured.
#include <windows.h>
#include <winternl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace {

struct Snapshot {
    int argc;
    bool has_argv;
    bool has_wargv;
    char argv[8][256];
    wchar_t wargv[8][256];
    char line[2048];
    wchar_t wline[2048];
    // Set by main for the native run under Wine only; the DLL gets it with the entry snapshot.
    bool wine_native;
};

Snapshot g_startup{};
bool g_captured = false;

template <typename C, size_t N>
bool copy(C (&to)[N], const C* from) {
    if (!from) return false;
    size_t i = 0;
    for (; i + 1 < N && from[i]; ++i) to[i] = from[i];
    to[i] = 0;
    return from[i] == 0;
}

bool capture(Snapshot& s) {
    s.argc = *__p___argc();
    char** args = *__p___argv();
    wchar_t** wargs = *__p___wargv();
    s.has_argv = args != nullptr;
    s.has_wargv = wargs != nullptr;
    if (s.argc < 0 || s.argc > 8) return false;
    for (int i = 0; i < s.argc; ++i)
        if ((args && !copy(s.argv[i], args[i])) || (wargs && !copy(s.wargv[i], wargs[i]))) return false;
    return (!args || !args[s.argc]) && (!wargs || !wargs[s.argc]) && copy(s.line, GetCommandLineA()) &&
           copy(s.wline, GetCommandLineW());
}

bool matches(const Snapshot& a, const Snapshot& b) {
    // Native UCRT has not configured either argv width at process attach. GetCommandLine is already complete.
    const bool unconfigured = a.argc == 0 && !a.has_argv && !a.has_wargv;
    return (unconfigured || (b.wine_native && a.argc == b.argc)) &&
           std::strcmp(a.line, b.line) == 0 && std::wcscmp(a.wline, b.wline) == 0;
}

}  // namespace

#ifdef STARTUP_CMDLINE_DLL

extern "C" __declspec(dllexport) bool __cdecl startup_cmdline_matches(const Snapshot* entry) {
    return g_captured && matches(g_startup, *entry);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_captured = capture(g_startup);
        if (std::strstr(GetCommandLineA(), "fail-init")) return FALSE;
    }
    return TRUE;
}

#else

extern "C" __declspec(dllimport) bool __cdecl startup_cmdline_matches(const Snapshot* entry);

namespace {
extern "C" IMAGE_DOS_HEADER __ImageBase;

// This image is the process image (run directly), not one gta6rt mapped into its own process. PEB first, because
// under the loader GetModuleHandleW(nullptr) answers for the guest.
bool run_directly() {
    return NtCurrentTeb()->ProcessEnvironmentBlock->Reserved3[1] == static_cast<void*>(&__ImageBase);
}

// Wine's ucrtbase configures argc and both argv widths when it loads, before any DLL attach, where Microsoft's
// configures neither until the startup code asks. So the native reference cannot show the console-like state
// there; it keeps comparing the command lines. The gta6rt run is never relaxed: the loader has to provide it.
bool wine_native_run() {
    if (!run_directly()) return false;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

void NTAPI startup_callback(PVOID, DWORD reason, PVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_captured = capture(g_startup);
}
}  // namespace

#pragma section(".CRT$XLB", read)
extern "C" __declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK g_startup_callback = startup_callback;
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:g_startup_callback")

int main(int argc, char** argv) {
    Snapshot entry{};
    entry.wine_native = wine_native_run();
    bool ok = capture(entry) && g_captured && matches(g_startup, entry) && startup_cmdline_matches(&entry);
    // The wide width stays unconfigured for a narrow main, except under Wine (see wine_native_run).
    ok = ok && argc == entry.argc && entry.has_argv && (entry.wine_native || !entry.has_wargv);
    for (int i = 0; ok && i < argc; ++i) ok = std::strcmp(argv[i], entry.argv[i]) == 0;
    std::printf("tiny_startup_cmdline: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

#endif
