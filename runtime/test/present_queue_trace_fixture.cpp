// Exercise the actual wrapper with a synthetic dispatch, without Vulkan or a GPU.
#include "../src/present_queue_trace.h"
#include <cstdio>

int main() {
    using Trace = leonida::PresentQueueTrace;
    Trace a(Trace::Source::presenter), b(Trace::Source::presenter), c(Trace::Source::dxgi);
    auto call = [](Trace& trace, unsigned image, int result) {
        int calls = 0;
        const int actual = trace.call(image, [&] {
            ++calls;
            std::fprintf(stderr, "dispatch %d\n", result);
            return result;
        });
        return calls == 1 && actual == result;
    };
    return call(a, 2, 0) && call(b, 1, -1000001004) &&
           call(a, 0, 1000001003) && call(c, 3, -4) ? 0 : 1;
}
