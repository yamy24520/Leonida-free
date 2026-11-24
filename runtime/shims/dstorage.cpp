// Xbox DirectStorage for a mapped title: DStorageGetFactory and the IDStorageFactoryX2 / IDStorageQueueX2 /
// IDStorageFileX / IDStorageStatusArrayX objects of the console API (dstorage_xs_abi.h).
//
// Served as dstorage_xs.dll, xcrdapi.dll and dstorage.dll!DStorageGetFactory; docs/directstorage.md has the evidence
// for those names, the console ABI and what each method does here. In short:
//   - OpenFile resolves kernel32.dll!CreateFileW through the shim registry, so a GAME: path goes through the
//     runtime's console path mapping and write overlay (file_system.cpp) exactly as the title's own CreateFileW.
//   - Each queue has one worker thread that runs its submitted entries in order: a request reads a file range or
//     copies a memory range into the destination, a status entry, fence signal or event runs when every entry before
//     it has. Completion is therefore in queue order, as the API guarantees.
//   - A zlib request (ZlibDecompress alone) reads its source into a staging buffer, inflates it on the worker with
//     the runtime's RFC 1950 inflater (leonida::zlib_inflate, src/shader_library.h) and copies the result to the
//     destination only when it is exactly DestinationSize bytes; a stream that fails leaves the destination as it was.
//   - BCPack, swizzle and physical-page requests fail with E_NOTIMPL for now (later PRs: they become stages between
//     the read and the destination write, BCPack after the inflate).
// Nothing here goes online or touches the GPU.
#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../src/shader_library.h"  // leonida::zlib_inflate
#include "dstorage_xs_abi.h"
#include "shims.h"

using namespace dsx;

namespace {

// ---- Logging ------------------------------------------------------------------------------------------------------

// Process-lifetime state is allocated once and never freed: a worker thread may still run while statics are destroyed
// at exit.
std::mutex& log_mutex() {
    static auto* m = new std::mutex;
    return *m;
}

void log_once(const std::string& key, const std::string& line) {
    static auto* seen = new std::set<std::string>;
    std::lock_guard lock(log_mutex());
    if (!seen->insert(key).second) return;
    std::fprintf(stderr, "[rt] dstorage: %s\n", line.c_str());
}

std::string guid_text(const GUID& g) {
    char b[48];
    std::snprintf(b, sizeof b, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", g.Data1, g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

bool same_guid(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

std::string hr_text(HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08lx", static_cast<unsigned long>(hr));
    return b;
}

// ---- Interface ids ------------------------------------------------------------------------------------------------
//
// The X interfaces' ids are not public. A creating call (DStorageGetFactory, CreateQueue*, OpenFile,
// CreateStatusArray) accepts an id it has not seen for the kind of object it hands out and logs it the first time, so
// the first boot records the console's ids. It refuses an id seen for another kind, a PC DirectStorage id of another
// shape, and the system COM interfaces (IMarshal, IAgileObject, ...), which a DirectStorage object never implements.
// QueryInterface is stricter (docs/d3d12_vulkan.md: an unknown id is E_NOINTERFACE): it accepts IUnknown, the
// declared ids (dstorage_xs_abi.h) and the ids a creating call accepted for that kind, and logs anything else. This is
// a policy, not a fact; it lives here so a measured id list can replace it.

enum class Kind { Factory, Queue, File, StatusArray };

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Factory: return "factory";
        case Kind::Queue: return "queue";
        case Kind::File: return "file";
        case Kind::StatusArray: return "status array";
    }
    return "?";
}

// System interfaces: the COM base range {xxxxxxxx-0000-0000-C000-000000000046} (IClassFactory, IMarshal,
// IStdMarshalInfo, IExternalConnection, IMultiQI, IClientSecurity, IServerSecurity, IRpcOptions, IMarshal2,
// IWeakReferenceSource, IDispatch, IPersist, ...) and these others.
constexpr GUID kSystemInterfaceIds[] = {
    {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}},  // IAgileObject
    {0xecc8691b, 0xc1db, 0x4dc0, {0x85, 0x5e, 0x65, 0xf6, 0xc5, 0x51, 0xaf, 0x49}},  // INoMarshal
    {0x1c733a30, 0x2a1c, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}},  // ICallFactory
    {0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}},  // IInspectable
    {0xdf0b3d60, 0x548f, 0x101b, {0x8e, 0x65, 0x08, 0x00, 0x2b, 0x2b, 0xd1, 0x19}},  // ISupportErrorInfo
    {0xb196b283, 0xbab4, 0x101a, {0xb6, 0x9c, 0x00, 0xaa, 0x00, 0x34, 0x1d, 0x07}},  // IProvideClassInfo
    {0xb196b284, 0xbab4, 0x101a, {0xb6, 0x9c, 0x00, 0xaa, 0x00, 0x34, 0x1d, 0x07}},  // IConnectionPointContainer
    {0xfc4801a3, 0x2ba9, 0x11cf, {0xa2, 0x29, 0x00, 0xaa, 0x00, 0x3d, 0x73, 0x52}},  // IObjectWithSite
};

bool system_interface(const GUID& g) {
    static const uint8_t kComBase[8] = {0xc0, 0, 0, 0, 0, 0, 0, 0x46};
    if (g.Data2 == 0 && g.Data3 == 0 && std::memcmp(g.Data4, kComBase, 8) == 0) return true;
    for (const GUID& s : kSystemInterfaceIds)
        if (same_guid(g, s)) return true;
    return false;
}

// The ids declared for a kind: the PC IDStorageStatusArray id, whose interface has the console one's shape (console
// D3D12 binaries reuse the PC SDK's ids). No other X interface id is known.
bool declared(const GUID& g, Kind kind) {
    for (const GUID& id : kStatusArrayIds)
        if (same_guid(g, id)) return kind == Kind::StatusArray;
    return false;
}

bool declared_for_any(const GUID& g) {
    for (const GUID& id : kStatusArrayIds)
        if (same_guid(g, id)) return true;
    return false;
}

