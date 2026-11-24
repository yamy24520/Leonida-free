// Regression test for TLS-slot-0 safety across d3d12_com.cpp, d3d12_vulkan_device.cpp, queue_holds.h, and
// shader_library.h.
//
// When a mapped guest executable owns static TLS index 0 (#4313), TEB->ThreadLocalStoragePointer[0] holds the guest's
// TLS block instead of the host executable's _Init_thread_epoch. A zeroed guest block makes MSVC skip function-local
// dynamic static initialization ($TSS > _Init_thread_epoch = 0 > 0), and a static thread_local bool (QueueHolds::in_pump)
// reads/writes guest TLS memory directly.
//
// This synthetic test swaps slot 0 of TEB->ThreadLocalStoragePointer on worker threads to a 64 KiB scratch/readonly
// page before any affected function runs, exercising CPU and Vulkan paths concurrently. No game files.
#include <windows.h>
#include <intrin.h>

#include "../shims/d3d12_com.h"
#include "../shims/d3d12_trace.h"
#include "../src/queue_holds.h"
#include "../src/shader_library.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace rt::d3d12 {
void stub(const char* method);
}

namespace {

// Forces a .tls directory in this executable so TEB->ThreadLocalStoragePointer[0] exists.
thread_local std::uint32_t g_tls_slot0_anchor = 0x13572468u;
volatile std::uint32_t g_anchor_sink = 0;

template<class T>
struct Ref {
    T* p = nullptr;
    ~Ref() {
        if (p) p->Release();
    }
    T* operator->() const { return p; }
    void** out() { return reinterpret_cast<void**>(&p); }
};

std::atomic<int> g_failures{0};

void fail_at(int line, const char* what) {
    ++g_failures;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
    std::fflush(stderr);
}

#define CHECK(cond) do { if (!(cond)) { fail_at(__LINE__, #cond); return false; } } while (0)

void** module_tls_slots() noexcept {
    return reinterpret_cast<void**>(__readgsqword(0x58));
}

struct ScopedTlsSlot0Poison {
    bool active = false;
    void* saved = nullptr;

    explicit ScopedTlsSlot0Poison(void* replacement) noexcept {
        if (void** slots = module_tls_slots()) {
            saved = slots[0];
            slots[0] = replacement;
            active = true;
        }
    }

    ~ScopedTlsSlot0Poison() noexcept {
        // Re-read TEB->ThreadLocalStoragePointer because LoadLibrary during the scope can expand and reallocate
        // the TLS pointer array.
        if (active) {
            if (void** slots = module_tls_slots()) slots[0] = saved;
        }
    }

    ScopedTlsSlot0Poison(const ScopedTlsSlot0Poison&) = delete;
    ScopedTlsSlot0Poison& operator=(const ScopedTlsSlot0Poison&) = delete;
};

struct VirtualPage {
    void* ptr = nullptr;

    bool init(std::uint8_t fill, bool read_only) {
        ptr = VirtualAlloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!ptr) return false;
        if (fill != 0) std::memset(ptr, fill, 65536);
        if (read_only) {
            DWORD old = 0;
            if (!VirtualProtect(ptr, 65536, PAGE_READONLY, &old)) return false;
        }
        return true;
    }

    ~VirtualPage() {
        if (ptr) VirtualFree(ptr, 0, MEM_RELEASE);
    }
};

// Fixed-Huffman (RFC 1951 BTYPE=01) zlib stream for "leonida-fixed-huffman-tls0-safety-probe".
constexpr std::uint8_t kFixedHuffmanStream[] = {
    0x78, 0x01, 0xcb, 0x49, 0xcd, 0xcf, 0xcb, 0x4c, 0x49, 0xd4, 0x4d, 0xcb, 0xac, 0x48, 0x4d, 0xd1,
    0xcd, 0x28, 0x4d, 0x4b, 0xcb, 0x4d, 0xcc, 0xd3, 0x2d, 0xc9, 0x29, 0x36, 0xd0, 0x2d, 0x4e, 0x4c,
    0x4b, 0x2d, 0xa9, 0xd4, 0x2d, 0x28, 0xca, 0x4f, 0x4a, 0x05, 0x00, 0x29, 0xdd, 0x0e, 0xda,
};
constexpr const char kFixedHuffmanExpected[] = "leonida-fixed-huffman-tls0-safety-probe";

bool check_zlib_once() {
    std::vector<std::uint8_t> out;
    CHECK(leonida::zlib_inflate(kFixedHuffmanStream, out));
    const std::string got(out.begin(), out.end());
    CHECK(got == kFixedHuffmanExpected);
    return true;
}

struct InflateWorkerArgs {
    void* poison_page = nullptr;
    HANDLE start_event = nullptr;
    int id = 0;
    std::atomic<int>* passed = nullptr;
};

