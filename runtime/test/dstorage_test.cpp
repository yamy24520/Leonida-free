// DirectStorage shim (runtime/shims/dstorage.cpp): DStorageGetFactory resolved through the shim registry under its
// three module names, driven the way a console title calls it, over synthetic files in %TEMP%. No game data, no GPU.
#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "dstorage_xs_abi.h"
#include "file_system.h"
#include "shims.h"

using namespace dsx;
namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

void check_hr(HRESULT got, HRESULT want, const char* what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("FAIL: %s (got 0x%08lx, want 0x%08lx)\n", what, static_cast<unsigned long>(got),
                    static_cast<unsigned long>(want));
    }
}

// Interface ids a title might pass. The console's own ids are not public, so these are made up; the shim accepts any
// id per kind of object.
constexpr GUID kFactoryId = {0x0d5e0001, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x01}};
constexpr GUID kQueueId = {0x0d5e0002, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x02}};
constexpr GUID kFileId = {0x0d5e0003, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x03}};
constexpr GUID kStatusId = {0x0d5e0004, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x04}};
// IDStorageFactory of the PC API (dstorage.h): refused, its object has another shape.
constexpr GUID kPcFactoryId = {0x6924ea0c, 0xc3cd, 0x4826, {0xb1, 0x0a, 0xf6, 0x4f, 0x4e, 0xd9, 0x27, 0xc1}};

// A CPU fence with the PC ID3D12Fence vtable (the one the d3d12_xs fence proxies forward). It records the values it is
// signalled with and runs a check at each signal.
class TestFence final : public ID3D12Fence {
public:
    std::function<void(UINT64)> on_signal;
    std::atomic<UINT64> value{0};
    std::atomic<ULONG> refs{1};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid != __uuidof(IUnknown) && riid != __uuidof(ID3D12Fence)) return E_NOINTERFACE;
        AddRef();
        *ppv = this;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }  // lives on the test's stack
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void** ppv) override {
        if (ppv) *ppv = nullptr;
        return E_NOTIMPL;
    }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return value.load(); }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Signal(UINT64 v) override {
        if (on_signal) on_signal(v);
        value.store(v);
        return S_OK;
    }
};

std::vector<uint8_t> pattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed * 2654435761u + 1;
    for (auto& b : v) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b = static_cast<uint8_t>(x);
    }
    return v;
}

void write_file(const fs::path& p, const std::vector<uint8_t>& bytes) {
    fs::create_directories(p.parent_path());
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"wb") != 0 || !f) {
        std::printf("FAIL: cannot write %ls\n", p.c_str());
        std::exit(1);
    }
    if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

bool wait_complete(IDStorageStatusArrayX* s, UINT32 index, int ms = 10000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!s->IsComplete(index)) {
        if (std::chrono::steady_clock::now() > end) return false;
        Sleep(1);
    }
    return true;
}

bool wait_until(const std::function<bool()>& done, int ms = 10000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        Sleep(1);
    }
    return true;
}

DSTORAGE_REQUEST file_request(IDStorageFileX* f, uint64_t offset, void* dst, uint32_t n, uint64_t tag = 0,
                              const char* name = nullptr) {
    DSTORAGE_REQUEST r{};
    r.File = f;
    r.FileOffset = offset;
    r.Destination = dst;
    r.DestinationSize = n;
    r.SourceSize = n;
    r.CancellationTag = tag;
    r.Name = name;
    return r;
}

struct Env {
    fs::path root, game, overlay;
    IDStorageFactoryX2* factory = nullptr;
    std::vector<uint8_t> a_package, a_overlay, b, big;
};

IDStorageQueueX2* make_queue(Env& env, uint16_t capacity = 0x80, bool auto_submit = true,
                             DSTORAGE_REQUEST_SOURCE_TYPE type = DSTORAGE_REQUEST_SOURCE_FILE,
                             DSTORAGE_PRIORITY priority = DSTORAGE_PRIORITY_NORMAL, const char* name = "test queue") {
    DSTORAGE_QUEUE_DESC1 d{};
    d.SourceType = type;
    d.Capacity = capacity;
    d.Priority = priority;
    d.Name = name;
    d.Options.AutoSubmitDisabled = auto_submit ? 0 : 1;
    void* q = nullptr;
    const HRESULT hr = env.factory->CreateQueue1(&d, kQueueId, &q);
    check_hr(hr, S_OK, "CreateQueue1");
    return static_cast<IDStorageQueueX2*>(q);
}

IDStorageFileX* open(Env& env, const wchar_t* path) {
    void* f = nullptr;
    check_hr(env.factory->OpenFile(path, kFileId, &f), S_OK, "OpenFile");
    return static_cast<IDStorageFileX*>(f);
}

IDStorageStatusArrayX* make_status(Env& env, UINT32 capacity) {
    void* s = nullptr;
    check_hr(env.factory->CreateStatusArray(capacity, "test status", kStatusId, &s), S_OK, "CreateStatusArray");
    return static_cast<IDStorageStatusArrayX*>(s);
}

// ---- tests ----------------------------------------------------------------------------------------------------------

void test_layout_and_options() {
    DSTORAGE_REQUEST_OPTIONS o{};
    o.ZlibDecompress = 1;
    check(o.AsUINT64 == options::kZlib, "ZlibDecompress is bit 0");
    o.AsUINT64 = 0;
    o.BcpackMode = DSTORAGE_BCPACK_MODE_BC7;
    check(options::bcpack(o.AsUINT64) == 7 && o.AsUINT64 == (7ull << options::kBcpackShift), "BcpackMode is bits 1-4");
    o.AsUINT64 = 0;
    o.SwizzleMode = DSTORAGE_SWIZZLE_MODE_4K_12K_4K_12K;
    check(options::swizzle(o.AsUINT64) == 4 && o.AsUINT64 == (4ull << options::kSwizzleShift),
          "SwizzleMode is bits 5-20");
    o.AsUINT64 = 0;
    o.DestinationIsPhysicalPages = 1;
    check(o.AsUINT64 == options::kDestinationPages, "DestinationIsPhysicalPages is bit 21");
    o.AsUINT64 = 0;
    o.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    check(o.AsUINT64 == options::kSourceMemory, "SourceType is bit 22");
    o.AsUINT64 = 0;
    o.SourceIsPhysicalPages = 1;
    check(o.AsUINT64 == options::kSourcePages, "SourceIsPhysicalPages is bit 23");
    o.AsUINT64 = 0;
    o.Reserved = 1;
    check(o.AsUINT64 == (1ull << 24) && (o.AsUINT64 & options::kReservedMask), "Reserved starts at bit 24");
    DSTORAGE_QUEUE_OPTIONS q{};
    q.AutoSubmitDisabled = 1;
    check(q.AsUINT64 == 1, "AutoSubmitDisabled is bit 0");
}

void test_registry(Env& env) {
    const char* names[] = {"dstorage_xs.dll", "xcrdapi.dll", "dstorage.dll"};
    void* first = rt::shims::find("dstorage_xs.dll", "DStorageGetFactory");
    check(first != nullptr, "dstorage_xs.dll!DStorageGetFactory is registered");
    for (const char* n : names) {
        check(rt::shims::find(n, "DStorageGetFactory") == first, "every module name serves the same function");
        check(rt::shims::owns_dll(n), "the shims own the module (LoadLibrary gets a pseudo-handle)");
    }
    auto get = reinterpret_cast<DStorageGetFactoryFn>(first);
    if (!get) std::exit(1);

    check_hr(get(kFactoryId, nullptr), E_POINTER, "DStorageGetFactory without ppv");
    void* pc = reinterpret_cast<void*>(1);
    check_hr(get(kPcFactoryId, &pc), E_NOINTERFACE, "the PC IDStorageFactory id is refused");
    check(pc == nullptr, "a refused factory clears ppv");

    void* f1 = nullptr;
    void* f2 = nullptr;
    check_hr(get(kFactoryId, &f1), S_OK, "DStorageGetFactory");
    check_hr(reinterpret_cast<DStorageGetFactoryFn>(rt::shims::find("xcrdapi.dll", "DStorageGetFactory"))(
                 __uuidof(IUnknown), &f2),
             S_OK, "DStorageGetFactory(IUnknown) under xcrdapi.dll");
    check(f1 && f1 == f2, "the factory is one object");
    env.factory = static_cast<IDStorageFactoryX2*>(f1);
    static_cast<IUnknown*>(f2)->Release();
    check(env.factory->Release() >= 1 && env.factory->Release() >= 1, "the factory's count does not drop below one");
    env.factory->AddRef();

    void* again = nullptr;
    check_hr(env.factory->QueryInterface(kFactoryId, &again), S_OK, "factory QueryInterface for its own id");
    if (again) static_cast<IUnknown*>(again)->Release();
}

