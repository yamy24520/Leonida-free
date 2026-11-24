#pragma once

#include <windows.h>

#include <atomic>
#include <cstdio>

namespace rt::d3d12_trace {

inline std::atomic<bool> tracing{false};
// Constant-initialized and immortal so guest threads (when the mapped executable owns TLS slot 0) never enter
// an _Init_thread_header guard or touch a destroyed mutex during teardown.
inline SRWLOCK trace_lock = SRWLOCK_INIT;
inline unsigned long long sequence = 0;

inline void set_enabled(bool enable) noexcept {
    tracing.store(enable, std::memory_order_relaxed);
}

inline bool enabled() noexcept {
    return tracing.load(std::memory_order_relaxed);
}

// Callers supply fixed method names and outcomes only. Keep one ordered record per call across threads.
inline void record(const char* method, const char* outcome = "called") noexcept {
    if (!enabled()) return;
    AcquireSRWLockExclusive(&trace_lock);
    std::fprintf(stderr, "[d3d12-trace] #%llu %s %s\n", ++sequence, method, outcome);
    std::fflush(stderr);
    ReleaseSRWLockExclusive(&trace_lock);
}

inline HRESULT result(const char* method, HRESULT code) noexcept {
    record(method, code == E_NOTIMPL ? "unsupported" : FAILED(code) ? "failed" : "ok");
    return code;
}

}  // namespace rt::d3d12_trace
