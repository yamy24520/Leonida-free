// Tiny fixture EXE for module TLS index 0. It is ordinary test code, not game content.
//
// Built /GA (CMakeLists.txt), the EXE-only option that lets the compiler assume the image is the process executable
// and so has module TLS index 0: every thread_local access below reads TEB->ThreadLocalStoragePointer[0] directly
// (mov rax, gs:[58h]; mov rax, [rax]) and never loads _tls_index. Mapped with any other index, the getter reads
// whatever block sits in slot 0. loader_tls_index0_test calls the exports on threads of its own; under
// `gta6rt --run` (run_tiny_tls_ga) main() checks the value on the game thread and on a second thread and returns 0
// only when both read the template value 42.
#include <cstdio>
#include <thread>

extern "C" unsigned long _tls_index;

namespace {
thread_local int t_value = 42;
}

extern "C" __declspec(dllexport) __declspec(noinline) int __cdecl tiny_ga_get() { return t_value; }
extern "C" __declspec(dllexport) __declspec(noinline) void __cdecl tiny_ga_set(int value) { t_value = value; }
// Where this thread's t_value is, so the test can find its offset inside the slot-0 block.
extern "C" __declspec(dllexport) __declspec(noinline) int* __cdecl tiny_ga_address() { return &t_value; }
extern "C" __declspec(dllexport) unsigned long __cdecl tiny_ga_tls_index() { return _tls_index; }
extern "C" __declspec(dllexport) unsigned long* __cdecl tiny_ga_tls_index_address() { return &_tls_index; }

int main() {
    const int on_main = tiny_ga_get();
    tiny_ga_set(7);
    int on_thread = -1;
    std::thread worker([&] { on_thread = tiny_ga_get(); });
    worker.join();
    const bool ok = on_main == 42 && on_thread == 42 && tiny_ga_get() == 7;
    std::printf("tiny_tls_ga: tls_index=%lu main=%d thread=%d %s\n", _tls_index, on_main, on_thread,
                ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