void test_factory_checks(Env& env) {
    // Before any queue or file exists.
    check_hr(env.factory->SetStagingBufferSize(17u << 20), E_DSTORAGE_INVALID_STAGING_BUFFER_SIZE, "staging 17 MiB");
    check_hr(env.factory->SetStagingBufferSize(16u << 20), S_OK, "staging 16 MiB");
    check_hr(env.factory->SetStagingBufferSize(0), S_OK, "staging 0");
    check_hr(env.factory->SetCpuAffinity(0x3c), S_OK, "SetCpuAffinity");
    env.factory->SetDebugFlags(DSTORAGE_DEBUG_SHOW_ERRORS);

    auto create = [&](DSTORAGE_REQUEST_SOURCE_TYPE type, uint16_t cap, int prio, uint64_t opts = 0) {
        DSTORAGE_QUEUE_DESC1 d{};
        d.SourceType = type;
        d.Capacity = cap;
        d.Priority = static_cast<DSTORAGE_PRIORITY>(prio);
        d.Options.AsUINT64 = opts;
        void* q = reinterpret_cast<void*>(1);
        const HRESULT hr = env.factory->CreateQueue1(&d, kQueueId, &q);
        if (SUCCEEDED(hr)) static_cast<IUnknown*>(q)->Release();
        else check(q == nullptr, "a failed CreateQueue1 clears ppv");
        return hr;
    };
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x7f, 0), E_DSTORAGE_INVALID_QUEUE_CAPACITY, "capacity 0x7f");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x2001, 0), E_DSTORAGE_INVALID_QUEUE_CAPACITY, "capacity 0x2001");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x80, 3), E_DSTORAGE_INVALID_QUEUE_PRIORITY, "priority 3");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x80, -2), E_DSTORAGE_INVALID_QUEUE_PRIORITY, "priority -2");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_MEMORY, 0x80, 0), E_DSTORAGE_INVALID_MEMORY_QUEUE_PRIORITY,
             "a memory queue must be realtime");
    check_hr(create(static_cast<DSTORAGE_REQUEST_SOURCE_TYPE>(2), 0x80, 0), E_DSTORAGE_INVALID_SOURCE_TYPE,
             "source type 2");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x80, 0, 2), E_DSTORAGE_RESERVED_FIELDS, "queue option bit 1");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x2000, -1), S_OK, "capacity 0x2000, low priority");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_MEMORY, 0x80, 2), S_OK, "a realtime memory queue");
    check_hr(env.factory->CreateQueue1(nullptr, kQueueId, nullptr), E_POINTER, "CreateQueue1 without ppv");
    void* q = nullptr;
    check_hr(env.factory->CreateQueue(nullptr, kQueueId, &q), E_INVALIDARG, "CreateQueue without a desc");
    check_hr(env.factory->CreateQueue1(nullptr, kQueueId, &q), E_INVALIDARG, "CreateQueue1 without a desc");
    void* s = nullptr;
    check_hr(env.factory->CreateStatusArray(0, nullptr, kStatusId, &s), E_INVALIDARG, "a status array of 0");

    // The id policy: an id seen for a queue is not a factory id.
    void* wrong = nullptr;
    check_hr(env.factory->QueryInterface(kQueueId, &wrong), E_NOINTERFACE, "a queue id is not a factory id");
    check_hr(env.factory->CreateStatusArray(4, nullptr, kFactoryId, &s), E_NOINTERFACE,
             "a factory id is not a status array id");

    // CreateQueue (DSTORAGE_QUEUE_DESC) and Query reflect the desc.
    DSTORAGE_QUEUE_DESC d{};
    d.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    d.Capacity = 0x100;
    d.Priority = DSTORAGE_PRIORITY_HIGH;
    d.Name = "named";
    check_hr(env.factory->CreateQueue(&d, kQueueId, &q), S_OK, "CreateQueue");
    auto* queue = static_cast<IDStorageQueueX2*>(q);
    DSTORAGE_QUEUE_INFO info{};
    queue->Query(&info);
    check(info.Desc.Capacity == 0x100 && info.Desc.Priority == DSTORAGE_PRIORITY_HIGH && info.Desc.Name == d.Name &&
              info.Desc.SourceType == DSTORAGE_REQUEST_SOURCE_FILE,
          "Query returns the creation desc");
    check(info.EmptySlotCount == 0xff, "an empty queue has capacity - 1 empty slots");
    check(info.RequestCountUntilAutoSubmit == 0x80, "auto-submit at half the capacity");
    DSTORAGE_QUEUE_INFO1 info1{};
    queue->Query1(&info1);
    check(info1.Desc.Options.AsUINT64 == 0 && info1.EmptySlotCount == 0xff, "Query1");
    check_hr(env.factory->SetStagingBufferSize(32u << 20), E_DSTORAGE_STAGING_BUFFER_LOCKED,
             "staging size is locked while a queue exists");
    void* qi = nullptr;
    check_hr(queue->QueryInterface(kQueueId, &qi), S_OK, "queue QueryInterface");
    if (qi) static_cast<IUnknown*>(qi)->Release();
    check(queue->Release() == 0, "the last queue reference frees it");
    check_hr(env.factory->SetStagingBufferSize(32u << 20), S_OK, "staging size unlocked again");
}

void test_open_paths(Env& env) {
    void* f = reinterpret_cast<void*>(1);
    check_hr(env.factory->OpenFile(L"GAME:\\data\\missing.bin", kFileId, &f), HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
             "a missing GAME: file");
    check(f == nullptr, "a failed OpenFile clears ppv");
    check_hr(env.factory->OpenFile(L"GAME:\\..\\outside.bin", kFileId, &f), HRESULT_FROM_WIN32(ERROR_INVALID_NAME),
             "a GAME: path that climbs out is refused");
    check_hr(env.factory->OpenFile(nullptr, kFileId, &f), E_INVALIDARG, "OpenFile without a path");
    check_hr(env.factory->OpenFile(L"GAME:\\data\\a.bin", kFileId, nullptr), E_POINTER, "OpenFile without ppv");

    // The overlay copy wins over the package; a host path is passed through.
    IDStorageFileX* a = open(env, L"GAME:\\data\\a.bin");
    IDStorageFileX* b = open(env, L"game:/data/b.bin");
    const std::wstring host = (env.game / L"data" / L"b.bin").wstring();
    IDStorageFileX* h = open(env, host.c_str());
    if (!a || !b || !h) std::exit(1);

    IDStorageQueueX2* q = make_queue(env);
    IDStorageStatusArrayX* s = make_status(env, 4);
    std::vector<uint8_t> da(4096), db(1000), dh(1000);
    DSTORAGE_REQUEST r = file_request(a, 0, da.data(), 4096);
    q->EnqueueRequest(&r);
    r = file_request(b, 500, db.data(), 1000);
    q->EnqueueRequest(&r);
    r = file_request(h, 500, dh.data(), 1000);
    q->EnqueueRequest(&r);
    q->EnqueueStatus(s, 0);
    check_hr(s->GetHResult(0), E_PENDING, "a status entry is pending once enqueued");
    q->Submit();
    check(wait_complete(s, 0), "the reads complete");
    check_hr(s->GetHResult(0), S_OK, "the reads succeed");
    check(std::memcmp(da.data(), env.a_overlay.data(), 4096) == 0 && da != env.a_package,
          "GAME: reads the overlay copy, not the package");

    // GetHandle: a handle of its own (ReOpenFile), so its file position does not follow the shim's reads, which
    // just ended at byte 4096 of this file.
    HANDLE own = a->GetHandle();
    LARGE_INTEGER size{};
    check(own != INVALID_HANDLE_VALUE && GetFileSizeEx(own, &size) && size.QuadPart == 5000,
          "GetHandle names the overlay copy (5000 bytes, the package has 4096)");
    std::vector<uint8_t> via_handle(6000);
    DWORD got = 0;
    check(own != INVALID_HANDLE_VALUE && ReadFile(own, via_handle.data(), 6000, &got, nullptr) && got == 5000 &&
              std::memcmp(via_handle.data(), env.a_overlay.data(), 5000) == 0,
          "a ReadFile through GetHandle's handle starts at byte 0 and reads the overlay copy");
    if (own != INVALID_HANDLE_VALUE) CloseHandle(own);
    r = file_request(a, 100, da.data(), 50);
    q->EnqueueRequest(&r);
    q->EnqueueStatus(s, 1);
    q->Submit();
    check(wait_complete(s, 1) && std::memcmp(da.data(), env.a_overlay.data() + 100, 50) == 0,
          "the shim's reads do not follow a GetHandle handle's position either");
    check(std::memcmp(db.data(), env.b.data() + 500, 1000) == 0,
          "a GAME: read at an offset (forward slashes, any case)");
    check(db == dh, "a host path reads the same file");
    a->Release();
    b->Release();
    h->Release();
    s->Release();
    q->Release();
}

