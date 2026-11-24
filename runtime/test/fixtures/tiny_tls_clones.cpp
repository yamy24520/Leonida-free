// Tiny fixture EXE for check_tls_clone_cleanup.cmake. It is ordinary test code, not game content.
//
// It imports two DLLs with static TLS, tiny_tls.dll and tiny_tls_cb_only.dll, and has TLS of its own. Mapped by gta6rt
// the EXE takes module TLS index 0, one DLL takes the primary helper's index and the other needs a helper clone
// (leonida_tls_host_<pid>_1.dll in %TEMP%), whose file gta6rt has to delete when it exits.
extern "C" __declspec(dllimport) int __cdecl tiny_tls_get();
extern "C" __declspec(dllimport) int __cdecl tiny_tls_cb_attach_count();

namespace {
thread_local int t_value = 3;
}

int main() { return tiny_tls_get() == 7 && tiny_tls_cb_attach_count() >= 0 && t_value == 3 ? 0 : 1; }
