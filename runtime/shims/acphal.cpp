// AcpHal.dll: the console's audio hardware layer. The Apu* functions are a heap of memory the audio processor reads,
// with 32-bit "APU addresses" for it; AcpHalCreate returns the object commands go through and the shape contexts are
// the per-voice records the hardware works on. A PC has no audio processor: the heap is ordinary memory, an APU
// address is an offset into it, contexts are zeroed memory and commands are accepted and dropped (no sound).
//
// Measured from the game executable (one thunk per import; call-site counts in brackets), statically read:
//   HRESULT ApuCreateHeap(UINT32 a, UINT32 b)                 [1] a = MiB << 20, b = a byte count; failure is fatal
//   HRESULT ApuAlloc(void** va, UINT32* apu, UINT32 size, UINT32 align, UINT32 flags)
//                                                             [1] apu NULL, align 0x800, flags 2; result unchecked,
//                                                                 *va read straight after
//   ApuFree(void* va)                                         [1] tail call at shutdown, result ignored
//   bool ApuIsVirtualAddressValid(void* va, UINT32 align)     [4] align 0x40, 0x80 or 0x800; result in AL
//   UINT32 ApuMapVirtualAddress(void* va)                     [4] stored into a 32-bit context field; once with NULL
//   void* ApuMapApuAddress(UINT32 apu)                        [2] result tested for NULL, then written through
//   HRESULT AcpHalAllocateShapeContexts(contexts*)            [1] failure is fatal, see kShapeTypes
//   HRESULT AcpHalCreate(IAcpHal** out)                       [1] failure is fatal; the object's slots below
//   AcpHalReleaseShapeContexts()                              [1] no argument register set up; result ignored
// The argument meanings (cached/non-cached sizes, flags) and the names of the object's slots are hypotheses.
#include <windows.h>

#include <malloc.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <new>

#include "shims.h"

namespace {

constexpr HRESULT kEFail = static_cast<HRESULT>(0x80004005L);
constexpr HRESULT kEOutOfMemory = static_cast<HRESULT>(0x8007000EL);
constexpr HRESULT kEInvalidArg = static_cast<HRESULT>(0x80070057L);
constexpr HRESULT kEPointer = static_cast<HRESULT>(0x80004003L);

// Hypothesis: any nonzero base works, the title only passes APU addresses back to this DLL and to the hardware.
// Nonzero so that address 0 stays "none" (ApuMapVirtualAddress(NULL) is 0).
constexpr uint32_t kApuBase = 0x40000000;

std::mutex g_mutex;
uint8_t* g_heap = nullptr;
uint32_t g_heap_size = 0;
std::map<uint32_t, uint32_t> g_blocks;  // offset -> size of each live ApuAlloc block

void warn_once(std::atomic<bool>& flag, const char* text) {
    if (!flag.exchange(true, std::memory_order_relaxed)) std::fprintf(stderr, "[rt] acphal: %s\n", text);
}

// One region for both sizes: a PC has no cached/non-cached split. The single call site's second size is the first
// ApuAlloc's size plus 16 KiB, so flags 2 is probably the non-cached heap.
HRESULT ApuCreateHeap(uint32_t cached, uint32_t non_cached) {
    const uint64_t total = uint64_t(cached) + non_cached;
    if (total == 0 || total > 0xFFFFFFFFull - kApuBase) return kEInvalidArg;
    std::lock_guard lock(g_mutex);
    if (g_heap) return kEFail;  // shortcut: one heap per process, the only call site creates it once at boot
    g_heap = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_t(total), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_heap) return kEOutOfMemory;
    g_heap_size = static_cast<uint32_t>(total);
    return S_OK;
}

// First fit between the live blocks. The flags are not decoded (see ApuCreateHeap).
HRESULT ApuAlloc(void** va, uint32_t* apu, uint32_t size, uint32_t align, uint32_t /*flags*/) {
    if (!va) return kEPointer;
    *va = nullptr;
    if (align == 0) align = 1;
    if (size == 0 || (align & (align - 1)) != 0) return kEInvalidArg;
    std::lock_guard lock(g_mutex);
    if (!g_heap) return kEFail;
    uint64_t at = 0;
    for (auto it = g_blocks.begin();; ++it) {
        at = (at + align - 1) & ~uint64_t(align - 1);
        const uint64_t limit = it == g_blocks.end() ? g_heap_size : it->first;
        if (at + size <= limit) break;
        if (it == g_blocks.end()) return kEOutOfMemory;
        at = uint64_t(it->first) + it->second;
    }
    g_blocks.emplace(static_cast<uint32_t>(at), size);
    std::memset(g_heap + at, 0, size);
    *va = g_heap + at;
    if (apu) *apu = kApuBase + static_cast<uint32_t>(at);
    return S_OK;
}

HRESULT ApuFree(void* va) {
    std::lock_guard lock(g_mutex);
    auto* p = static_cast<uint8_t*>(va);
    if (!g_heap || p < g_heap || p >= g_heap + g_heap_size) return kEInvalidArg;
    return g_blocks.erase(static_cast<uint32_t>(p - g_heap)) ? S_OK : kEInvalidArg;
}

