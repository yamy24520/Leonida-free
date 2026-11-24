// Check that abort() from a mapped guest raises SIGABRT when the guest installed a handler, and keeps gta6rt's
// recorded abort code (3) for the default, ignored and returning-handler cases. Each mode runs natively too: the
// handler case must match, while the other three intentionally differ (current UCRT fail-fasts with 0xC0000409
// after the raise, where gta6rt ends the run with its documented code). No game files.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {
int failures = 0;

bool run(const std::wstring& exe, const std::wstring& args, const char* what, DWORD want) {
    std::wstring command = L"\"" + exe + L"\" " + args;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &child)) {
        std::fprintf(stderr, "FAIL %s: could not start (Win32 error %lu)\n", what, GetLastError());
        ++failures;
        return false;
    }
    CloseHandle(child.hThread);
    DWORD code = 0;
    const DWORD wait = WaitForSingleObject(child.hProcess, 30000);
    const bool exited = wait == WAIT_OBJECT_0 && GetExitCodeProcess(child.hProcess, &code);
    if (wait == WAIT_TIMEOUT) TerminateProcess(child.hProcess, 1);
    CloseHandle(child.hProcess);
    if (!exited) {
        std::fprintf(stderr, "FAIL %s: could not collect the exit code (wait %#lx)\n", what, wait);
        ++failures;
        return false;
    }
    if (code != want) {
        std::fprintf(stderr, "FAIL %s: exit code %lu, expected %lu\n", what, code, want);
        ++failures;
        return false;
    }
    std::printf("PASS %s (exit %lu)\n", what, code);
    return true;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: tiny_game_abort_test <gta6rt.exe> <tiny_game.exe>\n");
        return 1;
    }
    struct Case {
        const char* mode;
        DWORD native_code;
        DWORD loader_code;
    };
    static const Case cases[] = {
        {"abort-handler", 43, 43},     // the handler ends the run itself, so abort cannot overwrite its code
        {"abort-crt-exit", 43, 43},    // the same through the guest's shimmed exit() import
        {"abort-return", 0xC0000409, 3},
        {"abort-ignore", 0xC0000409, 3},
        {"abort-default", 0xC0000409, 3},
    };
    for (const Case& c : cases) {
        const std::wstring mode(c.mode, c.mode + std::strlen(c.mode));
        const std::wstring guest = L"hello 42 " + mode;
        const std::string native = std::string("native ") + c.mode;
        const std::string loaded = std::string("gta6rt ") + c.mode;
        run(argv[2], guest, native.c_str(), c.native_code);
        run(argv[1], L"--run \"" + std::wstring(argv[2]) + L"\" -- " + guest, loaded.c_str(), c.loader_code);
    }
    if (failures) {
        std::fprintf(stderr, "%d abort exit-code check(s) failed\n", failures);
        return 1;
    }
    std::printf("all abort exit-code checks passed\n");
    return 0;
}