DWORD WINAPI inflate_and_stub_worker(LPVOID raw) {
    auto* args = static_cast<InflateWorkerArgs*>(raw);
    WaitForSingleObject(args->start_event, INFINITE);
    ScopedTlsSlot0Poison poison(args->poison_page);
    if (!poison.active) {
        fail_at(__LINE__, "TEB ThreadLocalStoragePointer is null");
        return 1;
    }
    for (int i = 0; i < 8; ++i) {
        if (!check_zlib_once()) return 1;
        rt::d3d12::stub("ID3D12TlsZeroTest::ConcurrentWorkerShared");
        if (i == 0 && args->id == 0) rt::d3d12::stub("ID3D12TlsZeroTest::Worker0");
        if (i == 0 && args->id == 1) rt::d3d12::stub("ID3D12TlsZeroTest::Worker1");
        if (i == 0 && args->id == 2) rt::d3d12::stub("ID3D12TlsZeroTest::Worker2");
        if (i == 0 && args->id == 3) rt::d3d12::stub("ID3D12TlsZeroTest::Worker3");
    }
    args->passed->fetch_add(1, std::memory_order_relaxed);
    return 0;
}

template<class Fn>
bool run_on_poisoned_thread(void* poison_page, Fn&& fn) {
    struct State {
        void* page;
        Fn* fn;
        bool ok = false;
    } state{poison_page, &fn, false};

    HANDLE thread = CreateThread(
        nullptr, 0,
        [](LPVOID raw) -> DWORD {
            auto* s = static_cast<State*>(raw);
            ScopedTlsSlot0Poison poison(s->page);
            if (!poison.active) {
                fail_at(__LINE__, "TEB ThreadLocalStoragePointer is null");
                return 1;
            }
            s->ok = (*s->fn)();
            return s->ok ? 0u : 1u;
        },
        &state, 0, nullptr);
    CHECK(thread != nullptr);
    const DWORD wait = WaitForSingleObject(thread, 30000);
    DWORD exit_code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    CHECK(wait == WAIT_OBJECT_0 && exit_code == 0 && state.ok);
    return true;
}

bool exercise_queue_holds() {
    class MockHolder final : public leonida::QueueHolds<std::uint64_t>::Holder {
    public:
        explicit MockHolder(leonida::QueueHolds<std::uint64_t>& holds) : holds_(holds) {}

        bool release_ready() override {
            if (released || holds_.pending(1, 1)) return false;
            released = true;
            ++release_calls;
            // Re-entrant pump_ready + remove_fence from inside release_ready exercises in_pump().
            holds_.pump_ready();
            holds_.remove_fence(2);
            held_seq = 0;
            holds_.held_changed(-1);
            return true;
        }
        void discard_waiting_on(std::uint64_t) override { ++discard_calls; }
        std::uint64_t oldest_held_seq() override { return held_seq; }

        leonida::QueueHolds<std::uint64_t>& holds_;
        std::uint64_t held_seq = 0;
        bool released = false;
        int release_calls = 0;
        int discard_calls = 0;
    };

    leonida::QueueHolds<std::uint64_t> holds;
    MockHolder holder(holds);
    holds.add_fence(1, 0);
    holds.add_fence(2, 0);
    holds.attach(&holder);

    holder.held_seq = holds.next_seq();
    holds.held_changed(1);

    int barrier_runs = 0;
    int owner_tag = 0;
    holds.after_held(&owner_tag, [&] { ++barrier_runs; });
    CHECK(barrier_runs == 0);

    holds.signaled(1, 1);
    holds.pump_ready();
    CHECK(holder.release_calls == 1);
    CHECK(barrier_runs == 1);
    CHECK(holder.discard_calls == 0);

    holds.remove_fence(1);
    holds.detach(&holder);
    return true;
}

bool test_cpu_paths(void* ro_zero_page, void* canary_a5_page) {
    rt::d3d12_trace::set_enabled(false);

    // 1. Single-threaded poisoned zlib fixed-Huffman inflate + QueueHolds + stub under READONLY zero page.
    CHECK(run_on_poisoned_thread(ro_zero_page, [&]() {
        CHECK(check_zlib_once());
        rt::d3d12::stub("ID3D12TlsZeroTest::SingleThread");
        CHECK(exercise_queue_holds());
        return true;
    }));

    // 2. QueueHolds under writable 0xA5 canary page: catches pre-existing truthy in_pump() read and verifies zero
    // guest TLS bytes are corrupted.
    CHECK(run_on_poisoned_thread(canary_a5_page, [&]() {
        CHECK(exercise_queue_holds());
        return true;
    }));
    const auto* bytes = static_cast<const std::uint8_t*>(canary_a5_page);
    for (std::size_t i = 0; i < 65536; ++i) {
        CHECK(bytes[i] == 0xA5);
    }

    // 3. Concurrent poisoned workers on zlib_inflate + d3d12_com::stub.
    HANDLE start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(start != nullptr);
    std::atomic<int> passed{0};
    InflateWorkerArgs args[4];
    HANDLE threads[4]{};
    for (int i = 0; i < 4; ++i) {
        args[i] = {ro_zero_page, start, i, &passed};
        threads[i] = CreateThread(nullptr, 0, inflate_and_stub_worker, &args[i], 0, nullptr);
        CHECK(threads[i] != nullptr);
    }
    SetEvent(start);
    const DWORD wait = WaitForMultipleObjects(4, threads, TRUE, 30000);
    for (HANDLE t : threads) CloseHandle(t);
    CloseHandle(start);
    CHECK(wait == WAIT_OBJECT_0 && passed.load() == 4);
    return true;
}