class IidPolicy {
public:
    bool accept_creating(REFIID riid, Kind kind, const char* caller) {
        if (same_guid(riid, __uuidof(IUnknown)) || declared(riid, kind)) return true;
        if (system_interface(riid)) return refuse(riid, caller, "a system COM interface");
        if (declared_for_any(riid)) return refuse(riid, caller, "an id declared for another kind of object");
        for (const GUID& pc : kPcInterfaceIds)
            if (same_guid(riid, pc))
                return refuse(riid, caller, "a PC DirectStorage interface; only the console ABI is served");
        std::lock_guard lock(mutex_);
        for (const auto& [id, k] : learned_)
            if (same_guid(id, riid)) {
                if (k == kind) return true;
                return refuse(riid, caller, std::string("seen before as a ") + kind_name(k) + " interface");
            }
        learned_.push_back({riid, kind});
        log_once("learn" + guid_text(riid), std::string(caller) + " asked for " + guid_text(riid) + ": serving the " +
                                                kind_name(kind) + " interface");
        return true;
    }

    bool accept_query(REFIID riid, Kind kind, const char* caller) {
        if (same_guid(riid, __uuidof(IUnknown)) || declared(riid, kind)) return true;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [id, k] : learned_)
                if (same_guid(id, riid) && k == kind) return true;
        }
        return refuse(riid, caller, std::string("not an id a ") + kind_name(kind) + " was created with");
    }

private:
    static bool refuse(REFIID riid, const char* caller, const std::string& why) {
        log_once(std::string("refuse") + caller + guid_text(riid),
                 std::string(caller) + " asked for " + guid_text(riid) + ": " + why + " (E_NOINTERFACE)");
        return false;
    }

    std::mutex mutex_;
    std::vector<std::pair<GUID, Kind>> learned_;
};

IidPolicy& iid_policy() {
    static auto* p = new IidPolicy;
    return *p;
}

// A bad argument to a factory or queue method, logged once per method and kind: a wrong vtable guess for the console
// interfaces would show up here first, as arguments that make no sense for the method.
HRESULT bad_call(const char* method, HRESULT hr, const char* what) {
    log_once(std::string("bad") + method + what,
             std::string(method) + ": " + what + " (" + hr_text(hr) +
                 "; logged once; a wrong vtable slot guess would show up like this)");
    return hr;
}

// ---- Statistics (IDStorageFactoryX1::GetStats) --------------------------------------------------------------------

struct Stats {
    std::mutex mutex;
    DSTORAGE_STATS s{};
    uint32_t live_queues = 0;  // all priorities; SetStagingBufferSize is refused while queues or files exist
};

Stats& stats() {
    static auto* s = new Stats;
    return *s;
}

DSTORAGE_PRIORITY_STATS& priority_stats(DSTORAGE_STATS& s, DSTORAGE_PRIORITY p) {
    switch (p) {
        case DSTORAGE_PRIORITY_LOW: return s.LowPriorityStats;
        case DSTORAGE_PRIORITY_HIGH: return s.HighPriorityStats;
        case DSTORAGE_PRIORITY_REALTIME: return s.RealtimePriorityStats;
        default: return s.NormalPriorityStats;
    }
}

void count_error() {
    std::lock_guard lock(stats().mutex);
    ++stats().s.ErrorCount;
}

// ---- Live objects ---------------------------------------------------------------------------------------------------
//
// A request names its file and a status entry its array by pointer. Only objects this shim made and that are still
// alive are used; anything else is an error in the request, never a call through a stranger's vtable.

std::mutex& live_mutex() {
    static auto* m = new std::mutex;
    return *m;
}
// One set per kind, so a status array passed as a file (or the other way round) is not found.
std::unordered_set<const void*>& live_files() {
    static auto* s = new std::unordered_set<const void*>;
    return *s;
}
std::unordered_set<const void*>& live_status_arrays() {
    static auto* s = new std::unordered_set<const void*>;
    return *s;
}

// AddRef only if the count has not reached zero (an object being released is not revived).
bool add_ref_if_live(std::atomic<ULONG>& refs) {
    ULONG r = refs.load();
    while (r != 0)
        if (refs.compare_exchange_weak(r, r + 1)) return true;
    return false;
}

// ---- Status arrays ------------------------------------------------------------------------------------------------

class StatusArray final : public IDStorageStatusArrayX {
public:
    StatusArray(uint32_t capacity, std::unique_ptr<std::atomic<HRESULT>[]> entries)
        : capacity_(capacity), entries_(std::move(entries)) {
        for (uint32_t i = 0; i < capacity_; ++i) entries_[i].store(S_OK, std::memory_order_relaxed);
        std::lock_guard lock(live_mutex());
        live_status_arrays().insert(static_cast<IDStorageStatusArrayX*>(this));
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!iid_policy().accept_query(riid, Kind::StatusArray, "IDStorageStatusArrayX::QueryInterface"))
            return E_NOINTERFACE;
        AddRef();
        *ppv = static_cast<IDStorageStatusArrayX*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r;
        {
            std::lock_guard lock(live_mutex());
            r = --refs_;
            if (r == 0) live_status_arrays().erase(static_cast<IDStorageStatusArrayX*>(this));
        }
        if (r == 0) delete this;
        return r;
    }

    // IsComplete is GetHResult(index) != E_PENDING (the reference says so); an index past the array is not pending.
    bool STDMETHODCALLTYPE IsComplete(UINT32 index) override { return GetHResult(index) != E_PENDING; }
    HRESULT STDMETHODCALLTYPE GetHResult(UINT32 index) override {
        if (index >= capacity_) return E_DSTORAGE_INDEX_BOUND;
        return entries_[index].load(std::memory_order_acquire);
    }

    uint32_t capacity() const { return capacity_; }
    void set_pending(uint32_t index) { entries_[index].store(E_PENDING, std::memory_order_release); }
    void complete(uint32_t index, HRESULT hr) { entries_[index].store(hr, std::memory_order_release); }
    std::atomic<ULONG>& refs() { return refs_; }

private:
    std::atomic<ULONG> refs_{1};
    const uint32_t capacity_;
    std::unique_ptr<std::atomic<HRESULT>[]> entries_;
};

// ---- Files --------------------------------------------------------------------------------------------------------

