#pragma once

// Observe a serialized native queue-present call, not GPU completion or screen contents.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace leonida {
class PresentQueueTrace {
public:
    enum class Source { presenter, dxgi };

    explicit PresentQueueTrace(Source source)
        : source_(source == Source::presenter ? "presenter" : "dxgi"),
          id_(enabled() ? next_.fetch_add(1, std::memory_order_relaxed) : 0) {}
    PresentQueueTrace(const PresentQueueTrace&) = delete;
    PresentQueueTrace& operator=(const PresentQueueTrace&) = delete;

    // The caller holds its normal queue lock, including while assigning the sequence number.
    template <typename Dispatch>
    auto call(std::uint32_t image, Dispatch dispatch) -> decltype(dispatch()) {
        if (!id_) return dispatch();
#ifdef _WIN32
        const auto pid = static_cast<unsigned long>(_getpid());
#else
        const auto pid = static_cast<unsigned long>(getpid());
#endif
        const auto n = ++sequence_;
        std::fprintf(stderr, "FRAMEPROOF QUEUE_CALL pid=%lu source=%s id=%llu n=%llu image=%u\n",
                     pid, source_, id_, n, static_cast<unsigned>(image));
        std::fflush(stderr);
        const auto raw = dispatch();
        std::fprintf(stderr, "FRAMEPROOF QUEUE_RETURN pid=%lu source=%s id=%llu n=%llu image=%u raw=%d\n",
                     pid, source_, id_, n, static_cast<unsigned>(image), static_cast<int>(raw));
        std::fflush(stderr);
        return raw;
    }

private:
    static bool enabled() {
#ifdef _MSC_VER
        char value[2]{};
        size_t required = 0;
        if (getenv_s(&required, nullptr, 0, "LEONIDA_PRESENT_QUEUE_TRACE") || required != sizeof value)
            return false;
        return !getenv_s(&required, value, sizeof value, "LEONIDA_PRESENT_QUEUE_TRACE") && value[0] == '1';
#else
        const char* value = std::getenv("LEONIDA_PRESENT_QUEUE_TRACE");
        return value && value[0] == '1' && value[1] == '\0';
#endif
    }

    inline static std::atomic<unsigned long long> next_{1};
    const char* const source_;
    const unsigned long long id_;  // opaque per loaded producer; never emit addresses or Vulkan handles
    unsigned long long sequence_ = 0;
};
}  // namespace leonida
