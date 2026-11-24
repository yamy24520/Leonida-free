// Checks runtime/shims/registry.cpp's constant-initialized lazy state (RT-488 class: zero function-local static init
// guards for table(), bundled loaders, and unimplemented thunks) and XCRDAPI.dll!DStorageGetFactory classification.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "shims.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

int WINAPI probe_shim_fn() { return 0x4C45; }

struct FakeLoader {
    void* handle = nullptr;
    void* proc = nullptr;
    std::atomic<int> find_calls{0};
    std::atomic<int> load_calls{0};
    std::atomic<int> proc_calls{0};
    std::atomic<int> owns_calls{0};
};

void* fake_find_loaded(void* ctx, const std::string& dll_lower) {
    auto* fl = static_cast<FakeLoader*>(ctx);
    fl->find_calls.fetch_add(1, std::memory_order_relaxed);
    return dll_lower == "probe_bundled.dll" ? fl->handle : nullptr;
}

void* fake_load_bundled(void* ctx, const std::string& dll_lower, uint32_t, bool& handled) {
    auto* fl = static_cast<FakeLoader*>(ctx);
    fl->load_calls.fetch_add(1, std::memory_order_relaxed);
    if (dll_lower == "probe_bundled.dll") {
        handled = true;
        return fl->handle;
    }
    handled = false;
    return nullptr;
}

void* fake_get_proc(void* ctx, void* module, const std::string& sym, bool& owns_handle) {
    auto* fl = static_cast<FakeLoader*>(ctx);
    fl->proc_calls.fetch_add(1, std::memory_order_relaxed);
    if (module == fl->handle) {
        owns_handle = true;
        return sym == "ProbeExport" ? fl->proc : nullptr;
    }
    owns_handle = false;
    return nullptr;
}

bool fake_owns_handle(void* ctx, void* module) {
    auto* fl = static_cast<FakeLoader*>(ctx);
    fl->owns_calls.fetch_add(1, std::memory_order_relaxed);
    return module == fl->handle;
}

const rt::shims::BundledModuleOps kFakeOps{
    &fake_find_loaded,
    &fake_load_bundled,
    &fake_get_proc,
    &fake_owns_handle,
};

}  // namespace

SHIM_TABLE(registry_init_probe,
    {"ProbeOwned.dll", "ProbeFn", reinterpret_cast<void*>(&probe_shim_fn)},
    {"kernel32.dll", "ProbeKernelFn", reinterpret_cast<void*>(&probe_shim_fn)},
);

