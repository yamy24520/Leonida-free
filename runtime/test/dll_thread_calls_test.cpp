// DisableThreadLibraryCalls from a mapped image's DllMain (shims/library_loader.cpp).
//
// The real function only accepts a module the Windows loader knows, so for a manually mapped image it returned FALSE
// and a DllMain that returns its result made Loader::initialise() throw "DllMain returned FALSE". The shim answers
// TRUE for a mapped module and for a module the shims own (a pseudo-handle), and leaves every other handle to the
// real function.
//
// Maps dll_thread_calls.dll (test/fixtures/dll_thread_calls.cpp). xmem.cpp is linked only so the shims own a module
// (xmem.dll) to hand out a pseudo-handle for. No game files.
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "pe_loader.h"
#include "shims.h"

namespace {

int fake_shim_han() { return 1; }
int fake_shim_zi() { return 2; }
int fake_shim_qmark() { return 3; }

SHIM_TABLE(test_unicode_module_shims,
           {"uni-\xe6\xbc\xa2.dll", "probe_han", reinterpret_cast<void*>(&fake_shim_han)},
           {"uni-\xe5\xad\x97.dll", "probe_zi", reinterpret_cast<void*>(&fake_shim_zi)},
           {"uni-?.dll", "probe_collide", reinterpret_cast<void*>(&fake_shim_qmark)});

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: dll_thread_calls_test <dll_thread_calls.dll>\n");
        return 2;
    }
    const std::filesystem::path dll = argv[1];
    try {
        rt::Loader loader(dll.parent_path());
        rt::Module& m = loader.load(dll);

        bool shimmed = false;
        for (const auto& r : m.imports)
            if (r.name == "DisableThreadLibraryCalls") shimmed = r.binding == rt::Binding::Shim;
        check(shimmed, "the DisableThreadLibraryCalls import is bound to the shim");

        bool initialised = true;
        try {
            loader.initialise();
        } catch (const std::exception& e) {
            initialised = false;
            std::printf("     initialise() threw: %s\n", e.what());
        }
        check(initialised, "initialise() succeeds when DllMain returns DisableThreadLibraryCalls(hinstance)");

        const auto bogus =
            reinterpret_cast<BOOL(__cdecl*)()>(m.exports.at("dll_thread_calls_bogus_handle"));
        SetLastError(0);
        check(bogus() == FALSE, "a handle that names no module still gets the real answer (FALSE)");

        // A shim pseudo-handle is no module the Windows loader knows, so the real function would fail on it.
        using GetModuleHandleAFn = HMODULE(WINAPI*)(LPCSTR);
        using DisableFn = BOOL(WINAPI*)(HMODULE);
        const auto get_module =
            reinterpret_cast<GetModuleHandleAFn>(rt::shims::find("kernel32.dll", "GetModuleHandleA"));
        const auto disable =
            reinterpret_cast<DisableFn>(rt::shims::find("kernel32.dll", "DisableThreadLibraryCalls"));
        check(get_module != nullptr && disable != nullptr,
              "the registry serves GetModuleHandleA and DisableThreadLibraryCalls");
        const HMODULE owned = get_module ? get_module("xmem.dll") : nullptr;
        check(owned != nullptr, "the shims hand out a pseudo-handle for the module they own (xmem.dll)");
        check(disable != nullptr && owned != nullptr && disable(owned) == TRUE,
              "DisableThreadLibraryCalls on a shim pseudo-handle answers TRUE");

        // Host modules with distinct non-ASCII names must not collide on '?' in host_module_name() / get_proc_address().
        using GetProcFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
        using LoadLibWFn = HMODULE(WINAPI*)(LPCWSTR);
        const auto get_proc =
            reinterpret_cast<GetProcFn>(rt::shims::find("kernel32.dll", "GetProcAddress"));
        const auto load_lib_w =
            reinterpret_cast<LoadLibWFn>(rt::shims::find("kernel32.dll", "LoadLibraryW"));
        check(get_proc != nullptr && load_lib_w != nullptr,
              "the registry serves GetProcAddress and LoadLibraryW");
        if (get_proc && load_lib_w) {
            const std::filesystem::path uni_dir =
                std::filesystem::temp_directory_path() /
                ("leonida_ll_utf8_" + std::to_string(GetCurrentProcessId()));
            std::error_code ec;
            std::filesystem::remove_all(uni_dir, ec);
            std::filesystem::create_directories(uni_dir, ec);
            const std::filesystem::path han_path = uni_dir / L"Uni-\u6f22.dll";
            const std::filesystem::path zi_path = uni_dir / L"Uni-\u5b57.dll";
            std::filesystem::copy_file(dll, han_path, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::copy_file(dll, zi_path, std::filesystem::copy_options::overwrite_existing, ec);
            const HMODULE h_han = LoadLibraryW(han_path.c_str());
            const HMODULE h_zi = LoadLibraryW(zi_path.c_str());
            check(h_han != nullptr && h_zi != nullptr && h_han != h_zi,
                  "native Windows loads distinct non-ASCII DLL copies");
            if (h_han && h_zi) {
                check(reinterpret_cast<void*>(get_proc(h_han, "probe_han")) ==
                          reinterpret_cast<void*>(&fake_shim_han),
                      "GetProcAddress resolves UTF-8 module name Uni-U+6F22.dll without '?' replacement");
                check(reinterpret_cast<void*>(get_proc(h_zi, "probe_zi")) ==
                          reinterpret_cast<void*>(&fake_shim_zi),
                      "GetProcAddress resolves UTF-8 module name Uni-U+5B57.dll without '?' replacement");
                check(get_proc(h_han, "probe_collide") == nullptr && get_proc(h_zi, "probe_collide") == nullptr,
                      "distinct non-ASCII module names do not collapse to uni-?.dll");
            }
            if (h_han) FreeLibrary(h_han);
            if (h_zi) FreeLibrary(h_zi);
            const HMODULE owned_dir = load_lib_w(L"C:\\\u6f22\\xmem.dll");
            check(owned_dir != nullptr && owned_dir == owned,
                  "LoadLibraryW finds an owned ASCII shim inside a non-ASCII directory");
            std::filesystem::remove_all(uni_dir, ec);
        }
    } catch (const std::exception& e) {
        std::printf("FAIL unexpected exception: %s\n", e.what());
        ++g_failures;
    }
    std::printf("dll_thread_calls_test: %s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