class File final : public IDStorageFileX {
public:
    File(HANDLE handle, std::wstring path) : handle_(handle), path_(std::move(path)) {
        {
            std::lock_guard lock(live_mutex());
            live_files().insert(static_cast<IDStorageFileX*>(this));
        }
        std::lock_guard lock(stats().mutex);
        auto& s = stats().s;
        ++s.CurrentOpenFileCount;
        ++s.CumulativeOpenFileCount;
        s.MaxOpenFileCount = std::max(s.MaxOpenFileCount, s.CurrentOpenFileCount);
    }
    ~File() {
        Close();
        std::lock_guard lock(stats().mutex);
        --stats().s.CurrentOpenFileCount;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!iid_policy().accept_query(riid, Kind::File, "IDStorageFileX::QueryInterface")) return E_NOINTERFACE;
        AddRef();
        *ppv = static_cast<IDStorageFileX*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r;
        {
            std::lock_guard lock(live_mutex());
            r = --refs_;
            if (r == 0) live_files().erase(static_cast<IDStorageFileX*>(this));
        }
        if (r == 0) delete this;
        return r;
    }

    // Waits for the submitted requests that read this file, then closes the handle. A request submitted later fails
    // with E_DSTORAGE_FILE_NOT_OPEN. Waiting only for submitted requests keeps a Close from blocking forever on a
    // request that sits unsubmitted in a queue with auto-submit disabled.
    void STDMETHODCALLTYPE Close() override {
        std::unique_lock lock(mutex_);
        closed_ = true;
        idle_.wait(lock, [&] { return pending_ == 0; });
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    // A new handle the caller closes (GENERIC_READ, FILE_SHARE_READ, as the reference describes it). ReOpenFile,
    // not DuplicateHandle: a duplicate shares the file position with the shim's own reads, so the title's ReadFile
    // through it would start wherever the worker's last read ended.
    HANDLE STDMETHODCALLTYPE GetHandle() override {
        std::lock_guard lock(mutex_);
        if (closed_ || handle_ == INVALID_HANDLE_VALUE) {
            SetLastError(ERROR_INVALID_HANDLE);
            return INVALID_HANDLE_VALUE;
        }
        return ReOpenFile(handle_, GENERIC_READ, FILE_SHARE_READ, 0);
    }

    // Called when a request naming this file is submitted. False once the file is closed.
    bool add_pending() {
        std::lock_guard lock(mutex_);
        if (closed_) return false;
        ++pending_;
        return true;
    }
    void finish_pending() {
        std::lock_guard lock(mutex_);
        if (--pending_ == 0) idle_.notify_all();
    }
    // Valid while the caller holds a pending count: Close does not close the handle before it drops to zero.
    HANDLE handle() const { return handle_; }
    const std::wstring& path() const { return path_; }
    std::atomic<ULONG>& refs() { return refs_; }

private:
    std::atomic<ULONG> refs_{1};
    std::mutex mutex_;
    std::condition_variable idle_;
    HANDLE handle_;
    const std::wstring path_;
    uint32_t pending_ = 0;
    bool closed_ = false;
};

StatusArray* live_status_array(IDStorageStatusArrayX* p) {
    std::lock_guard lock(live_mutex());
    if (!p || !live_status_arrays().count(p)) return nullptr;
    auto* a = static_cast<StatusArray*>(p);
    return add_ref_if_live(a->refs()) ? a : nullptr;
}

File* live_file(IDStorageFileX* p) {
    std::lock_guard lock(live_mutex());
    if (!p || !live_files().count(p)) return nullptr;
    auto* f = static_cast<File*>(p);
    return add_ref_if_live(f->refs()) ? f : nullptr;
}

// ---- Reads --------------------------------------------------------------------------------------------------------

