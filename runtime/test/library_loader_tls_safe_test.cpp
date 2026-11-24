// Regression test for TLS-slot-0 safety in library_loader.cpp and d3d12_trace.h.
//
// When a mapped guest executable owns static TLS index 0, TEB->ThreadLocalStoragePointer[0] holds guest TLS data
// rather than the host executable's _Init_thread_epoch. Under MSVC / clang-cl thread-safe static initialization,
// any dynamically initialized function-local static reads _Init_thread_epoch from slot 0; a zeroed guest TLS block
// makes the guard skip initialization and dereference a null pointer (or an uninitialized mutex).
//
// This synthetic test swaps TEB->ThreadLocalStoragePointer[0] on worker threads to a zeroed page before the first
// call into library_loader or rt::d3d12_trace::record, then exercises single-threaded and concurrent calls across
// both. No game files and no GPU are involved.
#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "d3d12_trace.h"
#include "shims.h"

namespace {

// Ensure this test executable has a static .tls directory so TEB->ThreadLocalStoragePointer[0] is allocated.
thread_local uint32_t g_tls_slot0_anchor = 0x13572468u;

uint32_t WINAPI probe_export() { return 0x5A5A1234u; }

std::atomic<int> g_checks{0};
std::atomic<int> g_failures{0};

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
        std::fflush(stdout);
    }
}

void** module_tls_slots() noexcept { return reinterpret_cast<void**>(__readgsqword(0x58)); }

struct ScopedTlsSlot0Poison {
    void** slots = nullptr;
    void* saved = nullptr;

    explicit ScopedTlsSlot0Poison(void* scratch_page) noexcept {
        slots = module_tls_slots();
        if (slots) {
            saved = slots[0];
            slots[0] = scratch_page;
        }
    }

    ~ScopedTlsSlot0Poison() noexcept {
        if (slots) slots[0] = saved;
    }

    ScopedTlsSlot0Poison(const ScopedTlsSlot0Poison&) = delete;
    ScopedTlsSlot0Poison& operator=(const ScopedTlsSlot0Poison&) = delete;
};

struct ShimApi {
    HMODULE(WINAPI* load_library_a)(LPCSTR) = nullptr;
    HMODULE(WINAPI* load_library_w)(LPCWSTR) = nullptr;
    HMODULE(WINAPI* get_module_handle_a)(LPCSTR) = nullptr;
    HMODULE(WINAPI* get_module_handle_w)(LPCWSTR) = nullptr;
    FARPROC(WINAPI* get_proc_address)(HMODULE, LPCSTR) = nullptr;
    BOOL(WINAPI* free_library)(HMODULE) = nullptr;
    BOOL(WINAPI* disable_thread_calls)(HMODULE) = nullptr;
};

struct WorkerArgs {
    const ShimApi* api = nullptr;
    void* zero_page = nullptr;
    HMODULE kernel32 = nullptr;
    HMODULE expected_pseudo = nullptr;
    int thread_id = 0;
    int iterations = 0;
};