int main() {
    using rt::shims::ReturnClass;
    using rt::shims::UnimplementedPolicy;

    // 1. Static SHIM_TABLE registered via InitOnceExecuteOnce table().
    check(rt::shims::find("probeowned.dll", "ProbeFn") == reinterpret_cast<void*>(&probe_shim_fn),
          "static SHIM_TABLE entry resolves via table()");
    check(rt::shims::find("probeowned.dll", "Missing") == nullptr, "missing shim entry returns null");
    check(rt::shims::owns_dll("probeowned.dll"), "owns_dll true for registered non-host DLL");
    check(!rt::shims::owns_dll("kernel32.dll"), "owns_dll false for host-excluded kernel32.dll");
    check(!rt::shims::owns_dll("api-ms-win-core-file-l1-1-0.dll"), "owns_dll false for api-ms-* sets");

    // 2. XCRDAPI.dll!DStorageGetFactory classified as HResult (#6715 retail executable import).
    check(rt::shims::classify_import("XCRDAPI.dll", "DStorageGetFactory") == ReturnClass::HResult,
          "XCRDAPI.dll!DStorageGetFactory is ReturnClass::HResult");
    check(rt::shims::classify_import("xcrdapi.dll", "DStorageGetFactory") == ReturnClass::HResult,
          "xcrdapi.dll!DStorageGetFactory lowercase is ReturnClass::HResult");
    check(rt::shims::classify_import("XCRDAPI.dll", "OtherUnmeasuredFn") == ReturnClass::Other,
          "other XCRDAPI.dll exports remain ReturnClass::Other");
    check(rt::shims::classify_import("dstorage.dll", "DStorageGetFactory") == ReturnClass::HResult &&
              rt::shims::classify_import("dstorage_xs.dll", "DStorageGetFactory") == ReturnClass::HResult,
          "dstorage.dll and dstorage_xs.dll DStorageGetFactory stay HResult");
    check(rt::shims::classify_import("acphal.dll", "AcpHalCreate") == ReturnClass::Other &&
              rt::shims::classify_import("xg_xs.dll", "XGCreateTexture2DComputer") == ReturnClass::Other,
          "unmeasured console DLLs remain ReturnClass::Other");

    // 3. Unimplemented thunk execution under Fail policy verifies E_NOTIMPL for XCRDAPI.dll!DStorageGetFactory.
    rt::shims::set_unimplemented_policy(UnimplementedPolicy::Fail);
    using ThunkFn = uint64_t(WINAPI*)(uint64_t, uint64_t);
    void* const t_xcrd_a = rt::shims::make_unimplemented_thunk("XCRDAPI.dll", "DStorageGetFactory");
    void* const t_xcrd_b = rt::shims::make_unimplemented_thunk("xcrdapi.dll", "DStorageGetFactory");
    void* const t_other = rt::shims::make_unimplemented_thunk("XCRDAPI.dll", "OtherUnmeasuredFn");
    check(t_xcrd_a != nullptr && t_xcrd_a == t_xcrd_b, "make_unimplemented_thunk deduplicates case-insensitively");
    check(t_other != nullptr && t_other != t_xcrd_a, "distinct unimplemented imports get distinct thunks");
    if (t_xcrd_a && t_other) {
        const uint64_t r_hr = reinterpret_cast<ThunkFn>(t_xcrd_a)(1, 2);
        const uint64_t r_other = reinterpret_cast<ThunkFn>(t_other)(1, 2);
        check(r_hr == static_cast<uint64_t>(static_cast<uint32_t>(E_NOTIMPL)),
              "XCRDAPI.dll!DStorageGetFactory thunk returns E_NOTIMPL under Fail policy");
        check(r_other == 0, "XCRDAPI.dll!OtherUnmeasuredFn thunk returns 0 under Fail policy");
    }

    // 4. Bundled-loader registry before any add_bundled_loader call (null-vector fast path).
    bool handled = true;
    bool owns = true;
    check(rt::shims::find_loaded_bundled_module("probe_bundled.dll") == nullptr,
          "find_loaded_bundled_module returns null before any loader is registered");
    check(rt::shims::load_bundled_module("probe_bundled.dll", 0, handled) == nullptr && !handled,
          "load_bundled_module reports unhandled before any loader is registered");
    check(rt::shims::get_bundled_proc_address(reinterpret_cast<void*>(0x1234), "ProbeExport", owns) == nullptr && !owns,
          "get_bundled_proc_address reports unowned before any loader is registered");
    check(!rt::shims::owns_bundled_module_handle(reinterpret_cast<void*>(0x1234)),
          "owns_bundled_module_handle false before any loader is registered");
    rt::shims::remove_bundled_loader(reinterpret_cast<void*>(0x1234));
    rt::shims::add_bundled_loader(nullptr, &kFakeOps);
    rt::shims::add_bundled_loader(reinterpret_cast<void*>(0x1234), nullptr);
    check(rt::shims::find_loaded_bundled_module("probe_bundled.dll") == nullptr,
          "null ctx/ops ignored by add_bundled_loader");

    // 5. Bundled-loader register / dispatch / remove lifecycle.
    FakeLoader fl{};
    fl.handle = reinterpret_cast<void*>(0x4C450001ull);
    fl.proc = reinterpret_cast<void*>(&probe_shim_fn);
    rt::shims::add_bundled_loader(&fl, &kFakeOps);
    check(rt::shims::find_loaded_bundled_module("probe_bundled.dll") == fl.handle,
          "find_loaded_bundled_module dispatches to registered loader");
    check(rt::shims::load_bundled_module("probe_bundled.dll", 0, handled) == fl.handle && handled,
          "load_bundled_module dispatches to registered loader");
    check(rt::shims::get_bundled_proc_address(fl.handle, "ProbeExport", owns) == fl.proc && owns,
          "get_bundled_proc_address resolves registered export");
    check(rt::shims::owns_bundled_module_handle(fl.handle), "owns_bundled_module_handle true for registered handle");
    rt::shims::remove_bundled_loader(&fl);
    check(rt::shims::find_loaded_bundled_module("probe_bundled.dll") == nullptr &&
              !rt::shims::owns_bundled_module_handle(fl.handle),
          "remove_bundled_loader cleanly unregisters loader");

    // 6. Multi-threaded stress on table(), make_unimplemented_thunk(), and bundled loader SRWLOCK.
    std::atomic<bool> mt_ok{true};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t]() {
            FakeLoader local_fl{};
            local_fl.handle = reinterpret_cast<void*>(0x50000000ull + static_cast<uint64_t>(t));
            local_fl.proc = reinterpret_cast<void*>(&probe_shim_fn);
            for (int i = 0; i < 60; ++i) {
                if (rt::shims::find("probeowned.dll", "ProbeFn") != reinterpret_cast<void*>(&probe_shim_fn) ||
                    !rt::shims::owns_dll("probeowned.dll")) {
                    mt_ok.store(false, std::memory_order_relaxed);
                    return;
                }
                void* p1 = rt::shims::make_unimplemented_thunk("XCRDAPI.dll", "DStorageGetFactory");
                void* p2 = rt::shims::make_unimplemented_thunk("stress_probe.dll", "Fn_" + std::to_string(t * 100 + i));
                if (p1 != t_xcrd_a || !p2) {
                    mt_ok.store(false, std::memory_order_relaxed);
                    return;
                }
                rt::shims::add_bundled_loader(&local_fl, &kFakeOps);
                (void)rt::shims::find_loaded_bundled_module("other.dll");
                rt::shims::remove_bundled_loader(&local_fl);
            }
        });
    }
    for (auto& th : threads) th.join();
    check(mt_ok.load(), "concurrent table(), make_unimplemented_thunk(), and bundled-loader stress");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