bool test_vulkan_device_paths(void* zero_page) {
    HRESULT poisoned_hr = E_FAIL;
    Ref<ID3D12Device> poisoned_dev;
    bool body_ok = false;

    // Must run FIRST on a poisoned thread before any unpoisoned call can initialize load_vulkan_library()!
    const bool thread_ran = run_on_poisoned_thread(zero_page, [&]() {
        poisoned_hr = rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&poisoned_dev.p));
        if (FAILED(poisoned_hr) || !poisoned_dev.p) {
            body_ok = true;  // inspect whether unpoisoned succeeds below
            return true;
        }

        // Open/Create ShaderCacheSession -> exercises process_shader_cache_registry().
        Ref<ID3D12Device9> dev9;
        CHECK(SUCCEEDED(poisoned_dev->QueryInterface(IID_PPV_ARGS(&dev9.p))) && dev9.p);
        const GUID cache_id = {0x6c42f11a, 0x9b08, 0x4d3c, {0xa5, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}};
        D3D12_SHADER_CACHE_SESSION_DESC cache_desc{};
        cache_desc.Identifier = cache_id;
        cache_desc.Mode = D3D12_SHADER_CACHE_MODE_MEMORY;
        cache_desc.Version = 1;
        Ref<ID3D12ShaderCacheSession> session;
        CHECK(SUCCEEDED(dev9->CreateShaderCacheSession(&cache_desc, IID_PPV_ARGS(&session.p))) && session.p);
        const std::uint32_t k = 7, v = 42;
        CHECK(session->StoreValue(&k, sizeof(k), &v, sizeof(v)) == S_OK);
        session.p->Release();
        session.p = nullptr;

        // Invalid CheckFeatureSupport -> exercises log_once in d3d12_vulkan_device.cpp.
        std::uint64_t dummy = 0;
        const HRESULT feat_hr = poisoned_dev->CheckFeatureSupport(static_cast<D3D12_FEATURE>(999999), &dummy, sizeof(dummy));
        CHECK(FAILED(feat_hr));

        // Command queue ExecuteCommandLists(1, nullptr) -> exercises note_once in d3d12_com.cpp.
        D3D12_COMMAND_QUEUE_DESC qdesc{};
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Ref<ID3D12CommandQueue> queue;
        CHECK(SUCCEEDED(poisoned_dev->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue.p))) && queue.p);
        queue->ExecuteCommandLists(1, nullptr);

        // Command list RSSetViewports(2, ...) -> exercises stub in d3d12_com.cpp via COM.
        Ref<ID3D12CommandAllocator> alloc;
        CHECK(SUCCEEDED(poisoned_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc.p))) &&
              alloc.p);
        Ref<ID3D12GraphicsCommandList> list;
        CHECK(SUCCEEDED(poisoned_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.p, nullptr,
                                                        IID_PPV_ARGS(&list.p))) &&
              list.p);
        D3D12_VIEWPORT vps[2]{{0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 8.0f, 8.0f, 0.0f, 1.0f}};
        list->RSSetViewports(2, vps);
        CHECK(SUCCEEDED(list->Close()));

        body_ok = true;
        return true;
    });
    CHECK(thread_ran && body_ok);

    if (FAILED(poisoned_hr)) {
        Ref<ID3D12Device> control_dev;
        const HRESULT control_hr = rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&control_dev.p));
        if (SUCCEEDED(control_hr)) {
            std::fprintf(stderr,
                         "FAIL: create_vulkan_device_standalone failed under TLS slot 0 poison (0x%08lx) but succeeded "
                         "unpoisoned\n",
                         static_cast<unsigned long>(poisoned_hr));
            ++g_failures;
            return false;
        }
        std::puts("note: no usable Vulkan device on this host; skipped Vulkan sub-check after CPU checks passed");
        return true;
    }

    std::puts("vulkan tls-slot-0 checks passed (device, shader cache, log_once, note_once, stub)");
    return true;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_anchor_sink = g_tls_slot0_anchor;

    VirtualPage ro_zero_page;
    VirtualPage canary_a5_page;
    if (!ro_zero_page.init(0, true) || !canary_a5_page.init(0xA5, false)) {
        std::fprintf(stderr, "VirtualAlloc/VirtualProtect failed\n");
        return 1;
    }

    const bool cpu_ok = test_cpu_paths(ro_zero_page.ptr, canary_a5_page.ptr);
    const bool vk_ok = cpu_ok && test_vulkan_device_paths(ro_zero_page.ptr);

    if (!cpu_ok || !vk_ok || g_failures.load() != 0) {
        std::fprintf(stderr, "d3d12_tls_safe_test: FAILED (%d failures)\n", g_failures.load());
        return 1;
    }
    std::puts("d3d12_tls_safe_test: PASS");
    return 0;
}