DWORD WINAPI first_use_worker(LPVOID param) {
    auto* args = static_cast<WorkerArgs*>(param);
    // Touch thread_local before poisoning so the OS allocates this thread's slot 0 array.
    if (g_tls_slot0_anchor != 0x13572468u) return 1;

    ScopedTlsSlot0Poison poison(args->zero_page);
    if (!poison.slots) {
        check(false, "worker thread has TEB->ThreadLocalStoragePointer");
        return 1;
    }

    // First-ever calls into library_loader.cpp with TLS slot 0 poisoned.
    const HMODULE h_mod_a = args->api->get_module_handle_a("leonida_tls_probe.dll");
    check(h_mod_a != nullptr, "GetModuleHandleA returns pseudo-handle under poisoned TLS slot 0");
    args->expected_pseudo = h_mod_a;

    const HMODULE h_load_a = args->api->load_library_a("leonida_tls_probe");
    check(h_load_a == h_mod_a, "LoadLibraryA returns same pseudo-handle under poisoned TLS slot 0");

    const HMODULE h_load_w = args->api->load_library_w(L"C:\\fake\\LEONIDA_TLS_PROBE.DLL");
    check(h_load_w == h_mod_a, "LoadLibraryW returns same pseudo-handle under poisoned TLS slot 0");

    const HMODULE h_mod_w = args->api->get_module_handle_w(L"leonida_tls_probe.dll");
    check(h_mod_w == h_mod_a, "GetModuleHandleW returns same pseudo-handle under poisoned TLS slot 0");

    const FARPROC proc = args->api->get_proc_address(h_mod_a, "ProbeExport");
    check(reinterpret_cast<const void*>(proc) == reinterpret_cast<const void*>(&probe_export),
          "GetProcAddress resolves served shim export under poisoned TLS slot 0");
    if (proc) {
        using ProbeFn = uint32_t(WINAPI*)();
        check(reinterpret_cast<ProbeFn>(reinterpret_cast<void*>(proc))() == 0x5A5A1234u,
              "resolved shim export executes cleanly");
    }

    // Exercises get_proc_address unserved-name dedup lock + set.
    SetLastError(0);
    const FARPROC missing1 = args->api->get_proc_address(h_mod_a, "MissingProbeExport");
    const DWORD err1 = GetLastError();
    check(missing1 == nullptr && err1 == ERROR_PROC_NOT_FOUND,
          "GetProcAddress on unserved owned export returns ERROR_PROC_NOT_FOUND under poisoned TLS slot 0");

    SetLastError(0);
    const FARPROC missing2 = args->api->get_proc_address(h_mod_a, "MissingProbeExport");
    const DWORD err2 = GetLastError();
    check(missing2 == nullptr && err2 == ERROR_PROC_NOT_FOUND,
          "repeated GetProcAddress on unserved owned export remains safe under poisoned TLS slot 0");

    // Exercises host_module_name() cache population and lookup.
    const FARPROC shimmed_gmh = args->api->get_proc_address(args->kernel32, "GetModuleHandleA");
    check(reinterpret_cast<const void*>(shimmed_gmh) == reinterpret_cast<const void*>(args->api->get_module_handle_a),
          "GetProcAddress on host kernel32 resolves shimmed export under poisoned TLS slot 0");

    const FARPROC host_pid = args->api->get_proc_address(args->kernel32, "GetCurrentProcessId");
    check(host_pid != nullptr, "GetProcAddress falls back to host export under poisoned TLS slot 0");

    check(args->api->disable_thread_calls(h_mod_a) == TRUE,
          "DisableThreadLibraryCalls on pseudo-handle returns TRUE under poisoned TLS slot 0");
    check(args->api->free_library(h_mod_a) == TRUE,
          "FreeLibrary on pseudo-handle returns TRUE under poisoned TLS slot 0");

    // First-ever call into rt::d3d12_trace::record/result with TLS slot 0 poisoned.
    rt::d3d12_trace::set_enabled(true);
    rt::d3d12_trace::record("TlsSlot0FirstUse", "called");
    check(rt::d3d12_trace::result("TlsSlot0FirstUse", S_OK) == S_OK,
          "d3d12_trace::result returns S_OK under poisoned TLS slot 0");
    check(rt::d3d12_trace::result("TlsSlot0FirstUse", E_NOTIMPL) == E_NOTIMPL,
          "d3d12_trace::result returns E_NOTIMPL under poisoned TLS slot 0");

    return 0;
}

DWORD WINAPI concurrent_worker(LPVOID param) {
    auto* args = static_cast<WorkerArgs*>(param);
    if (g_tls_slot0_anchor != 0x13572468u) return 1;

    ScopedTlsSlot0Poison poison(args->zero_page);
    if (!poison.slots) {
        check(false, "concurrent worker has TEB->ThreadLocalStoragePointer");
        return 1;
    }

    const std::string unserved = "ConcurrentMissing_" + std::to_string(args->thread_id);
    for (int i = 0; i < args->iterations; ++i) {
        const HMODULE h = args->api->get_module_handle_a("leonida_tls_probe.dll");
        if (h != args->expected_pseudo) {
            check(false, "concurrent GetModuleHandleA returned unexpected handle");
            return 1;
        }
        const FARPROC served = args->api->get_proc_address(h, "ProbeExport");
        if (reinterpret_cast<const void*>(served) != reinterpret_cast<const void*>(&probe_export)) {
            check(false, "concurrent GetProcAddress served lookup mismatch");
            return 1;
        }
        const FARPROC missing = args->api->get_proc_address(h, unserved.c_str());
        if (missing != nullptr || GetLastError() != ERROR_PROC_NOT_FOUND) {
            check(false, "concurrent GetProcAddress unserved lookup mismatch");
            return 1;
        }
        const FARPROC host_shim = args->api->get_proc_address(args->kernel32, "GetModuleHandleA");
        if (reinterpret_cast<const void*>(host_shim) !=
            reinterpret_cast<const void*>(args->api->get_module_handle_a)) {
            check(false, "concurrent host GetProcAddress lookup mismatch");
            return 1;
        }
        if (args->api->disable_thread_calls(h) != TRUE || args->api->free_library(h) != TRUE) {
            check(false, "concurrent DisableThreadLibraryCalls / FreeLibrary failed");
            return 1;
        }
        rt::d3d12_trace::record("TlsSlot0Concurrent", "ok");
    }
    return 0;
}

}  // namespace

SHIM_TABLE(library_loader_tls_probe,
    {"leonida_tls_probe.dll", "ProbeExport", reinterpret_cast<void*>(&probe_export)},
);