void test_reads_and_errors(Env& env) {
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");  // 10,000 bytes
    IDStorageFileX* big = open(env, L"GAME:\\data\\big.bin");
    IDStorageQueueX2* q = make_queue(env, 0x80, false);
    IDStorageStatusArrayX* s = make_status(env, 32);
    HANDLE error_event = q->GetErrorEvent();
    check(error_event != nullptr, "GetErrorEvent returns a handle");

    std::vector<uint8_t> big_out(env.big.size());
    std::vector<uint8_t> few(64, 0xAB);
    std::vector<uint8_t> zero(16, 0xCD);
    struct Case {
        DSTORAGE_REQUEST r;
        HRESULT want;
        const char* what;
    };
    std::vector<Case> cases;
    cases.push_back({file_request(big, 0, big_out.data(), static_cast<uint32_t>(big_out.size())), S_OK,
                     "a 3 MiB read"});
    cases.push_back({file_request(b, 10000, zero.data(), 0), S_OK, "a zero-size read at the end of the file"});
    cases.push_back({file_request(b, 9990, few.data(), 11, 0x77, "past-the-end"), E_DSTORAGE_END_OF_FILE,
                     "a read one byte past the end"});
    cases.push_back({file_request(b, ~0ull - 4, few.data(), 8), E_DSTORAGE_END_OF_FILE, "an offset near 2^64"});
    cases.push_back({file_request(b, 0, few.data(), 8), S_OK, "a good read after failures"});
    {
        DSTORAGE_REQUEST r = file_request(b, 0, few.data(), 8);
        r.DestinationSize = 16;
        cases.push_back({r, E_DSTORAGE_INVALID_DESTINATION_SIZE, "DestinationSize differs from SourceSize"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.ZlibDecompress = 1;
        cases.push_back({r, E_DSTORAGE_INVALID_DESTINATION_SIZE, "zlib with DestinationSize equal to SourceSize"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.BcpackMode = DSTORAGE_BCPACK_MODE_BC1;
        cases.push_back({r, E_NOTIMPL, "BCPack is not implemented yet"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.SwizzleMode = DSTORAGE_SWIZZLE_MODE_4K_4K_8K;
        cases.push_back({r, E_NOTIMPL, "swizzle is not implemented yet"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.AsUINT64 = 8ull << options::kBcpackShift;
        cases.push_back({r, E_DSTORAGE_INVALID_BCPACK_MODE, "BCPack mode 8"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.AsUINT64 = 5ull << options::kSwizzleShift;
        cases.push_back({r, E_DSTORAGE_INVALID_SWIZZLE_MODE, "swizzle mode 5"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.Reserved = 1;
        cases.push_back({r, E_DSTORAGE_RESERVED_FIELDS, "a reserved option bit"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.DestinationIsPhysicalPages = 1;
        cases.push_back({r, E_NOTIMPL, "physical pages are not implemented yet"});
        r = file_request(b, 0, few.data(), 8);
        r.IntermediateSize = 4;
        cases.push_back({r, E_DSTORAGE_INVALID_INTERMEDIATE_SIZE, "IntermediateSize without zlib and BCPack"});
        r = file_request(b, 0, nullptr, 8);
        cases.push_back({r, E_INVALIDARG, "a null destination"});
        r = file_request(nullptr, 0, few.data(), 8);
        cases.push_back({r, E_INVALIDARG, "a null file"});
        r = file_request(reinterpret_cast<IDStorageFileX*>(s), 0, few.data(), 8);
        cases.push_back({r, E_DSTORAGE_INVALID_FILE_HANDLE, "a status array passed as a file"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        cases.push_back({r, E_DSTORAGE_INVALID_SOURCE_TYPE, "a memory request on a file queue"});
    }
    cases.push_back({file_request(b, 0, nullptr, 0), E_INVALIDARG, "a zero-size read still needs a destination"});
    cases.push_back({file_request(nullptr, 0, zero.data(), 0), E_INVALIDARG, "a zero-size read still needs a file"});
    void* reserved = VirtualAlloc(nullptr, 1 << 16, MEM_RESERVE, PAGE_READWRITE);
    cases.push_back({file_request(b, 0, reserved, 4096), E_DSTORAGE_ACCESS_VIOLATION,
                     "a destination the process cannot write"});
    check(cases.size() <= 31, "the cases fit the status array");

    for (UINT32 i = 0; i < cases.size(); ++i) {
        q->EnqueueRequest(&cases[i].r);
        q->EnqueueStatus(s, i);
    }
    Sleep(30);
    check(!s->IsComplete(0), "nothing runs before Submit when auto-submit is disabled");
    DSTORAGE_QUEUE_INFO1 info{};
    q->Query1(&info);
    check(info.RequestCountUntilAutoSubmit == 0, "no auto-submit count when it is disabled");
    check(info.EmptySlotCount == 0x7f - 2 * cases.size(), "EmptySlotCount counts every entry");
    q->Submit();
    const UINT32 last = static_cast<UINT32>(cases.size() - 1);
    check(wait_complete(s, last), "all cases complete");
    for (UINT32 i = 0; i < cases.size(); ++i) check_hr(s->GetHResult(i), cases[i].want, cases[i].what);
    check(big_out == env.big, "the 3 MiB read has the file's bytes");
    check(zero == std::vector<uint8_t>(16, 0xCD), "a zero-size read writes nothing");
    check(std::memcmp(few.data(), env.b.data(), 8) == 0, "the good read after failures");

    check(WaitForSingleObject(error_event, 0) == WAIT_OBJECT_0, "the error event is set");
    DSTORAGE_ERROR_RECORD rec{};
    q->RetrieveErrorRecord(&rec);
    size_t failures = 0;
    for (const auto& c : cases) failures += FAILED(c.want) ? 1 : 0;
    check(rec.FailureCount == failures, "FailureCount counts every failed request");
    check_hr(rec.FirstFailure.HResult, E_DSTORAGE_END_OF_FILE, "the first failure is recorded");
    check(rec.FirstFailure.CommandType == DSTORAGE_COMMAND_TYPE_REQUEST, "its command type is REQUEST");
    check(rec.FirstFailure.Request.FileOffset == 9990 && rec.FirstFailure.Request.SourceSize == 11 &&
              rec.FirstFailure.Request.CancellationTag == 0x77,
          "the record has the request's offset, size and tag");
    check(std::wcscmp(rec.FirstFailure.Request.Filename, L"GAME:\\data\\b.bin") == 0,
          "the record names the file as the title opened it");
    check(std::strcmp(rec.FirstFailure.Request.Name, "past-the-end") == 0, "the record has the request's name");
    check(WaitForSingleObject(error_event, 0) == WAIT_TIMEOUT, "RetrieveErrorRecord resets the event");
    q->RetrieveErrorRecord(&rec);
    check(rec.FailureCount == 0 && rec.FirstFailure.CommandType == DSTORAGE_COMMAND_TYPE_NONE,
          "a second RetrieveErrorRecord has nothing");

    // Status and fence entries that fail.
    q->EnqueueStatus(s, 1000);
    q->EnqueueStatus(nullptr, 0);
    q->EnqueueSignal(nullptr, 1);
    q->EnqueueStatus(s, 31);
    q->Submit();
    check(wait_complete(s, 31), "the entries after the failed notifications complete");
    check_hr(s->GetHResult(31), S_OK, "notification failures do not fail the next status entry");
    check_hr(s->GetHResult(1000), E_DSTORAGE_INDEX_BOUND, "GetHResult past the array");
    check(s->IsComplete(1000), "IsComplete past the array is not pending");
    q->RetrieveErrorRecord(&rec);
    check(rec.FailureCount == 3 && rec.FirstFailure.CommandType == DSTORAGE_COMMAND_TYPE_STATUS &&
              rec.FirstFailure.HResult == E_DSTORAGE_INDEX_BOUND && rec.FirstFailure.Status.Index == 1000 &&
              rec.FirstFailure.Status.StatusArray == s,
          "an index past the array is recorded as a STATUS failure");

    VirtualFree(reserved, 0, MEM_RELEASE);
    CloseHandle(error_event);
    s->Release();
    q->Release();
    b->Release();
    big->Release();
}

void test_fence_order_and_events(Env& env) {
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");
    IDStorageQueueX2* q = make_queue(env, 0x200);
    TestFence fence;
    constexpr int kReads = 100;
    std::vector<std::vector<uint8_t>> outs(kReads, std::vector<uint8_t>(100));
    std::atomic<bool> in_place{false};
    fence.on_signal = [&](UINT64 v) {
        bool ok = v == 7;
        for (int i = 0; i < kReads; ++i) ok = ok && std::memcmp(outs[i].data(), env.b.data() + i * 100, 100) == 0;
        in_place.store(ok);
    };
    for (int i = 0; i < kReads; ++i) {
        DSTORAGE_REQUEST r = file_request(b, static_cast<uint64_t>(i) * 100, outs[i].data(), 100);
        q->EnqueueRequest(&r);
    }
    q->EnqueueSignal(&fence, 7);
    check(fence.refs.load() == 2, "an enqueued signal holds a fence reference");
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    q->EnqueueSetEvent(ev);
    q->Submit();
    check(WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0, "EnqueueSetEvent sets the event");
    check(fence.GetCompletedValue() == 7, "EnqueueSignal signals the fence with its value");
    check(in_place.load(), "the fence is signalled after every read before it is in place");
    check(wait_until([&] { return fence.refs.load() == 1; }), "the fence reference is released after the signal");

    // A failed request before a fence: the fence still signals, and the status after the fence is clean.
    IDStorageStatusArrayX* s = make_status(env, 2);
    std::vector<uint8_t> tmp(8);
    DSTORAGE_REQUEST bad = file_request(b, 20000, tmp.data(), 8);
    q->EnqueueRequest(&bad);
    q->EnqueueSignal(&fence, 8);
    q->EnqueueStatus(s, 0);
    q->Submit();
    check(wait_complete(s, 0), "the status after a fence completes");
    check_hr(s->GetHResult(0), S_OK, "a failure before a fence is not reported by a status after it");
    check(fence.GetCompletedValue() == 8, "a fence after a failed read still signals");
    CloseHandle(ev);
    s->Release();
    q->Release();
    b->Release();
}

void test_cancel(Env& env) {
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");
    IDStorageQueueX2* q = make_queue(env, 0x80, false);
    IDStorageStatusArrayX* s = make_status(env, 1);
    std::vector<std::vector<uint8_t>> outs(6, std::vector<uint8_t>(16, 0));
    const uint64_t tags[] = {0x11, 0x12, 0x21, 0x22, 0x31};
    for (int i = 0; i < 5; ++i) {
        DSTORAGE_REQUEST r = file_request(b, 0, outs[i].data(), 16, tags[i]);
        q->EnqueueRequest(&r);
    }
    q->CancelRequestsWithTag(0xF0, 0x10);  // 0x11 and 0x12
    q->CancelRequestsWithTag(0xFF, 0x22);  // 0x22
    DSTORAGE_REQUEST later = file_request(b, 0, outs[5].data(), 16, 0x11);  // enqueued after: not cancelled
    q->EnqueueRequest(&later);
    q->EnqueueStatus(s, 0);
    q->Submit();
    check(wait_complete(s, 0), "cancelled requests complete");
    check_hr(s->GetHResult(0), S_OK, "a cancelled request is a success");
    const std::vector<uint8_t> zeros(16, 0);
    check(outs[0] == zeros && outs[1] == zeros && outs[3] == zeros, "cancelled requests read nothing");
    check(std::memcmp(outs[2].data(), env.b.data(), 16) == 0 && std::memcmp(outs[4].data(), env.b.data(), 16) == 0,
          "requests the mask did not match are read");
    check(std::memcmp(outs[5].data(), env.b.data(), 16) == 0, "a request enqueued after the cancel is read");
    s->Release();
    q->Release();
    b->Release();
}

void test_auto_submit_and_full_queue(Env& env) {
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");
    IDStorageQueueX2* q = make_queue(env, 0x80, true);
    std::vector<std::vector<uint8_t>> outs(64, std::vector<uint8_t>(8, 0));
    for (int i = 0; i < 63; ++i) {
        DSTORAGE_REQUEST r = file_request(b, i, outs[i].data(), 8);
        q->EnqueueRequest(&r);
    }
    DSTORAGE_QUEUE_INFO info{};
    q->Query(&info);
    check(info.RequestCountUntilAutoSubmit == 1, "one entry before the automatic submit");
    Sleep(20);
    check(outs[0] == std::vector<uint8_t>(8, 0), "below half the capacity nothing is submitted");
    DSTORAGE_REQUEST r = file_request(b, 63, outs[63].data(), 8);
    q->EnqueueRequest(&r);  // the 64th: half of 0x80
    check(wait_until([&] { return std::memcmp(outs[63].data(), env.b.data() + 63, 8) == 0; }),
          "reaching half the capacity submits without Submit");
    bool all = true;
    for (int i = 0; i < 64; ++i) all = all && std::memcmp(outs[i].data(), env.b.data() + i, 8) == 0;
    check(all, "every auto-submitted read has its bytes");
    q->Release();

    // Auto-submit disabled and full: an enqueue waits until another thread submits.
    q = make_queue(env, 0x80, false);
    IDStorageStatusArrayX* s = make_status(env, 1);
    constexpr int kMany = 400;
    std::vector<uint8_t> sink(kMany * 4);
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (int i = 0; i < kMany; ++i) {
            DSTORAGE_REQUEST rr = file_request(b, i, sink.data() + i * 4, 4);
            q->EnqueueRequest(&rr);
        }
        q->EnqueueStatus(s, 0);
        done.store(true);
    });
    check(wait_until([&] {
              DSTORAGE_QUEUE_INFO i{};
              q->Query(&i);
              return i.EmptySlotCount == 0;
          }),
          "the queue fills to capacity - 1");
    Sleep(20);
    check(!done.load(), "an enqueue into a full queue waits");
    while (!done.load()) {
        q->Submit();
        Sleep(1);
    }
    producer.join();
    q->Submit();
    check(wait_complete(s, 0), "the producer's reads complete");
    bool same = true;
    for (int i = 0; i < kMany; ++i) same = same && std::memcmp(sink.data() + i * 4, env.b.data() + i, 4) == 0;
    check(same, "every read of the full-queue run has its bytes");
    s->Release();
    q->Release();
    b->Release();
}

void test_memory_queue(Env& env) {
    IDStorageQueueX2* q = make_queue(env, 0x80, true, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
    IDStorageStatusArrayX* s = make_status(env, 6);
    std::vector<uint8_t> buf = pattern(8192, 9);
    std::vector<uint8_t> want = buf;
    std::memmove(want.data() + 1024, want.data(), 4096);
    DSTORAGE_REQUEST r{};
    r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    r.Source = buf.data();
    r.Destination = buf.data() + 1024;
    r.SourceSize = r.DestinationSize = 4096;
    q->EnqueueRequest(&r);
    q->EnqueueStatus(s, 0);
    void* reserved = VirtualAlloc(nullptr, 1 << 16, MEM_RESERVE, PAGE_READWRITE);
    DSTORAGE_REQUEST bad = r;
    bad.Destination = reserved;
    q->EnqueueRequest(&bad);
    q->EnqueueStatus(s, 1);
    DSTORAGE_REQUEST file_on_memory{};  // a file request on a memory queue
    file_on_memory.Destination = buf.data();
    file_on_memory.SourceSize = file_on_memory.DestinationSize = 4;
    q->EnqueueRequest(&file_on_memory);
    q->EnqueueStatus(s, 2);
    DSTORAGE_REQUEST huge = r;
    huge.SourceSize = huge.DestinationSize = (1u << 29) + 1;  // more than 1 GiB together
    q->EnqueueRequest(&huge);
    q->EnqueueStatus(s, 3);
    // Buffers over 32 MiB need 64 KiB or 2 MiB pages (DSTORAGE_REQUEST). Reserved memory: a copy that is not refused
    // faults and reports an access violation instead.
    constexpr uint32_t k32MiB = 32u << 20;
    void* wide = VirtualAlloc(nullptr, 2 * (k32MiB + 4096), MEM_RESERVE, PAGE_READWRITE);
    check(wide != nullptr, "reserve 64 MiB for the over-limit copy");
    DSTORAGE_REQUEST over = r;
    over.Source = wide;
    over.Destination = static_cast<uint8_t*>(wide) + k32MiB + 4096;
    over.SourceSize = over.DestinationSize = k32MiB + 1;
    q->EnqueueRequest(&over);
    q->EnqueueStatus(s, 4);
    std::vector<uint8_t> at_limit_source = pattern(k32MiB, 10), at_limit(k32MiB, 0);
    DSTORAGE_REQUEST limit = r;
    limit.Source = at_limit_source.data();
    limit.Destination = at_limit.data();
    limit.SourceSize = limit.DestinationSize = k32MiB;
    q->EnqueueRequest(&limit);
    q->EnqueueStatus(s, 5);
    q->Submit();
    check(wait_complete(s, 5), "memory requests complete");
    check_hr(s->GetHResult(4), E_DSTORAGE_REQUEST_TOO_LARGE, "an unpaged buffer over 32 MiB is refused untouched");
    check_hr(s->GetHResult(5), S_OK, "a 32 MiB unpaged copy");
    check(at_limit == at_limit_source, "the 32 MiB copy moved every byte");
    VirtualFree(wide, 0, MEM_RELEASE);
    check_hr(s->GetHResult(0), S_OK, "a memory-to-memory copy");
    check(buf == want, "an overlapping memory copy moves like memmove");
    check_hr(s->GetHResult(1), E_DSTORAGE_ACCESS_VIOLATION, "a memory copy to memory the process cannot write");
    check_hr(s->GetHResult(2), E_DSTORAGE_INVALID_SOURCE_TYPE, "a file request on a memory queue");
    check_hr(s->GetHResult(3), E_DSTORAGE_REQUEST_TOO_LARGE, "source plus destination over 1 GiB");
    VirtualFree(reserved, 0, MEM_RELEASE);
    s->Release();
    q->Release();
}

void test_close_and_release(Env& env) {
    IDStorageFileX* big = open(env, L"GAME:\\data\\big.bin");
    // Close with requests in flight: it returns after they all ran.
    IDStorageQueueX2* q = make_queue(env, 0x100, true);
    constexpr int kReads = 300;
    constexpr uint32_t kChunk = 8192;
    std::vector<uint8_t> out(kReads * kChunk);
    for (int i = 0; i < kReads; ++i) {
        DSTORAGE_REQUEST r = file_request(big, static_cast<uint64_t>(i) * kChunk, out.data() + i * kChunk, kChunk);
        q->EnqueueRequest(&r);
    }
    q->Close();
    check(std::memcmp(out.data(), env.big.data(), out.size()) == 0, "Close returns after every enqueued read ran");
    IDStorageStatusArrayX* s = make_status(env, 1);
    std::vector<uint8_t> tmp(8, 0);
    DSTORAGE_REQUEST r = file_request(big, 0, tmp.data(), 8);
    q->EnqueueRequest(&r);
    q->EnqueueStatus(s, 0);
    check(s->IsComplete(0), "a status enqueued after Close completes");
    check_hr(s->GetHResult(0), E_DSTORAGE_QUEUE_CLOSED, "a status enqueued after Close reports the closed queue");
    q->Submit();
    q->Close();  // a second Close is harmless
    Sleep(20);
    check(tmp == std::vector<uint8_t>(8, 0), "a request enqueued after Close is not run");
    DSTORAGE_ERROR_RECORD rec{};
    q->RetrieveErrorRecord(&rec);
    check(rec.FailureCount == 2 && rec.FirstFailure.HResult == E_DSTORAGE_QUEUE_CLOSED,
          "enqueues after Close are recorded");
    check(q->Release() == 0, "a closed queue is freed by its last Release");

    // The last Release with requests in flight finishes them first.
    q = make_queue(env, 0x100, true);
    std::fill(out.begin(), out.end(), uint8_t{0});
    for (int i = 0; i < kReads; ++i) {
        DSTORAGE_REQUEST rr = file_request(big, static_cast<uint64_t>(i) * kChunk, out.data() + i * kChunk, kChunk);
        q->EnqueueRequest(&rr);
    }
    q->Submit();
    big->Release();  // the requests hold the file
    check(q->Release() == 0, "the last Release of a busy queue");
    check(std::memcmp(out.data(), env.big.data(), out.size()) == 0, "the last Release ran the reads in flight");

    // IDStorageFileX::Close: submitted reads finish, a later one fails with E_DSTORAGE_FILE_NOT_OPEN.
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");
    q = make_queue(env, 0x80, false);
    IDStorageStatusArrayX* s2 = make_status(env, 3);
    std::vector<uint8_t> first(100, 0), second(100, 0);
    DSTORAGE_REQUEST r1 = file_request(b, 0, first.data(), 100);
    q->EnqueueRequest(&r1);
    q->EnqueueStatus(s2, 0);
    q->Submit();
    DSTORAGE_REQUEST r2 = file_request(b, 0, second.data(), 100);
    q->EnqueueRequest(&r2);  // not submitted yet
    b->Close();
    check(std::memcmp(first.data(), env.b.data(), 100) == 0,
          "IDStorageFileX::Close waits for the file's submitted reads");
    check(wait_complete(s2, 0) && s2->GetHResult(0) == S_OK, "the submitted read succeeded");
    check(b->GetHandle() == INVALID_HANDLE_VALUE, "GetHandle on a closed file");
    q->EnqueueStatus(s2, 1);
    DSTORAGE_REQUEST r3 = file_request(b, 0, second.data(), 0);
    q->EnqueueRequest(&r3);
    q->EnqueueStatus(s2, 2);
    q->Submit();
    check(wait_complete(s2, 2), "the reads after the close complete");
    check_hr(s2->GetHResult(1), E_DSTORAGE_FILE_NOT_OPEN, "a read of a closed file");
    check_hr(s2->GetHResult(2), E_DSTORAGE_FILE_NOT_OPEN, "a zero-size read of a closed file");
    check(second == std::vector<uint8_t>(100, 0), "a read of a closed file writes nothing");
    q->Release();
    b->Release();
    s->Release();
    s2->Release();
}

void test_threads(Env& env) {
    IDStorageFileX* big = open(env, L"GAME:\\data\\big.bin");
    IDStorageQueueX2* q = make_queue(env, 0x80, true);
    constexpr int kThreads = 4, kPer = 250;
    constexpr uint32_t kChunk = 512;
    std::vector<std::vector<uint8_t>> outs(kThreads, std::vector<uint8_t>(kPer * kChunk));
    std::vector<IDStorageStatusArrayX*> arrays;
    for (int t = 0; t < kThreads; ++t) arrays.push_back(make_status(env, 1));
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPer; ++i) {
                const uint64_t offset = (static_cast<uint64_t>(t) * kPer + i) * kChunk % (env.big.size() - kChunk);
                DSTORAGE_REQUEST r = file_request(big, offset, outs[t].data() + i * kChunk, kChunk, t);
                q->EnqueueRequest(&r);
            }
            q->EnqueueStatus(arrays[t], 0);
            q->Submit();
        });
    for (auto& th : threads) th.join();
    bool ok = true;
    for (int t = 0; t < kThreads; ++t) {
        ok = ok && wait_complete(arrays[t], 0) && arrays[t]->GetHResult(0) == S_OK;
        for (int i = 0; i < kPer; ++i) {
            const uint64_t offset = (static_cast<uint64_t>(t) * kPer + i) * kChunk % (env.big.size() - kChunk);
            ok = ok && std::memcmp(outs[t].data() + i * kChunk, env.big.data() + offset, kChunk) == 0;
        }
    }
    check(ok, "four threads enqueueing at once all read their bytes");
    for (auto* a : arrays) a->Release();
    q->Release();
    big->Release();
}

void test_iid_policy_events_and_cancel(Env& env) {
    // Well-known system interfaces and made-up ids: QueryInterface refuses all of them.
    const GUID kRandom = {0x5eed0001, 0x2222, 0x4222, {0x82, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x01}};
    const GUID kIMarshal = {0x00000003, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    const GUID kIAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
    const GUID kINoMarshal = {0xecc8691b, 0xc1db, 0x4dc0, {0x85, 0x5e, 0x65, 0xf6, 0xc5, 0x51, 0xaf, 0x49}};
    const GUID kIClientSecurity = {0x0000013d, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    const GUID kPcStatusArrayId = {0x82397587, 0x7cd5, 0x453b, {0xa0, 0x2e, 0x31, 0x37, 0x9b, 0xd6, 0x46, 0x56}};
    const GUID kPcQueueId = {0xcfdbd83f, 0x9e06, 0x4fda, {0x8e, 0xa5, 0x69, 0x04, 0x21, 0x37, 0xf4, 0x9b}};
    const GUID kQueueId2 = {0x0d5e0012, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x12}};
    const GUID refused[] = {kRandom, kIMarshal, kIAgileObject, kINoMarshal, kIClientSecurity};

    IDStorageQueueX2* q = make_queue(env, 0x80, false);
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");
    IDStorageStatusArrayX* s = make_status(env, 8);
    IUnknown* objects[] = {env.factory, q, b, s};
    for (IUnknown* o : objects)
        for (const GUID& g : refused) {
            void* out = reinterpret_cast<void*>(1);
            check_hr(o->QueryInterface(g, &out), E_NOINTERFACE, "QueryInterface refuses an unknown or system id");
            check(out == nullptr, "a refused QueryInterface clears ppv");
        }
    void* out = nullptr;
    check_hr(q->QueryInterface(kFileId, &out), E_NOINTERFACE, "a queue refuses a file id");
    check_hr(b->QueryInterface(kQueueId, &out), E_NOINTERFACE, "a file refuses a queue id");
    check_hr(s->QueryInterface(kFactoryId, &out), E_NOINTERFACE, "a status array refuses a factory id");
    check_hr(q->QueryInterface(__uuidof(IUnknown), &out), S_OK, "QueryInterface(IUnknown)");
    if (out) static_cast<IUnknown*>(out)->Release();

    // Creating calls learn a new id but refuse system ones.
    auto get = reinterpret_cast<DStorageGetFactoryFn>(rt::shims::find("dstorage_xs.dll", "DStorageGetFactory"));
    check_hr(get(kIMarshal, &out), E_NOINTERFACE, "DStorageGetFactory refuses IMarshal");
    check_hr(get(kIAgileObject, &out), E_NOINTERFACE, "DStorageGetFactory refuses IAgileObject");
    DSTORAGE_QUEUE_DESC1 d{};
    d.Capacity = 0x80;
    check_hr(env.factory->CreateQueue1(&d, kINoMarshal, &out), E_NOINTERFACE, "CreateQueue1 refuses INoMarshal");
    check_hr(env.factory->OpenFile(L"GAME:\\data\\b.bin", kIClientSecurity, &out), E_NOINTERFACE,
             "OpenFile refuses IClientSecurity");
    check_hr(env.factory->CreateQueue1(&d, kPcQueueId, &out), E_NOINTERFACE, "CreateQueue1 refuses the PC queue id");
    check_hr(env.factory->CreateQueue1(&d, kPcStatusArrayId, &out), E_NOINTERFACE,
             "the status array id is not a queue id");
    check_hr(env.factory->CreateQueue1(&d, kQueueId2, &out), S_OK, "a creating call learns a new queue id");
    if (out) static_cast<IUnknown*>(out)->Release();
    check_hr(q->QueryInterface(kQueueId2, &out), S_OK, "QueryInterface accepts an id a queue was created with");
    if (out) static_cast<IUnknown*>(out)->Release();

    // The PC IDStorageStatusArray id has the console interface's shape and is accepted.
    check_hr(env.factory->CreateStatusArray(2, nullptr, kPcStatusArrayId, &out), S_OK,
             "CreateStatusArray accepts the PC IDStorageStatusArray id");
    if (out) static_cast<IUnknown*>(out)->Release();
    check_hr(s->QueryInterface(kPcStatusArrayId, &out), S_OK, "a status array answers the PC IDStorageStatusArray id");
    if (out) static_cast<IUnknown*>(out)->Release();

    // EnqueueSetEvent duplicates the handle: closing the title's handle, even with its value reused, changes nothing.
    HANDLE first = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    q->EnqueueSetEvent(first);
    CloseHandle(first);
    HANDLE second = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // may get the same handle value
    q->EnqueueSetEvent(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x7ffffff0)));  // not a handle
    q->EnqueueSetEvent(nullptr);
    q->EnqueueStatus(s, 0);
    q->Submit();
    check(wait_complete(s, 0), "the event entries complete");
    check(WaitForSingleObject(second, 0) == WAIT_TIMEOUT, "a closed event's reused handle value is not set");
    DSTORAGE_ERROR_RECORD rec{};
    q->RetrieveErrorRecord(&rec);
    check(rec.FailureCount == 2 && rec.FirstFailure.CommandType == kCommandTypeEvent &&
              FAILED(rec.FirstFailure.HResult),
          "an invalid and a null event handle are recorded as EVENT failures; the closed one is not");
    CloseHandle(second);

    // A cancelled request that also had an enqueue error is still a success, and not counted as a failure.
    std::vector<uint8_t> tmp(8, 0);
    DSTORAGE_REQUEST foreign = file_request(reinterpret_cast<IDStorageFileX*>(s), 0, tmp.data(), 8, 0x99);
    q->EnqueueRequest(&foreign);
    DSTORAGE_REQUEST null_file = file_request(nullptr, 0, tmp.data(), 8, 0x99);
    q->EnqueueRequest(&null_file);
    q->CancelRequestsWithTag(~0ull, 0x99);
    q->EnqueueStatus(s, 1);
    q->Submit();
    check(wait_complete(s, 1), "cancelled bad requests complete");
    check_hr(s->GetHResult(1), S_OK, "a cancelled request with an enqueue error is a success");
    q->RetrieveErrorRecord(&rec);
    check(rec.FailureCount == 0, "a cancelled request is not counted as a failure");

    s->Release();
    b->Release();
    q->Release();
}

// Holds the queue's worker inside a copy: the copy's source is a guard page, and this handler waits on the first
// touch until the test releases it. The guard is gone after that fault, so the copy then goes on.
struct GuardHold {
    uint8_t* page = nullptr;
    HANDLE entered = nullptr;
    HANDLE release = nullptr;
};
GuardHold* g_hold = nullptr;

LONG CALLBACK hold_on_guard(EXCEPTION_POINTERS* x) {
    const EXCEPTION_RECORD* e = x->ExceptionRecord;
    if (!g_hold || e->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION || e->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto* address = reinterpret_cast<const uint8_t*>(e->ExceptionInformation[1]);
    if (address < g_hold->page || address >= g_hold->page + 4096) return EXCEPTION_CONTINUE_SEARCH;
    SetEvent(g_hold->entered);
    WaitForSingleObject(g_hold->release, 10000);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// DSTORAGE_PRIORITY_STATS: TotalRequestCount and the byte totals count submitted requests; PendingRequestCount counts
// requests whose processing has started and not completed. A memory queue (realtime, the only priority it may have):
// the counts are compared with the ones before.
void test_stats_timing(Env& env) {
    auto realtime = [&] {
        DSTORAGE_STATS st{};
        env.factory->GetStats(&st);
        return st.RealtimePriorityStats;
    };
    const DSTORAGE_PRIORITY_STATS before = realtime();
    IDStorageQueueX2* q = make_queue(env, 0x80, false, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
    if (!q) return;
    IDStorageStatusArrayX* s = make_status(env, 1);
    GuardHold hold;
    hold.page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    hold.entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    hold.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hold.page || !hold.entered || !hold.release) {
        check(false, "allocate the guard page and its events");
        return;
    }
    std::memset(hold.page, 0x5A, 4096);
    DWORD old = 0;
    VirtualProtect(hold.page, 4096, PAGE_READWRITE | PAGE_GUARD, &old);
    std::vector<uint8_t> out(4096, 0);
    DSTORAGE_REQUEST r{};
    r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    r.Source = hold.page;
    r.Destination = out.data();
    r.SourceSize = r.DestinationSize = 4096;
    q->EnqueueRequest(&r);
    q->EnqueueStatus(s, 0);
    DSTORAGE_PRIORITY_STATS p = realtime();
    check(p.TotalRequestCount == before.TotalRequestCount &&
              p.TotalRequestSourceByteCount == before.TotalRequestSourceByteCount &&
              p.TotalRequestDestinationByteCount == before.TotalRequestDestinationByteCount,
          "an enqueued request is not counted before Submit");

    g_hold = &hold;
    void* handler = AddVectoredExceptionHandler(1, hold_on_guard);
    q->Submit();
    check(WaitForSingleObject(hold.entered, 10000) == WAIT_OBJECT_0, "the copy reached the guard page");
    p = realtime();
    check(p.TotalRequestCount == before.TotalRequestCount + 1 &&
              p.TotalRequestSourceByteCount == before.TotalRequestSourceByteCount + 4096 &&
              p.TotalRequestDestinationByteCount == before.TotalRequestDestinationByteCount + 4096,
          "a submitted request is counted, with its sizes");
    check(p.PendingRequestCount == before.PendingRequestCount + 1, "a running request is pending");
    SetEvent(hold.release);
    check(wait_complete(s, 0), "the held copy completes");
    check_hr(s->GetHResult(0), S_OK, "the held copy succeeds");
    check(out == std::vector<uint8_t>(4096, 0x5A), "the held copy moved every byte");
    check(realtime().PendingRequestCount == before.PendingRequestCount, "a completed request is no longer pending");
    RemoveVectoredExceptionHandler(handler);
    g_hold = nullptr;
    CloseHandle(hold.entered);
    CloseHandle(hold.release);
    VirtualFree(hold.page, 0, MEM_RELEASE);
    s->Release();
    q->Release();
}

void test_stats(Env& env) {
    DSTORAGE_STATS st{};
    env.factory->GetStats(&st);
    check(st.CurrentOpenFileCount == 0, "every file was released");
    check(st.NormalPriorityStats.CurrentQueueCount == 0 && st.RealtimePriorityStats.CurrentQueueCount == 0,
          "every queue was released");
    check(st.NormalPriorityStats.TotalRequestCount > 1000 && st.NormalPriorityStats.TotalFenceCount >= 2 &&
              st.RealtimePriorityStats.TotalRequestCount == 7,
          "requests and fences are counted per priority");
    check(st.CumulativeOpenFileCount >= 10 && st.MaxOpenFileCount >= 3, "files are counted");
    check(st.ErrorCount > 10, "errors are counted");
}


// ---- zlib requests ------------------------------------------------------------------------------------------------
//
// Synthetic streams only. The two Huffman streams were made with Python's zlib (3.14, zlib-ng 1.3.1 API) from the
// text zlib_text() builds, which in Python is
//   text = lambda n: b"".join(b"line %04d: a zlib request for the queue, value %d\n" % (i, (i * i) % 97)
//                             for i in range(n))
// kZlibDynamic = zlib.compress(text(64), 9): one final dynamic-Huffman block (BTYPE 2), 345 bytes for 3,191.
// kZlibFixed = zlib.compress(text(4), 1): one final fixed-Huffman block (BTYPE 1), 77 bytes for 196.
// Streams with stored blocks (BTYPE 0) are built here by stored_stream(); for a single block that is byte for byte
// what zlib.compress(data, 0) gives.
constexpr uint8_t kZlibDynamic[] = {
    0x78, 0xda, 0x95, 0xd4, 0x4d, 0x4e, 0x03, 0x31, 0x0c, 0x05, 0xe0, 0x3d, 0xa7, 0xc8, 0x01, 0x58, 0x4c, 0xfc,
    0x97, 0x84, 0xdb, 0x14, 0x69, 0x10, 0x95, 0x46, 0x20, 0x4a, 0xcb, 0x82, 0xd3, 0x33, 0x0b, 0xf4, 0xbc, 0x7d,
    0xee, 0x6e, 0xa4, 0x3e, 0x25, 0xce, 0x67, 0xfb, 0xb8, 0x7e, 0xec, 0x6d, 0x3b, 0x7f, 0x2f, 0xed, 0xd2, 0x7e,
    0x8f, 0xeb, 0x6b, 0xbb, 0xed, 0x5f, 0x8f, 0xfd, 0xfb, 0xde, 0xde, 0x3e, 0x6f, 0xed, 0xfe, 0xbe, 0xb7, 0xf3,
    0xeb, 0xb1, 0x3f, 0xb7, 0x9f, 0xcb, 0xf1, 0x38, 0xff, 0xf9, 0x74, 0xfc, 0x07, 0x3a, 0x17, 0xe8, 0x08, 0x08,
    0x17, 0x30, 0x04, 0x94, 0x0b, 0x2c, 0x04, 0x8c, 0xbc, 0x52, 0x20, 0xe1, 0x5c, 0x42, 0x1c, 0x89, 0xe0, 0x12,
    0x9a, 0x67, 0x0c, 0xb2, 0xee, 0xac, 0x63, 0x72, 0x89, 0xc8, 0xa7, 0x5a, 0x5c, 0x62, 0x42, 0xa3, 0x93, 0xde,
    0x8a, 0x00, 0xe9, 0x2d, 0xb8, 0x54, 0x67, 0xc1, 0x07, 0x12, 0xa4, 0xf8, 0x10, 0x24, 0x48, 0xf2, 0x0c, 0x90,
    0xe2, 0x9a, 0x2f, 0x45, 0x8a, 0x47, 0x9e, 0x41, 0x8a, 0x2f, 0x74, 0x55, 0x27, 0xc5, 0x35, 0x39, 0x48, 0xf1,
    0x81, 0x81, 0x15, 0x52, 0xbc, 0xa3, 0x0e, 0x21, 0xc9, 0x1d, 0xb7, 0x12, 0x92, 0x7c, 0x61, 0x3a, 0x84, 0x24,
    0x37, 0xb4, 0x95, 0x90, 0xe4, 0x0b, 0x82, 0x42, 0x9a, 0x5b, 0xd6, 0x41, 0x9a, 0xaf, 0xbc, 0x15, 0x69, 0xee,
    0xe9, 0x41, 0x9a, 0x4f, 0x04, 0x48, 0xf2, 0x40, 0x5b, 0x29, 0x49, 0x2e, 0x98, 0x40, 0x25, 0xc9, 0x27, 0x6e,
    0xa5, 0x24, 0xb9, 0xe3, 0xa9, 0x94, 0x24, 0x17, 0x34, 0xa2, 0x92, 0xe4, 0x13, 0x2b, 0x54, 0x49, 0xf2, 0x40,
    0x93, 0x28, 0xbb, 0xd8, 0xf3, 0x75, 0x49, 0xf2, 0x9e, 0x67, 0xb0, 0xe4, 0x18, 0x0f, 0x65, 0xcd, 0x91, 0x30,
    0xd2, 0xdc, 0x20, 0x68, 0xa4, 0xb9, 0xc2, 0xc3, 0x48, 0xf3, 0x9e, 0x67, 0x90, 0xe6, 0x59, 0x06, 0x3b, 0xe5,
    0x98, 0x59, 0x23, 0xc9, 0x27, 0x00, 0x8d, 0x24, 0x1f, 0x68, 0x2b, 0x23, 0xc9, 0x47, 0x9e, 0x41, 0x92, 0x8f,
    0xac, 0x63, 0x55, 0x13, 0xbe, 0x55, 0x6f, 0xe5, 0xbd, 0x5a, 0xb9, 0x4b, 0xf5, 0x75, 0x5d, 0xab, 0x82, 0x6e,
    0xc5, 0x26, 0x71, 0xaf, 0xf6, 0xa1, 0x47, 0xb5, 0xd7, 0x7d, 0x54, 0xe7, 0xc9, 0x67, 0x75, 0x66, 0x7d, 0x55,
    0xf7, 0x42, 0x6c, 0xd5, 0xdd, 0x13, 0xbd, 0xba, 0xdf, 0x42, 0xaa, 0x3b, 0x34, 0x94, 0xde, 0xd3, 0x7f, 0x97,
    0x8b, 0x11, 0xce,
};
constexpr uint8_t kZlibFixed[] = {
    0x78, 0x01, 0xcb, 0xc9, 0xcc, 0x4b, 0x55, 0x30, 0x30, 0x30, 0x30, 0xb0, 0x52, 0x48, 0x54, 0xa8, 0xca, 0xc9,
    0x4c, 0x52, 0x28, 0x4a, 0x2d, 0x2c, 0x4d, 0x2d, 0x2e, 0x51, 0x48, 0xcb, 0x2f, 0x52, 0x28, 0xc9, 0x48, 0x55,
    0x28, 0x2c, 0x4d, 0x2d, 0x4d, 0xd5, 0x51, 0x28, 0x4b, 0xcc, 0x29, 0x4d, 0x55, 0x30, 0xe0, 0x82, 0x69, 0x30,
    0x24, 0x4e, 0x83, 0x21, 0x5c, 0x83, 0x11, 0x71, 0x1a, 0x4c, 0xe0, 0x1a, 0x8c, 0x89, 0xd3, 0x60, 0xc9, 0x05,
    0x00, 0xb0, 0x6e, 0x40, 0x49,
};

std::vector<uint8_t> zlib_text(int lines) {
    std::string t;
    char line[96];
    for (int i = 0; i < lines; ++i) {
        std::snprintf(line, sizeof line, "line %04d: a zlib request for the queue, value %d\n", i, (i * i) % 97);
        t += line;
    }
    return {t.begin(), t.end()};
}

uint32_t adler32(const std::vector<uint8_t>& data) {
    uint32_t a = 1, b = 0;
    for (const uint8_t x : data) {
        a = (a + x) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

// A zlib stream (header 78 01) whose first block stores `stored` (a final block when there is no tail), followed by
// the deflate blocks of the zlib stream `tail` (its header and Adler-32 dropped: a stored block ends on a byte
// boundary, so they follow as they are) and the Adler-32 of the whole result, `stored` then `tail_plain`.
std::vector<uint8_t> stored_stream(const std::vector<uint8_t>& stored, const uint8_t* tail = nullptr,
                                   size_t tail_size = 0, const std::vector<uint8_t>& tail_plain = {}) {
    const auto n = static_cast<uint16_t>(stored.size());
    std::vector<uint8_t> z = {0x78, 0x01, static_cast<uint8_t>(tail ? 0 : 1), static_cast<uint8_t>(n),
                              static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(~n), static_cast<uint8_t>(~n >> 8)};
    z.insert(z.end(), stored.begin(), stored.end());
    if (tail) z.insert(z.end(), tail + 2, tail + tail_size - 4);
    std::vector<uint8_t> plain = stored;
    plain.insert(plain.end(), tail_plain.begin(), tail_plain.end());
    const uint32_t check = adler32(plain);
    for (int shift = 24; shift >= 0; shift -= 8) z.push_back(static_cast<uint8_t>(check >> shift));
    return z;
}

void test_zlib(Env& env) {
    const std::vector<uint8_t> dynamic(std::begin(kZlibDynamic), std::end(kZlibDynamic));
    const std::vector<uint8_t> fixed(std::begin(kZlibFixed), std::end(kZlibFixed));
    const std::vector<uint8_t> dynamic_plain = zlib_text(64), fixed_plain = zlib_text(4);
    // The first block header is the low three bits after the two header bytes: BFINAL, then BTYPE (RFC 1951 3.2.3).
    check(dynamic.size() == 345 && (dynamic[2] & 7) == (1 | 2 << 1), "kZlibDynamic is one final dynamic block");
    check(fixed.size() == 77 && (fixed[2] & 7) == (1 | 1 << 1), "kZlibFixed is one final fixed-Huffman block");
    check(dynamic_plain.size() == 3191 && fixed_plain.size() == 196, "the synthetic text has the generator's sizes");

    const std::vector<uint8_t> stored_part = pattern(300, 21);
    const std::vector<uint8_t> mixed = stored_stream(stored_part, kZlibDynamic, sizeof kZlibDynamic, dynamic_plain);
    std::vector<uint8_t> mixed_plain = stored_part;
    mixed_plain.insert(mixed_plain.end(), dynamic_plain.begin(), dynamic_plain.end());
    check((mixed[2] & 7) == 0, "the mixed stream starts with a stored block that is not the last");
    const std::vector<uint8_t> stored_only = stored_stream(pattern(40, 22));  // larger than what it inflates to
    std::vector<uint8_t> bad_method = dynamic, bad_type = fixed, bad_check = dynamic, preset = dynamic;
    bad_method[0] ^= 1;          // CM 9: not deflate
    preset[1] = 0xbb;            // 78 BB: a valid check value with FDICT set (a preset dictionary)
    bad_type[2] |= 0x06;         // BTYPE 3, reserved
    bad_check.back() ^= 1;       // the Adler-32

    // One file, every stream at a 16-byte aligned offset and followed by 32 zero bytes.
    std::vector<uint8_t> file;
    auto put = [&](const std::vector<uint8_t>& z) {
        const uint64_t at = file.size();
        file.insert(file.end(), z.begin(), z.end());
        file.resize((file.size() + 32 + 15) & ~size_t{15}, 0);
        return at;
    };
    const uint64_t at_dynamic = put(dynamic), at_fixed = put(fixed), at_mixed = put(mixed),
                   at_stored = put(stored_only), at_method = put(bad_method), at_preset = put(preset),
                   at_type = put(bad_type), at_check = put(bad_check);
    write_file(env.game / L"data" / L"zlib.bin", file);

    struct Case {
        uint64_t offset;
        uint32_t source, destination;
        HRESULT want;
        const std::vector<uint8_t>* result;  // the expected bytes; null when the destination stays untouched
        const char* what;
        uint64_t options = 0;
        uint32_t intermediate = 0;
    };
    const auto z = static_cast<uint32_t>(dynamic.size());
    std::vector<Case> cases = {
        {at_dynamic, z, 3191, S_OK, &dynamic_plain, "a zlib file request: dynamic Huffman"},
        {at_fixed, 77, 196, S_OK, &fixed_plain, "a zlib file request: fixed Huffman"},
        {at_mixed, static_cast<uint32_t>(mixed.size()), 3491, S_OK, &mixed_plain,
         "a zlib file request: a stored block, then dynamic Huffman"},
        {at_dynamic, z + 16, 3191, S_OK, &dynamic_plain, "bytes after the Adler-32 are not read"},
        {at_stored, static_cast<uint32_t>(stored_only.size()), 40, E_DSTORAGE_INVALID_DESTINATION_SIZE, nullptr,
         "a zlib DestinationSize not larger than SourceSize"},
        {at_method, z, 3191, E_DSTORAGE_ZLIB_BAD_HEADER, nullptr, "a header that is not deflate"},
        {at_preset, z, 3191, E_DSTORAGE_ZLIB_BAD_HEADER, nullptr, "a header with a preset dictionary"},
        {at_type, 77, 196, E_DSTORAGE_ZLIB_BAD_DATA, nullptr, "a reserved block type"},
        {at_dynamic, z - 10, 3191, E_DSTORAGE_ZLIB_BAD_DATA, nullptr, "a truncated stream"},
        {at_check, z, 3191, E_DSTORAGE_ZLIB_PARITY_FAIL, nullptr, "an Adler-32 mismatch"},
        {at_dynamic, z, 3192, E_DSTORAGE_DECOMPRESSION_ERROR, nullptr, "a stream shorter than DestinationSize"},
        {at_dynamic, z, 3190, E_DSTORAGE_DECOMPRESSION_ERROR, nullptr, "a stream longer than DestinationSize"},
        {at_dynamic + 8, z, 3191, E_DSTORAGE_INVALID_FILE_OFFSET, nullptr, "a zlib offset not 16-byte aligned"},
        {file.size() - 16, 32, 64, E_DSTORAGE_END_OF_FILE, nullptr, "a zlib source past the end of the file"},
        {at_dynamic, z, 3191, E_DSTORAGE_INVALID_INTERMEDIATE_SIZE, nullptr, "zlib with an IntermediateSize", 0,
         16},
        {at_dynamic, z, 3191, E_NOTIMPL, nullptr, "zlib and BCPack is not implemented yet",
         static_cast<uint64_t>(DSTORAGE_BCPACK_MODE_BC1) << options::kBcpackShift, 64},
        {at_dynamic, z, 3191, E_NOTIMPL, nullptr, "zlib and swizzle is not implemented yet",
         static_cast<uint64_t>(DSTORAGE_SWIZZLE_MODE_4K_4K_8K) << options::kSwizzleShift},
    };

    DSTORAGE_STATS before{};
    env.factory->GetStats(&before);
    IDStorageFileX* f = open(env, L"GAME:\\data\\zlib.bin");
    IDStorageQueueX2* q = make_queue(env, 0x80, false);
    IDStorageStatusArrayX* s = make_status(env, static_cast<UINT32>(cases.size()));
    constexpr size_t kSlack = 64;  // past DestinationSize: must stay untouched
    std::vector<std::vector<uint8_t>> out(cases.size());
    for (size_t i = 0; i < cases.size(); ++i) {
        const Case& c = cases[i];
        out[i].assign(c.destination + kSlack, 0xEE);
        DSTORAGE_REQUEST r = file_request(f, c.offset, out[i].data(), c.destination);
        r.SourceSize = c.source;
        r.IntermediateSize = c.intermediate;
        r.Options.AsUINT64 = options::kZlib | c.options;
        q->EnqueueRequest(&r);
        q->EnqueueStatus(s, static_cast<UINT32>(i));
    }
    q->Submit();
    check(wait_complete(s, static_cast<UINT32>(cases.size() - 1)), "the zlib file requests complete");
    for (size_t i = 0; i < cases.size(); ++i) {
        const Case& c = cases[i];
        check_hr(s->GetHResult(static_cast<UINT32>(i)), c.want, c.what);
        const std::vector<uint8_t> untouched(c.destination, 0xEE);
        const bool head = c.result ? std::equal(c.result->begin(), c.result->end(), out[i].begin())
                                   : std::equal(untouched.begin(), untouched.end(), out[i].begin());
        const bool tail = std::all_of(out[i].end() - kSlack, out[i].end(), [](uint8_t x) { return x == 0xEE; });
        check(head && tail, c.result ? "the inflated bytes are the synthetic text, and nothing past them"
                                     : "a failed zlib request leaves the destination untouched");
    }
    s->Release();
    q->Release();
    f->Release();

    // A memory source: the same stream, one inflated in place (the compressed bytes sit at the end of their own
    // destination, as a title may load them), a corrupt one, a source and a destination the process cannot touch.
    IDStorageQueueX2* mq = make_queue(env, 0x80, true, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
    IDStorageStatusArrayX* ms = make_status(env, 5);
    void* reserved = VirtualAlloc(nullptr, 1 << 16, MEM_RESERVE, PAGE_READWRITE);
    std::vector<uint8_t> m0(3191, 0xEE), in_place(3191, 0xEE), m2(3191, 0xEE), m3(3191, 0xEE);
    std::copy(dynamic.begin(), dynamic.end(), in_place.end() - dynamic.size());
    DSTORAGE_REQUEST r{};
    r.Options.AsUINT64 = options::kZlib | options::kSourceMemory;
    r.SourceSize = z;
    r.DestinationSize = 3191;
    r.Source = dynamic.data();
    r.Destination = m0.data();
    mq->EnqueueRequest(&r);
    mq->EnqueueStatus(ms, 0);
    r.Source = in_place.data() + in_place.size() - dynamic.size();
    r.Destination = in_place.data();
    mq->EnqueueRequest(&r);
    mq->EnqueueStatus(ms, 1);
    r.Source = bad_check.data();
    r.Destination = m2.data();
    mq->EnqueueRequest(&r);
    mq->EnqueueStatus(ms, 2);
    r.Source = reserved;
    r.Destination = m3.data();
    mq->EnqueueRequest(&r);
    mq->EnqueueStatus(ms, 3);
    r.Source = dynamic.data();
    r.Destination = reserved;
    mq->EnqueueRequest(&r);
    mq->EnqueueStatus(ms, 4);
    mq->Submit();
    check(wait_complete(ms, 4), "the zlib memory requests complete");
    check_hr(ms->GetHResult(0), S_OK, "a zlib memory request");
    check(m0 == dynamic_plain, "the zlib memory request inflates to the synthetic text");
    check_hr(ms->GetHResult(1), S_OK, "a zlib memory request whose source lies inside its destination");
    check(in_place == dynamic_plain, "inflating in place gives the synthetic text");
    check_hr(ms->GetHResult(2), E_DSTORAGE_ZLIB_PARITY_FAIL, "a zlib memory request with an Adler-32 mismatch");
    check(m2 == std::vector<uint8_t>(3191, 0xEE), "the failed zlib memory request leaves the destination untouched");
    check_hr(ms->GetHResult(3), E_DSTORAGE_ACCESS_VIOLATION, "a zlib source the process cannot read");
    check_hr(ms->GetHResult(4), E_DSTORAGE_ACCESS_VIOLATION, "a zlib destination the process cannot write");
    VirtualFree(reserved, 0, MEM_RELEASE);
    ms->Release();
    mq->Release();

    // Counted when the inflate starts: the 11 file requests (4 good, 7 bad streams) and 4 memory requests (all but the
    // unreadable source) whose checks passed and whose source was read. Failed: the 7 bad file streams and the memory
    // one with an Adler-32 mismatch; a destination the process cannot write is not a decompression error.
    DSTORAGE_STATS after{};
    env.factory->GetStats(&after);
    check(after.TotalDecompressionRequestCount - before.TotalDecompressionRequestCount == 15,
          "TotalDecompressionRequestCount counts the zlib requests that reached the inflate");
    check(after.DecompressionErrors - before.DecompressionErrors == 8, "DecompressionErrors counts the bad streams");
    check(after.PendingDecompressionRequestCount == 0, "no decompression is pending afterwards");
}

}  // namespace

int main() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    Env env;
    env.root = fs::path(temp) / (L"leonida_dstorage_test_" + std::to_wstring(GetCurrentProcessId()));
    env.game = env.root / L"game";
    env.overlay = env.root / L"overlay";
    std::error_code ec;
    fs::remove_all(env.root, ec);
    env.a_package = pattern(4096, 1);
    env.a_overlay = pattern(5000, 2);  // another size and other bytes than the package's copy
    env.b = pattern(10000, 3);
    env.big = pattern(3u << 20, 4);
    write_file(env.game / L"data" / L"a.bin", env.a_package);
    write_file(env.overlay / L"data" / L"a.bin", env.a_overlay);
    write_file(env.game / L"data" / L"b.bin", env.b);
    write_file(env.game / L"data" / L"big.bin", env.big);
    rt::fs::configure(env.game, env.overlay);

    test_layout_and_options();
    test_registry(env);
    test_factory_checks(env);
    test_open_paths(env);
    test_reads_and_errors(env);
    test_fence_order_and_events(env);
    test_cancel(env);
    test_auto_submit_and_full_queue(env);
    test_memory_queue(env);
    test_close_and_release(env);
    test_threads(env);
    test_iid_policy_events_and_cancel(env);
    test_stats_timing(env);
    test_stats(env);
    test_zlib(env);  // after test_stats, whose request totals it would change

    fs::remove_all(env.root, ec);
    std::printf("dstorage: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