// memmove that turns an access violation into an error instead of ending the process. No C++ objects here: a
// function with __try cannot have any that need unwinding.
bool guarded_move(void* destination, const void* source, size_t size) {
    __try {
        std::memmove(destination, source, size);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

HRESULT read_error(DWORD error) {
    switch (error) {
        case ERROR_NOACCESS:
        case ERROR_INVALID_USER_BUFFER: return E_DSTORAGE_ACCESS_VIOLATION;
        case ERROR_HANDLE_EOF: return E_DSTORAGE_END_OF_FILE;
        default: return HRESULT_FROM_WIN32(error);
    }
}

// Reads exactly "size" bytes at "offset" of a synchronous handle into "destination".
HRESULT read_file_range(HANDLE handle, uint64_t offset, void* destination, uint32_t size) {
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(handle, &length)) return HRESULT_FROM_WIN32(GetLastError());
    const uint64_t file_size = static_cast<uint64_t>(length.QuadPart);
    if (offset > file_size || size > file_size - offset) return E_DSTORAGE_END_OF_FILE;
    auto* out = static_cast<uint8_t*>(destination);
    uint32_t done = 0;
    while (done < size) {
        OVERLAPPED at{};
        const uint64_t position = offset + done;
        at.Offset = static_cast<DWORD>(position);
        at.OffsetHigh = static_cast<DWORD>(position >> 32);
        DWORD got = 0;
        if (!ReadFile(handle, out + done, size - done, &got, &at)) return read_error(GetLastError());
        if (got == 0) return E_DSTORAGE_END_OF_FILE;  // the file shrank since the size check
        done += got;
    }
    return S_OK;
}

// ---- Queues -------------------------------------------------------------------------------------------------------

struct Entry {
    enum class Type { Request, Status, Signal, Event } type = Type::Request;
    DSTORAGE_REQUEST request{};
    File* file = nullptr;          // a reference, for a file request naming a live file
    bool file_counted = false;     // the file's pending count was raised at submit
    bool cancelled = false;        // CancelRequestsWithTag matched it before it started
    bool counted = false;          // a request the title passed: counted in the statistics when it is submitted
    HRESULT enqueue_error = S_OK;  // found at enqueue: reported when the entry is reached, in order
    StatusArray* status = nullptr;  // a reference
    IDStorageStatusArrayX* status_as_given = nullptr;
    uint32_t index = 0;
    ID3D12Fence* fence = nullptr;  // a reference
    uint64_t value = 0;
    HANDLE event = nullptr;  // a duplicate the entry owns
};

class Queue final : public IDStorageQueueX2 {
public:
    explicit Queue(const DSTORAGE_QUEUE_DESC1& desc) : desc_(desc) {
        error_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        clear_record_locked();
        std::lock_guard lock(stats().mutex);
        auto& p = priority_stats(stats().s, desc_.Priority);
        ++p.CurrentQueueCount;
        ++p.CumulativeQueueCount;
        p.MaxQueueCount = std::max(p.MaxQueueCount, p.CurrentQueueCount);
        ++stats().live_queues;
    }
    // Separate from the constructor so a failure to start the thread can be reported.
    bool start() {
        if (!error_event_) return false;
        try {
            worker_ = std::thread([this] { run(); });
        } catch (...) {
            return false;
        }
        return true;
    }
    ~Queue() {
        Close();
        if (error_event_) CloseHandle(error_event_);
        std::lock_guard lock(stats().mutex);
        --priority_stats(stats().s, desc_.Priority).CurrentQueueCount;
        --stats().live_queues;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!iid_policy().accept_query(riid, Kind::Queue, "IDStorageQueueX::QueryInterface")) return E_NOINTERFACE;
        AddRef();
        *ppv = static_cast<IDStorageQueueX2*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    // The last Release closes the queue, which runs everything enqueued first.
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    void STDMETHODCALLTYPE EnqueueRequest(const DSTORAGE_REQUEST* request) override {
        Entry e;
        e.type = Entry::Type::Request;
        if (!request) {
            e.enqueue_error = bad_call("IDStorageQueueX::EnqueueRequest", E_INVALIDARG, "null request");
        } else {
            e.request = *request;  // the title may reuse its structure as soon as this returns
            const uint64_t o = e.request.Options.AsUINT64;
            if (options::source_type(o) == DSTORAGE_REQUEST_SOURCE_FILE &&
                desc_.SourceType == DSTORAGE_REQUEST_SOURCE_FILE && e.request.File) {
                e.file = live_file(e.request.File);
                if (!e.file) e.enqueue_error = E_DSTORAGE_INVALID_FILE_HANDLE;
            }
            e.counted = true;  // TotalRequestCount is "submitted requests" (DSTORAGE_PRIORITY_STATS): see submit_locked
        }
        enqueue(std::move(e));
    }

    void STDMETHODCALLTYPE EnqueueStatus(IDStorageStatusArrayX* status_array, UINT32 index) override {
        Entry e;
        e.type = Entry::Type::Status;
        e.status_as_given = status_array;
        e.index = index;
        e.status = live_status_array(status_array);
        if (!e.status) {
            e.enqueue_error = E_DSTORAGE_INVALID_STATUS_ARRAY;
        } else if (index >= e.status->capacity()) {
            e.enqueue_error = E_DSTORAGE_INDEX_BOUND;
        } else {
            e.status->set_pending(index);  // polling after this call sees E_PENDING until the entry is reached
        }
        {
            std::lock_guard lock(stats().mutex);
            ++priority_stats(stats().s, desc_.Priority).TotalStatusCount;
        }
        enqueue(std::move(e));
    }

    void STDMETHODCALLTYPE EnqueueSignal(ID3D12Fence* fence, UINT64 value) override {
        Entry e;
        e.type = Entry::Type::Signal;
        e.fence = fence;
        e.value = value;
        if (fence) {
            fence->AddRef();
        } else {
            e.enqueue_error = E_DSTORAGE_INVALID_FENCE;
        }
        {
            std::lock_guard lock(stats().mutex);
            ++priority_stats(stats().s, desc_.Priority).TotalFenceCount;
        }
        enqueue(std::move(e));
    }

    // The handle is duplicated here and the duplicate closed once it has been set (or dropped), so an event the
    // title closes, and whose handle value is then reused for another object, is never the one set.
    void STDMETHODCALLTYPE EnqueueSetEvent(HANDLE handle) override {
        Entry e;
        e.type = Entry::Type::Event;
        if (!handle) {
            e.enqueue_error = bad_call("IDStorageQueueX1::EnqueueSetEvent", HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE),
                                       "null event handle");
        } else if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &e.event, 0, FALSE,
                                    DUPLICATE_SAME_ACCESS)) {
            e.event = nullptr;
            e.enqueue_error = HRESULT_FROM_WIN32(GetLastError());
        }
        enqueue(std::move(e));
    }

    void STDMETHODCALLTYPE Submit() override {
        std::lock_guard lock(mutex_);
        submit_locked();
    }

    // Marks the requests enqueued before this call that have not started. A cancelled request completes as a success
    // without being read (the overview: the title may no longer use its result).
    void STDMETHODCALLTYPE CancelRequestsWithTag(UINT64 mask, UINT64 value) override {
        std::lock_guard lock(mutex_);
        for (Entry& e : entries_)
            if (e.type == Entry::Type::Request && (e.request.CancellationTag & mask) == value) e.cancelled = true;
    }

    // Submits what is enqueued, waits until the worker has run all of it and stops the worker. Later enqueues are
    // dropped and recorded as E_DSTORAGE_QUEUE_CLOSED.
    void STDMETHODCALLTYPE Close() override {
        std::lock_guard close_lock(close_mutex_);
        {
            std::unique_lock lock(mutex_);
            if (closed_) return;
            closing_ = true;
            submit_locked();
            space_.notify_all();  // an enqueue waiting for space gives up (the queue is closing)
            idle_.wait(lock, [&] { return entries_.empty() && !in_flight_; });
            closed_ = true;
            work_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
    }

    HANDLE STDMETHODCALLTYPE GetErrorEvent() override {
        HANDLE dup = nullptr;
        if (!error_event_ || !DuplicateHandle(GetCurrentProcess(), error_event_, GetCurrentProcess(), &dup, 0, FALSE,
                                              DUPLICATE_SAME_ACCESS))
            return nullptr;
        return dup;  // the reference says the title closes it
    }

    void STDMETHODCALLTYPE RetrieveErrorRecord(DSTORAGE_ERROR_RECORD* record) override {
        if (!record) {
            bad_call("IDStorageQueueX::RetrieveErrorRecord", E_POINTER, "null record");
            return;
        }
        std::lock_guard lock(error_mutex_);
        *record = record_;
        clear_record_locked();
        if (error_event_) ResetEvent(error_event_);
    }

    void STDMETHODCALLTYPE Query(DSTORAGE_QUEUE_INFO* info) override {
        if (!info) {
            bad_call("IDStorageQueueX::Query", E_POINTER, "null info");
            return;
        }
        DSTORAGE_QUEUE_INFO1 full{};
        Query1(&full);
        info->Desc.SourceType = full.Desc.SourceType;
        info->Desc.Capacity = full.Desc.Capacity;
        info->Desc.Priority = full.Desc.Priority;
        info->Desc.Name = full.Desc.Name;
        info->EmptySlotCount = full.EmptySlotCount;
        info->RequestCountUntilAutoSubmit = full.RequestCountUntilAutoSubmit;
    }

    // An empty queue has capacity - 1 empty slots (one slot tells empty from full). The automatic submit happens when
    // the entries enqueued since the last submit reach half the capacity; with it disabled the count is 0.
    void STDMETHODCALLTYPE Query1(DSTORAGE_QUEUE_INFO1* info) override {
        if (!info) {
            bad_call("IDStorageQueueX2::Query1", E_POINTER, "null info");
            return;
        }
        std::lock_guard lock(mutex_);
        info->Desc = desc_;
        info->EmptySlotCount = static_cast<UINT16>(usable() - used_locked());
        const size_t unsubmitted = entries_.size() - submitted_;
        info->RequestCountUntilAutoSubmit =
            auto_submit() ? static_cast<UINT16>(half() > unsubmitted ? half() - unsubmitted : 0) : 0;
    }

private:
    size_t usable() const { return desc_.Capacity - 1u; }
    size_t half() const { return desc_.Capacity / 2u; }
    bool auto_submit() const { return !desc_.Options.AutoSubmitDisabled; }
    size_t used_locked() const { return entries_.size() + (in_flight_ ? 1 : 0); }

    void enqueue(Entry&& e) {
        {
            std::unique_lock lock(mutex_);
            // A full queue waits for the worker to free a slot. With auto-submit on, half the queue is always
            // submitted by then; with it off, only a Submit from another thread frees one (as the reference warns).
            space_.wait(lock, [&] { return closing_ || used_locked() < usable(); });
            if (!closing_) {
                entries_.push_back(std::move(e));
                if (auto_submit() && entries_.size() - submitted_ >= half()) submit_locked();
                return;
            }
        }
        DSTORAGE_ERROR_FIRST_FAILURE f{};
        f.HResult = E_DSTORAGE_QUEUE_CLOSED;
        describe(e, f);
        record_failure(f);
        if (e.type == Entry::Type::Status && SUCCEEDED(e.enqueue_error))
            e.status->complete(e.index, E_DSTORAGE_QUEUE_CLOSED);
        log_once("closed", "an entry was enqueued on a closed queue (E_DSTORAGE_QUEUE_CLOSED)");
        release_references(e);
    }

    void submit_locked() {
        uint32_t requests = 0;
        uint64_t source_bytes = 0, destination_bytes = 0;
        for (size_t i = submitted_; i < entries_.size(); ++i) {
            Entry& e = entries_[i];
            if (e.file) e.file_counted = e.file->add_pending();
            if (e.counted) {
                ++requests;
                source_bytes += e.request.SourceSize;
                destination_bytes += e.request.DestinationSize;
            }
        }
        if (requests) {
            std::lock_guard stats_lock(stats().mutex);
            auto& p = priority_stats(stats().s, desc_.Priority);
            p.TotalRequestCount += requests;
            p.TotalRequestSourceByteCount += source_bytes;
            p.TotalRequestDestinationByteCount += destination_bytes;
        }
        if (entries_.size() > submitted_) {
            submitted_ = entries_.size();
            work_.notify_one();
        }
    }

    void run() {
        for (;;) {
            Entry e;
            {
                std::unique_lock lock(mutex_);
                work_.wait(lock, [&] { return submitted_ > 0 || closed_; });
                if (submitted_ == 0) return;  // closed and drained
                e = std::move(entries_.front());
                entries_.pop_front();
                --submitted_;
                in_flight_ = true;
            }
            process(e);
            release_references(e);
            {
                std::lock_guard lock(mutex_);
                in_flight_ = false;
                space_.notify_all();
                if (entries_.empty()) idle_.notify_all();
            }
        }
    }

    // Runs on the worker. since_notify_ is the first failure since the last status entry or fence signal: the
    // HRESULT a status entry reports.
    void process(Entry& e) {
        HRESULT hr = e.enqueue_error;
        switch (e.type) {
            case Entry::Type::Request:
                // A cancelled request is a success whatever else is wrong with it (the GDK: not counted as a failure).
                if (e.cancelled) {
                    hr = S_OK;
                } else if (SUCCEEDED(hr)) {
                    // Pending: processing has started and not completed (DSTORAGE_PRIORITY_STATS).
                    set_pending(+1);
                    hr = execute_request(e);
                    set_pending(-1);
                }
                if (e.file && e.file_counted) e.file->finish_pending();
                if (FAILED(hr) && SUCCEEDED(since_notify_)) since_notify_ = hr;
                break;
            case Entry::Type::Status:
                if (SUCCEEDED(hr)) {
                    e.status->complete(e.index, since_notify_);
                    since_notify_ = S_OK;
                }
                break;
            case Entry::Type::Signal:
                if (SUCCEEDED(hr)) hr = e.fence->Signal(e.value);
                since_notify_ = S_OK;
                break;
            case Entry::Type::Event:
                if (SUCCEEDED(hr) && !SetEvent(e.event)) hr = HRESULT_FROM_WIN32(GetLastError());
                break;
        }
        if (FAILED(hr)) {
            DSTORAGE_ERROR_FIRST_FAILURE f{};
            f.HResult = hr;
            describe(e, f);
            record_failure(f);
        }
    }

    HRESULT execute_request(const Entry& e) {
        const DSTORAGE_REQUEST& r = e.request;
        const uint64_t o = r.Options.AsUINT64;
        if (o & options::kReservedMask) return E_DSTORAGE_RESERVED_FIELDS;
        if (options::source_type(o) != desc_.SourceType) return E_DSTORAGE_INVALID_SOURCE_TYPE;
        if (options::bcpack(o) > DSTORAGE_BCPACK_MODE_BC7) return E_DSTORAGE_INVALID_BCPACK_MODE;
        if (options::swizzle(o) > DSTORAGE_SWIZZLE_MODE_4K_12K_4K_12K) return E_DSTORAGE_INVALID_SWIZZLE_MODE;
        if (options::bcpack(o) != 0 || options::swizzle(o) != 0) {
            log_once("decompress", "BCPack and swizzle requests are not implemented yet (E_NOTIMPL)");
            return E_NOTIMPL;
        }
        if (o & (options::kDestinationPages | options::kSourcePages)) {
            log_once("pages", "physical-page requests are not implemented yet (E_NOTIMPL)");
            return E_NOTIMPL;
        }
        const bool zlib = options::zlib(o);
        // IntermediateSize is the BCPack data's size between the inflate and the BCPack decode (DSTORAGE_REQUEST), so
        // it is 0 for every request that gets here (BCPack is refused above).
        if (r.IntermediateSize != 0) return E_DSTORAGE_INVALID_INTERMEDIATE_SIZE;
        // E_DSTORAGE_INVALID_DESTINATION_SIZE: without decompression DestinationSize must equal the source's length,
        // with it it must be larger (dstorageerr.h's description; DSTORAGE_REQUEST says the same of DestinationSize).
        if (zlib ? r.DestinationSize <= r.SourceSize : r.DestinationSize != r.SourceSize)
            return E_DSTORAGE_INVALID_DESTINATION_SIZE;
        const bool memory = desc_.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY;
        const uint64_t total = static_cast<uint64_t>(r.DestinationSize) + (memory ? r.SourceSize : 0u);
        if (total > kMaxRequestBytes) return E_DSTORAGE_REQUEST_TOO_LARGE;
        // DSTORAGE_REQUEST: a buffer over 32 MiB is allowed only with 64 KiB or 2 MiB pages, which are not implemented
        // yet (refused above). The source is never larger than the destination here (the same size without zlib,
        // smaller with it), so this covers both buffers, and bounds the staging buffers of a zlib request too.
        if (r.DestinationSize > kMax4kPageDestinationSize) return E_DSTORAGE_REQUEST_TOO_LARGE;
        // DSTORAGE_REQUEST: with ZlibDecompress the file offset must be 16-byte aligned.
        if (zlib && !memory && r.FileOffset % 16 != 0) return E_DSTORAGE_INVALID_FILE_OFFSET;
        if (!r.Destination) return E_INVALIDARG;
        if (memory) {
            if (!r.Source) return E_INVALIDARG;
        } else {
            if (!r.File) return E_INVALIDARG;
            if (!e.file_counted) return E_DSTORAGE_FILE_NOT_OPEN;
        }
        if (zlib) return inflate_request(e, memory);
        if (r.SourceSize == 0) return S_OK;  // nothing to move; the reference does not say this fails
        if (memory) return guarded_move(r.Destination, r.Source, r.SourceSize) ? S_OK : E_DSTORAGE_ACCESS_VIOLATION;
        return read_file_range(e.file->handle(), r.FileOffset, r.Destination, r.SourceSize);
    }

    // A zlib request whose checks have passed: the source (SourceSize bytes of the file or of memory) goes into a
    // staging buffer, inflates there, and only a result of exactly DestinationSize bytes is copied to the destination.
    // So a failed stream leaves the destination as it was, and a memory source that overlaps its destination is safe.
    // Both buffers are at most 32 MiB (execute_request). The stream is RFC 1950: the two header bytes, the deflate
    // data, the Adler-32 of the result; bytes after the Adler-32 are not read. A failure is the matching
    // dstorageerr.h code: a bad header (not deflate, a window over 32 KiB, a bad check value or a preset dictionary)
    // E_DSTORAGE_ZLIB_BAD_HEADER, bad or truncated deflate data E_DSTORAGE_ZLIB_BAD_DATA, an Adler-32 mismatch
    // E_DSTORAGE_ZLIB_PARITY_FAIL, and a stream that inflates to more or fewer bytes than DestinationSize
    // E_DSTORAGE_DECOMPRESSION_ERROR (no code names a size mismatch; this is the header's generic one).
    HRESULT inflate_request(const Entry& e, bool memory) {
        const DSTORAGE_REQUEST& r = e.request;
        std::vector<uint8_t> source, result;
        try {
            source.resize(r.SourceSize);
            result.reserve(r.DestinationSize);
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
        if (r.SourceSize != 0) {
            if (memory) {
                if (!guarded_move(source.data(), r.Source, r.SourceSize)) return E_DSTORAGE_ACCESS_VIOLATION;
            } else if (const HRESULT hr = read_file_range(e.file->handle(), r.FileOffset, source.data(), r.SourceSize);
                       FAILED(hr)) {
                return hr;
            }
        }
        log_once("zlib", "zlib requests are inflated on the CPU (RFC 1950, leonida::zlib_inflate)");
        set_decompression_pending(+1);
        leonida::ZlibError why = leonida::ZlibError::none;
        const bool inflated = leonida::zlib_inflate(source, result, r.DestinationSize, &why);
        set_decompression_pending(-1);
        HRESULT hr = S_OK;
        if (!inflated) {
            switch (why) {
                case leonida::ZlibError::header: hr = E_DSTORAGE_ZLIB_BAD_HEADER; break;
                case leonida::ZlibError::checksum: hr = E_DSTORAGE_ZLIB_PARITY_FAIL; break;
                case leonida::ZlibError::limit: hr = E_DSTORAGE_DECOMPRESSION_ERROR; break;  // longer than the destination
                default: hr = E_DSTORAGE_ZLIB_BAD_DATA; break;
            }
        } else if (result.size() != r.DestinationSize) {
            hr = E_DSTORAGE_DECOMPRESSION_ERROR;  // shorter than the destination
        }
        if (FAILED(hr)) {
            std::lock_guard lock(stats().mutex);
            ++stats().s.DecompressionErrors;
            return hr;
        }
        return guarded_move(r.Destination, result.data(), result.size()) ? S_OK : E_DSTORAGE_ACCESS_VIOLATION;
    }

    void set_pending(int delta) {
        std::lock_guard lock(stats().mutex);
        priority_stats(stats().s, desc_.Priority).PendingRequestCount += static_cast<uint32_t>(delta);
    }

    // PendingDecompressionRequestCount is the zlib requests being inflated; TotalDecompressionRequestCount counts each
    // one that reached its inflate (its source was read), DecompressionErrors those whose stream then failed.
    static void set_decompression_pending(int delta) {
        std::lock_guard lock(stats().mutex);
        auto& s = stats().s;
        s.PendingDecompressionRequestCount += static_cast<uint32_t>(delta);
        if (delta > 0) ++s.TotalDecompressionRequestCount;
    }

    void describe(const Entry& e, DSTORAGE_ERROR_FIRST_FAILURE& f) const {
        switch (e.type) {
            case Entry::Type::Request: {
                f.CommandType = DSTORAGE_COMMAND_TYPE_REQUEST;
                auto& q = f.Request;
                const DSTORAGE_REQUEST& r = e.request;
                q.Options = r.Options;
                q.Destination = r.Destination;
                q.DestinationSize = r.DestinationSize;
                q.SourceSize = r.SourceSize;
                q.CancellationTag = r.CancellationTag;
                if (options::source_type(r.Options.AsUINT64) == DSTORAGE_REQUEST_SOURCE_MEMORY) {
                    q.Source = r.Source;
                } else {
                    q.FileOffset = r.FileOffset;
                    if (e.file) wcsncpy_s(q.Filename, e.file->path().c_str(), _TRUNCATE);
                }
                // The reference says the name stays readable until the request completes.
                if (r.Name)
                    for (size_t i = 0; i + 1 < kRequestMaxName && r.Name[i]; ++i) q.Name[i] = r.Name[i];
                break;
            }
            case Entry::Type::Status:
                f.CommandType = DSTORAGE_COMMAND_TYPE_STATUS;
                f.Status.StatusArray = e.status_as_given;
                f.Status.Index = e.index;
                break;
            case Entry::Type::Signal:
                f.CommandType = DSTORAGE_COMMAND_TYPE_SIGNAL;
                f.Signal.Fence = e.fence;
                f.Signal.Value = e.value;
                break;
            case Entry::Type::Event:
                f.CommandType = kCommandTypeEvent;
                break;
        }
    }

    void record_failure(const DSTORAGE_ERROR_FIRST_FAILURE& f) {
        count_error();
        {
            std::lock_guard lock(error_mutex_);
            if (record_.FailureCount++ == 0) record_.FirstFailure = f;
            if (error_event_) SetEvent(error_event_);
        }
        log_once("fail" + hr_text(f.HResult) + std::to_string(f.CommandType),
                 "an entry failed with " + hr_text(f.HResult) + " (command type " + std::to_string(f.CommandType) +
                     "; logged once per code)");
    }

    void clear_record_locked() {
        record_ = DSTORAGE_ERROR_RECORD{};
        record_.FirstFailure.CommandType = DSTORAGE_COMMAND_TYPE_NONE;
    }

    static void release_references(Entry& e) {
        if (e.file) e.file->Release();
        if (e.status) e.status->Release();
        if (e.fence) e.fence->Release();
        if (e.event) CloseHandle(e.event);
        e.event = nullptr;
        e.file = nullptr;
        e.status = nullptr;
        e.fence = nullptr;
    }

    std::atomic<ULONG> refs_{1};
    const DSTORAGE_QUEUE_DESC1 desc_;
    std::mutex close_mutex_;  // one Close at a time
    std::mutex mutex_;        // entries_, submitted_, in_flight_, closing_, closed_
    std::condition_variable work_, space_, idle_;
    std::deque<Entry> entries_;
    size_t submitted_ = 0;  // the first submitted_ entries are submitted
    bool in_flight_ = false;
    bool closing_ = false;
    bool closed_ = false;
    std::thread worker_;
    HRESULT since_notify_ = S_OK;  // worker only
    std::mutex error_mutex_;
    DSTORAGE_ERROR_RECORD record_{};
    HANDLE error_event_ = nullptr;
};

// ---- The factory ----------------------------------------------------------------------------------------------------

HRESULT validate_queue_desc(const DSTORAGE_QUEUE_DESC1& d) {
    if (d.SourceType != DSTORAGE_REQUEST_SOURCE_FILE && d.SourceType != DSTORAGE_REQUEST_SOURCE_MEMORY)
        return E_DSTORAGE_INVALID_SOURCE_TYPE;
    if (d.Capacity < kMinQueueCapacity || d.Capacity > kMaxQueueCapacity) return E_DSTORAGE_INVALID_QUEUE_CAPACITY;
    if (d.Priority < DSTORAGE_PRIORITY_FIRST || d.Priority > DSTORAGE_PRIORITY_LAST)
        return E_DSTORAGE_INVALID_QUEUE_PRIORITY;
    if (d.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY && d.Priority != DSTORAGE_PRIORITY_REALTIME)
        return E_DSTORAGE_INVALID_MEMORY_QUEUE_PRIORITY;
    if (d.Options.Reserved) return E_DSTORAGE_RESERVED_FIELDS;
    return S_OK;
}

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

// The title's own CreateFileW: the GAME: mapping and overlay when file_system.cpp is linked, else the host's.
CreateFileWFn create_file_w() {
    if (void* fn = rt::shims::find("kernel32.dll", "CreateFileW")) return reinterpret_cast<CreateFileWFn>(fn);
    return &::CreateFileW;
}

class Factory final : public IDStorageFactoryX2 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!iid_policy().accept_query(riid, Kind::Factory, "IDStorageFactoryX::QueryInterface")) return E_NOINTERFACE;
        AddRef();
        *ppv = static_cast<IDStorageFactoryX2*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    // One factory for the process; its count never drops below one, so it is never freed.
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = refs_.load();
        while (r > 1 && !refs_.compare_exchange_weak(r, r - 1)) {}
        return r > 1 ? r - 1 : 1;
    }

    HRESULT STDMETHODCALLTYPE CreateQueue(const DSTORAGE_QUEUE_DESC* desc, REFIID riid, void** ppv) override {
        if (!ppv) return bad_call("IDStorageFactoryX::CreateQueue", E_POINTER, "null ppv");
        *ppv = nullptr;
        if (!desc) return bad_call("IDStorageFactoryX::CreateQueue", E_INVALIDARG, "null desc");
        DSTORAGE_QUEUE_DESC1 d{};
        d.SourceType = desc->SourceType;
        d.Capacity = desc->Capacity;
        d.Priority = desc->Priority;
        d.Name = desc->Name;
        return create_queue(d, riid, ppv, "IDStorageFactoryX::CreateQueue");
    }

    HRESULT STDMETHODCALLTYPE CreateQueue1(const DSTORAGE_QUEUE_DESC1* desc, REFIID riid, void** ppv) override {
        if (!ppv) return bad_call("IDStorageFactoryX2::CreateQueue1", E_POINTER, "null ppv");
        *ppv = nullptr;
        if (!desc) return bad_call("IDStorageFactoryX2::CreateQueue1", E_INVALIDARG, "null desc");
        return create_queue(*desc, riid, ppv, "IDStorageFactoryX2::CreateQueue1");
    }

    HRESULT STDMETHODCALLTYPE OpenFile(const WCHAR* path, REFIID riid, void** ppv) override {
        if (!ppv) return bad_call("IDStorageFactoryX::OpenFile", E_POINTER, "null ppv");
        *ppv = nullptr;
        if (!path || !*path) return bad_call("IDStorageFactoryX::OpenFile", E_INVALIDARG, "null or empty path");
        if (!iid_policy().accept_creating(riid, Kind::File, "IDStorageFactoryX::OpenFile")) return E_NOINTERFACE;
        const HANDLE h = create_file_w()(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            return HRESULT_FROM_WIN32(error ? error : ERROR_FILE_NOT_FOUND);
        }
        File* f = new (std::nothrow) File(h, path);
        if (!f) {
            CloseHandle(h);
            return E_OUTOFMEMORY;
        }
        *ppv = static_cast<IDStorageFileX*>(f);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateStatusArray(UINT32 capacity, PCSTR, REFIID riid, void** ppv) override {
        if (!ppv) return bad_call("IDStorageFactoryX::CreateStatusArray", E_POINTER, "null ppv");
        *ppv = nullptr;
        if (capacity == 0) return bad_call("IDStorageFactoryX::CreateStatusArray", E_INVALIDARG, "capacity 0");
        if (!iid_policy().accept_creating(riid, Kind::StatusArray, "IDStorageFactoryX::CreateStatusArray"))
            return E_NOINTERFACE;
        std::unique_ptr<std::atomic<HRESULT>[]> entries(new (std::nothrow) std::atomic<HRESULT>[capacity]);
        if (!entries) return E_OUTOFMEMORY;
        auto* a = new (std::nothrow) StatusArray(capacity, std::move(entries));
        if (!a) return E_OUTOFMEMORY;
        *ppv = static_cast<IDStorageStatusArrayX*>(a);
        return S_OK;
    }

    // DirectStorage's own worker work would be pinned to these cores on the console. The mask names console cores,
    // so it is stored and not applied to the host's.
    HRESULT STDMETHODCALLTYPE SetCpuAffinity(UINT64 affinity) override {
        cpu_affinity_.store(affinity);
        return S_OK;
    }
    void STDMETHODCALLTYPE SetDebugFlags(UINT32 flags) override { debug_flags_.store(flags); }

    HRESULT STDMETHODCALLTYPE SetStagingBufferSize(UINT32 size) override {
        if (std::find(std::begin(kStagingBufferSizes), std::end(kStagingBufferSizes), size) ==
            std::end(kStagingBufferSizes))
            return bad_call("IDStorageFactoryX::SetStagingBufferSize", E_DSTORAGE_INVALID_STAGING_BUFFER_SIZE,
                            "not one of 0, 16, 20, 24, 28 or 32 MiB");
        std::lock_guard lock(stats().mutex);
        if (stats().live_queues || stats().s.CurrentOpenFileCount) return E_DSTORAGE_STAGING_BUFFER_LOCKED;
        staging_size_ = size;  // there is no staging buffer here; the size is only kept
        return S_OK;
    }

    void STDMETHODCALLTYPE GetStats(DSTORAGE_STATS* out) override {
        if (!out) {
            bad_call("IDStorageFactoryX1::GetStats", E_POINTER, "null stats");
            return;
        }
        std::lock_guard lock(stats().mutex);
        *out = stats().s;
    }

private:
    HRESULT create_queue(const DSTORAGE_QUEUE_DESC1& desc, REFIID riid, void** ppv, const char* caller) {
        if (const HRESULT hr = validate_queue_desc(desc); FAILED(hr)) return bad_call(caller, hr, "invalid queue desc");
        if (!iid_policy().accept_creating(riid, Kind::Queue, caller)) return E_NOINTERFACE;
        auto* q = new (std::nothrow) Queue(desc);
        if (!q) return E_OUTOFMEMORY;
        if (!q->start()) {
            q->Release();
            return E_OUTOFMEMORY;
        }
        *ppv = static_cast<IDStorageQueueX2*>(q);
        return S_OK;
    }

    std::atomic<ULONG> refs_{1};
    std::atomic<uint64_t> cpu_affinity_{~0ull};
    std::atomic<uint32_t> debug_flags_{0};
    uint32_t staging_size_ = 32u << 20;  // under stats().mutex
};

Factory& factory() {
    static auto* f = new Factory;
    return *f;
}

HRESULT WINAPI dstorage_get_factory(REFIID riid, void** ppv) {
    if (!ppv) return bad_call("DStorageGetFactory", E_POINTER, "null ppv");
    *ppv = nullptr;
    if (!iid_policy().accept_creating(riid, Kind::Factory, "DStorageGetFactory")) return E_NOINTERFACE;
    Factory& f = factory();
    f.AddRef();
    *ppv = static_cast<IDStorageFactoryX2*>(&f);
    return S_OK;
}

}  // namespace

// The console module names; docs/directstorage.md has the evidence and the confidence for each.
SHIM_TABLE(dstorage,
    {"dstorage_xs.dll", "DStorageGetFactory", reinterpret_cast<void*>(&dstorage_get_factory)},
    {"xcrdapi.dll", "DStorageGetFactory", reinterpret_cast<void*>(&dstorage_get_factory)},
    {"dstorage.dll", "DStorageGetFactory", reinterpret_cast<void*>(&dstorage_get_factory)},
);