int main() {
    check(g_tls_slot0_anchor == 0x13572468u, "main thread TLS slot 0 is active");
    check(module_tls_slots() != nullptr, "TEB->ThreadLocalStoragePointer is non-null");

    // Prime registry.cpp lookups (tracked separately) before poisoning TLS slot 0 so this test isolates
    // library_loader.cpp and d3d12_trace.h. Do NOT call any library_loader function or d3d12_trace::record yet.
    ShimApi api;
    api.load_library_a = reinterpret_cast<HMODULE(WINAPI*)(LPCSTR)>(rt::shims::find("kernel32.dll", "LoadLibraryA"));
    api.load_library_w = reinterpret_cast<HMODULE(WINAPI*)(LPCWSTR)>(rt::shims::find("kernel32.dll", "LoadLibraryW"));
    api.get_module_handle_a =
        reinterpret_cast<HMODULE(WINAPI*)(LPCSTR)>(rt::shims::find("kernel32.dll", "GetModuleHandleA"));
    api.get_module_handle_w =
        reinterpret_cast<HMODULE(WINAPI*)(LPCWSTR)>(rt::shims::find("kernel32.dll", "GetModuleHandleW"));
    api.get_proc_address =
        reinterpret_cast<FARPROC(WINAPI*)(HMODULE, LPCSTR)>(rt::shims::find("kernel32.dll", "GetProcAddress"));
    api.free_library = reinterpret_cast<BOOL(WINAPI*)(HMODULE)>(rt::shims::find("kernel32.dll", "FreeLibrary"));
    api.disable_thread_calls =
        reinterpret_cast<BOOL(WINAPI*)(HMODULE)>(rt::shims::find("kernel32.dll", "DisableThreadLibraryCalls"));
    check(api.load_library_a && api.load_library_w && api.get_module_handle_a && api.get_module_handle_w &&
              api.get_proc_address && api.free_library && api.disable_thread_calls,
          "library_loader shim table is registered");
    check(rt::shims::owns_dll("leonida_tls_probe.dll"), "synthetic probe DLL is owned by shim registry");
    check(!rt::shims::owns_bundled_module_handle(nullptr), "registry bundled-loader table primed");

    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    check(kernel32 != nullptr, "host kernel32.dll handle acquired");

    void* zero_page = VirtualAlloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    check(zero_page != nullptr, "allocated zeroed scratch TLS page");
    if (!zero_page || g_failures.load() != 0) {
        if (zero_page) VirtualFree(zero_page, 0, MEM_RELEASE);
        return 1;
    }

    // Flush stdio before entering poisoned-TLS worker threads.
    std::fflush(stdout);
    std::fflush(stderr);

    // Phase 1: first-ever initialization of library_loader.cpp and d3d12_trace.h under poisoned TLS slot 0.
    WorkerArgs first_args{&api, zero_page, kernel32, nullptr, 0, 1};
    HANDLE h_first = CreateThread(nullptr, 0, &first_use_worker, &first_args, 0, nullptr);
    check(h_first != nullptr, "created first-use worker thread");
    if (h_first) {
        check(WaitForSingleObject(h_first, 30000) == WAIT_OBJECT_0, "first-use worker exited cleanly");
        DWORD exit_code = 1;
        GetExitCodeThread(h_first, &exit_code);
        check(exit_code == 0, "first-use worker returned 0");
        CloseHandle(h_first);
    }

    // Phase 2: concurrent access across 4 worker threads with TLS slot 0 poisoned.
    constexpr int kThreads = 4;
    constexpr int kIterations = 8;
    std::vector<WorkerArgs> args(kThreads);
    std::vector<HANDLE> threads(kThreads, nullptr);
    for (int t = 0; t < kThreads; ++t) {
        args[t] = WorkerArgs{&api, zero_page, kernel32, first_args.expected_pseudo, t, kIterations};
        threads[t] = CreateThread(nullptr, 0, &concurrent_worker, &args[t], 0, nullptr);
        check(threads[t] != nullptr, "created concurrent worker thread");
    }
    for (int t = 0; t < kThreads; ++t) {
        if (!threads[t]) continue;
        check(WaitForSingleObject(threads[t], 30000) == WAIT_OBJECT_0, "concurrent worker exited cleanly");
        DWORD exit_code = 1;
        GetExitCodeThread(threads[t], &exit_code);
        check(exit_code == 0, "concurrent worker returned 0");
        CloseHandle(threads[t]);
    }

    rt::d3d12_trace::set_enabled(false);
    VirtualFree(zero_page, 0, MEM_RELEASE);

    std::printf("library_loader_tls_safe_test: %s (%d checks, %d failures)\n",
                g_failures.load() ? "FAILED" : "OK", g_checks.load(), g_failures.load());
    return g_failures.load() ? 1 : 0;
}
