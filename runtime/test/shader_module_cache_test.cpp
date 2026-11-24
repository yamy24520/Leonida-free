// leonida::ShaderModuleCache (src/shader_module_cache.h): one module per distinct SPIR-V, shared by the pipeline
// states built from it and destroyed with the last of them. CPU only: the create and destroy callbacks hand out and
// take back fake handles, so the test counts what a device would have created.
#include "../src/shader_module_cache.h"

#include <cstdint>
#include <cstdio>
#include <set>
#include <thread>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

struct FakeDevice {
    std::uintptr_t next = 1;
    std::set<VkShaderModule> alive;
    std::size_t created = 0;
    bool fail = false;

    leonida::ShaderModuleCache cache() {
        return leonida::ShaderModuleCache(
            [this](const std::vector<std::uint32_t>&) {
                if (fail) return VkShaderModule(VK_NULL_HANDLE);
                const auto module = reinterpret_cast<VkShaderModule>(next++);
                alive.insert(module);
                ++created;
                return module;
            },
            [this](VkShaderModule module) { alive.erase(module); });
    }
};

}  // namespace

int main() {
    const std::vector<std::uint32_t> vs = {0x07230203, 0x00010500, 0, 10, 0, 1, 2};
    const std::vector<std::uint32_t> ps = {0x07230203, 0x00010500, 0, 10, 0, 3, 4};
    std::vector<std::uint32_t> ps_patched = ps;  // a per-PSO edit (the depth24 patch): other words, its own module
    ps_patched.back() = 5;

    // 1,000 pipeline states from one vertex and one pixel shader: two modules, not 2,000.
    {
        FakeDevice device;
        auto cache = device.cache();
        std::vector<VkShaderModule> held;
        for (int i = 0; i < 1000; ++i) {
            held.push_back(cache.acquire(vs));
            held.push_back(cache.acquire(ps));
        }
        CHECK(device.created == 2);
        CHECK(cache.live() == 2 && device.alive.size() == 2);
        CHECK(cache.shared() == 1998);
        CHECK(held[0] == held[2] && held[1] == held[3] && held[0] != held[1]);
        // Different words never share, whatever their hash.
        const VkShaderModule patched = cache.acquire(ps_patched);
        CHECK(patched != held[1] && device.created == 3);
        cache.release(patched);
        CHECK(device.alive.size() == 2);
        // Released one by one: alive until the last pipeline state goes.
        for (std::size_t i = 0; i + 2 < held.size(); ++i) cache.release(held[i]);
        CHECK(device.alive.size() == 2);
        cache.release(held[held.size() - 2]);
        CHECK(device.alive.size() == 1);
        cache.release(held.back());
        CHECK(device.alive.empty() && cache.live() == 0);
        // A released module is gone: the same words create a new one.
        cache.release(cache.acquire(vs));
        CHECK(device.created == 4 && device.alive.empty());
        // Unknown and null handles are ignored.
        cache.release(VK_NULL_HANDLE);
        cache.release(reinterpret_cast<VkShaderModule>(std::uintptr_t(0x9999)));
        CHECK(device.alive.empty());
    }

    // A failed creation caches nothing; the next acquire tries again.
    {
        FakeDevice device;
        auto cache = device.cache();
        device.fail = true;
        CHECK(cache.acquire(vs) == VK_NULL_HANDLE && cache.live() == 0);
        device.fail = false;
        const VkShaderModule module = cache.acquire(vs);
        CHECK(module != VK_NULL_HANDLE && cache.live() == 1);
        cache.release(module);
    }

    // The destructor destroys what a leaked pipeline state still held.
    {
        FakeDevice device;
        {
            auto cache = device.cache();
            cache.acquire(vs);
            cache.acquire(ps);
        }
        CHECK(device.alive.empty());
    }

    // Concurrent acquires and releases from several threads (PSOs are created on the title's job threads).
    {
        FakeDevice device;
        auto cache = device.cache();
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
            threads.emplace_back([&] {
                for (int i = 0; i < 2000; ++i) cache.release(cache.acquire(i % 2 ? vs : ps));
            });
        for (auto& thread : threads) thread.join();
        CHECK(cache.live() == 0 && device.alive.empty());
    }

    if (failures) return 1;
    std::printf("shader_module_cache: ok\n");
    return 0;
}