// TRUE when the address is inside a live block and its heap offset is a multiple of `align`.
BOOL ApuIsVirtualAddressValid(const void* va, uint32_t align) {
    if (align && (align & (align - 1)) != 0) return FALSE;
    std::lock_guard lock(g_mutex);
    auto* p = static_cast<const uint8_t*>(va);
    if (!g_heap || p < g_heap || p >= g_heap + g_heap_size) return FALSE;
    const uint32_t offset = static_cast<uint32_t>(p - g_heap);
    auto it = g_blocks.upper_bound(offset);
    if (it == g_blocks.begin()) return FALSE;
    --it;
    if (offset - it->first >= it->second) return FALSE;
    return align == 0 || (offset & (align - 1)) == 0;
}

uint32_t ApuMapVirtualAddress(const void* va) {
    std::lock_guard lock(g_mutex);
    auto* p = static_cast<const uint8_t*>(va);
    if (!g_heap || p < g_heap || p >= g_heap + g_heap_size) return 0;
    return kApuBase + static_cast<uint32_t>(p - g_heap);
}

void* ApuMapApuAddress(uint32_t apu) {
    std::lock_guard lock(g_mutex);
    if (!g_heap || apu < kApuBase || apu - kApuBase >= g_heap_size) return nullptr;
    return g_heap + (apu - kApuBase);
}

// The one caller zeroes 6 dwords, sets the fifth to 350, and afterwards reads a pointer at +0x38 and clears
// count * 64 bytes there; nothing else in the image touches the struct. Hypothesis: 6 counts, then 6 pointers, one
// per context type, 64 bytes per context (measured for the fifth type only; the others are zero at the call site).
constexpr int kShapeTypes = 6;
constexpr size_t kShapeContextSize = 64;
struct ShapeContexts {
    uint32_t count[kShapeTypes];
    void* contexts[kShapeTypes];
};
static_assert(offsetof(ShapeContexts, contexts) + 4 * sizeof(void*) == 0x38, "fifth pointer is read at +0x38");
void* g_shape[kShapeTypes] = {};

HRESULT AcpHalAllocateShapeContexts(ShapeContexts* s) {
    if (!s) return kEPointer;
    std::lock_guard lock(g_mutex);
    void* new_shape[kShapeTypes] = {};
    for (int i = 0; i < kShapeTypes; ++i) {
        if (!s->count[i]) continue;
        new_shape[i] = _aligned_malloc(s->count[i] * kShapeContextSize, 64);
        if (!new_shape[i]) {
            for (void* p : new_shape) _aligned_free(p);
            return kEOutOfMemory;
        }
        std::memset(new_shape[i], 0, s->count[i] * kShapeContextSize);
    }
    for (int i = 0; i < kShapeTypes; ++i) {
        _aligned_free(g_shape[i]);
        g_shape[i] = new_shape[i];
        if (s->count[i]) s->contexts[i] = g_shape[i];  // a zero count leaves its pointer alone: only +0x38 is measured
    }
    return S_OK;
}

HRESULT AcpHalReleaseShapeContexts() {
    std::lock_guard lock(g_mutex);
    for (void*& p : g_shape) {
        _aligned_free(p);
        p = nullptr;
    }
    return S_OK;
}

// Slots as the title calls them: 0 with (0x1000, 0x1000) once at boot, checked; 1 with no arguments at shutdown;
// 2 with a command id, two dwords, a pointer and a dword, checked once at boot; 5 last at shutdown, after which the
// title drops the pointer (taken as Release). Slots 3 and 4 are never called. Names are hypotheses.
struct AcpHal final {  // no virtual destructor: it would add a slot the title does not know
    virtual HRESULT connect(uint32_t, uint32_t) { return S_OK; }
    virtual HRESULT disconnect() { return S_OK; }
    virtual HRESULT submit_command(uint32_t, uint32_t, uint32_t, const void*, uint32_t) {
        static std::atomic<bool> warned{false};
        warn_once(warned, "AcpHal command accepted and dropped: no audio processor on a PC (logged once)");
        return S_OK;
    }
    // Not called by this title. Read elsewhere as PopMessage (bool) and GetNumMessages (count), a hypothesis; 0 is
    // "no message" under that reading and S_OK under any other, where E_NOTIMPL would read as a pending message.
    virtual uint32_t slot3() { return 0; }
    virtual uint32_t slot4() { return 0; }
    virtual HRESULT release() {
        delete this;
        return S_OK;
    }
};

HRESULT AcpHalCreate(AcpHal** out) {
    if (!out) return kEPointer;
    *out = new (std::nothrow) AcpHal;
    return *out ? S_OK : kEOutOfMemory;
}

}  // namespace

SHIM_TABLE(acphal,
    {"acphal.dll", "AcpHalAllocateShapeContexts", reinterpret_cast<void*>(&AcpHalAllocateShapeContexts)},
    {"acphal.dll", "AcpHalCreate", reinterpret_cast<void*>(&AcpHalCreate)},
    {"acphal.dll", "AcpHalReleaseShapeContexts", reinterpret_cast<void*>(&AcpHalReleaseShapeContexts)},
    {"acphal.dll", "ApuAlloc", reinterpret_cast<void*>(&ApuAlloc)},
    {"acphal.dll", "ApuCreateHeap", reinterpret_cast<void*>(&ApuCreateHeap)},
    {"acphal.dll", "ApuFree", reinterpret_cast<void*>(&ApuFree)},
    {"acphal.dll", "ApuIsVirtualAddressValid", reinterpret_cast<void*>(&ApuIsVirtualAddressValid)},
    {"acphal.dll", "ApuMapApuAddress", reinterpret_cast<void*>(&ApuMapApuAddress)},
    {"acphal.dll", "ApuMapVirtualAddress", reinterpret_cast<void*>(&ApuMapVirtualAddress)},
);
