#pragma once
// Buffer addresses in the title's own memory and the console VA a translated shader reaches them by
// (docs/d3d12_vulkan.md section 3.3, "Title-chosen addresses").
//
// A translated shader computes heap_base + console_va, with heap_base the GPU address arena's device address. The
// layer chooses every console VA it hands out, so they all land in the arena. The title does not always use them:
//   - device slot 133 places a buffer at the CPU address of the title's own pages (a user-mode Windows address, far above 4 GiB);
//   - the title points V#s and root views at its own memory (a per-frame constant ring, #8862 section 3.5).
// Both are far past the arena, so heap_base + VA reads nothing the layer made. An address inside such a range is
// therefore replaced, wherever the layer can see it, by the console VA of a GPU copy of that range: its arena offset
// when the copy is an arena range (a slot-133 shadow, or a window of the title's pages copied before every
// submission), or its device address minus heap_base (mod 2^64) when it has its own memory (an imported slot-133
// buffer). Addresses that are not in the title's memory are passed through unchanged.
//
// The table is process-wide, like the title's address space. The device behind it registers two hooks: a resolver
// that makes a GPU copy for an address in the title's memory that has none yet, and a scanner that rewrites the
// buffer V#s of a bound descriptor table before a submission. Without hooks the table only translates what was
// added to it, so it is tested without a GPU (title_va_alias_test).
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <shared_mutex>

namespace leonida {

class TitleVaAliases {
public:
    // Where the layer saw the address, for the counters.
    enum Kind : unsigned { root_view, created_view, table_view, vertex_view, index_view, kind_count };
    static constexpr const char* kKindNames[kind_count] = {"root view", "created view", "table V#", "vertex view",
                                                           "index view"};

    // An address in the title's memory: true, with `shader_va` what a shader must be given (the address itself when
    // no copy could be made). False: not the title's memory (an arena VA, a buffer of the layer's).
    using Resolver = bool (*)(void* context, std::uint64_t va, std::uint64_t bytes, std::uint64_t& shader_va);
    using TableScanner = void (*)(void* context, std::uint64_t table_va, std::uint32_t descriptors);

    // [title_va, title_va + bytes) is reached as [shader_va, shader_va + bytes). A range that overlaps a live one
    // replaces it (the title has placed a new buffer over pages it released). False for an empty or wrapping range.
    bool add(std::uint64_t title_va, std::uint64_t bytes, std::uint64_t shader_va) {
        if (!bytes || title_va > UINT64_MAX - bytes) return false;
        std::unique_lock lock(mutex_);
        erase_overlapping(title_va, title_va + bytes);
        ranges_[title_va] = {title_va + bytes, shader_va};
        count_.store(ranges_.size(), std::memory_order_release);
        return true;
    }

    // Forgets the range that starts at title_va, if it still maps to shader_va (a newer range at the same VA keeps
    // its own entry).
    void remove(std::uint64_t title_va, std::uint64_t shader_va) {
        std::unique_lock lock(mutex_);
        const auto it = ranges_.find(title_va);
        if (it != ranges_.end() && it->second.shader_va == shader_va) ranges_.erase(it);
        count_.store(ranges_.size(), std::memory_order_release);
    }

    // The address a shader must be given for `va` from the ranges added so far: translated inside one, unchanged
    // elsewhere. No copy is made here.
    std::uint64_t shader_va(std::uint64_t va) const {
        if (!count_.load(std::memory_order_acquire)) return va;  // the common case: no title range at all
        std::shared_lock lock(mutex_);
        auto it = ranges_.upper_bound(va);
        if (it == ranges_.begin()) return va;
        --it;
        if (va >= it->second.end) return va;
        return it->second.shader_va + (va - it->first);
    }

    // The free span around `va` (not in any range): [lo, hi). A copy made for `va` must stay inside it, so it never
    // replaces a range another buffer still uses.
    void gap(std::uint64_t va, std::uint64_t& lo, std::uint64_t& hi) const {
        std::shared_lock lock(mutex_);
        lo = 0;
        hi = UINT64_MAX;
        auto it = ranges_.upper_bound(va);
        if (it != ranges_.end()) hi = it->first;
        if (it != ranges_.begin()) lo = std::prev(it)->second.end;
    }

    // shader_va, and when `va` is in the title's memory with no copy yet, a copy made by the device's resolver.
    // Counted by kind: translated, or in the title's memory and left as it was.
    std::uint64_t resolve(std::uint64_t va, std::uint64_t bytes, Kind kind) {
        if (!va) return va;
        std::uint64_t to = shader_va(va);
        if (to != va) {
            translated_[kind].fetch_add(1, std::memory_order_relaxed);
            return to;
        }
        Resolver resolver;
        void* context;
        {
            std::lock_guard lock(hook_mutex_);
            resolver = resolver_;
            context = hook_context_;
        }
        if (!resolver || !resolver(context, va, bytes, to)) return va;
        (to != va ? translated_ : untranslated_)[kind].fetch_add(1, std::memory_order_relaxed);
        return to;
    }

    // Counts an address that the layer resolves through its buffer index instead (vertex and index views): only
    // whether it is in a title range.
    void note(std::uint64_t va, Kind kind) {
        if (va && shader_va(va) != va) untranslated_[kind].fetch_add(1, std::memory_order_relaxed);
    }

    // Before a submission: the device rewrites the buffer V#s of the `descriptors` slots the root signature declares
    // for the table at `table_va` (a console VA in one of its shader-visible CBV/SRV/UAV heaps).
    void scan_table(std::uint64_t table_va, std::uint32_t descriptors) {
        TableScanner scanner;
        void* context;
        {
            std::lock_guard lock(hook_mutex_);
            scanner = scanner_;
            context = hook_context_;
        }
        if (scanner) scanner(context, table_va, descriptors);
    }

    // The scan's cost, per ExecuteCommandLists that had tables to scan.
    void note_scan(std::uint64_t nanoseconds) {
        scans_.fetch_add(1, std::memory_order_relaxed);
        scan_ns_.fetch_add(nanoseconds, std::memory_order_relaxed);
        std::uint64_t seen = scan_max_ns_.load(std::memory_order_relaxed);
        while (nanoseconds > seen && !scan_max_ns_.compare_exchange_weak(seen, nanoseconds)) {
        }
    }
    std::uint64_t scans() const { return scans_.load(std::memory_order_relaxed); }
    std::uint64_t scan_ns() const { return scan_ns_.load(std::memory_order_relaxed); }
    std::uint64_t scan_max_ns() const { return scan_max_ns_.load(std::memory_order_relaxed); }

    // The title's own memory, as the xmem shims hand it out (XMemAlloc, XMemVirtualAlloc, the title's VirtualAlloc):
    // only an address in one of these ranges gets a copy. A base anywhere else (the runtime's own heap, a host DLL)
    // is left as it was and counted.
    void add_title_memory(std::uint64_t base, std::uint64_t bytes) {
        if (!base || !bytes || base > UINT64_MAX - bytes) return;
        std::uint64_t begin = base, end = base + bytes;
        std::unique_lock lock(memory_mutex_);
        // A commit inside a reserved range, or two that overlap, are one range.
        auto it = memory_.upper_bound(begin);
        if (it != memory_.begin() && std::prev(it)->second > begin) --it;
        while (it != memory_.end() && it->first < end) {
            begin = std::min(begin, it->first);
            end = std::max(end, it->second);
            it = memory_.erase(it);
        }
        memory_[begin] = end;
    }
    void remove_title_memory(std::uint64_t base) {
        std::unique_lock lock(memory_mutex_);
        memory_.erase(base);
    }
    // The title range that holds `va`: [lo, hi). False when none does.
    bool title_memory(std::uint64_t va, std::uint64_t& lo, std::uint64_t& hi) const {
        std::shared_lock lock(memory_mutex_);
        auto it = memory_.upper_bound(va);
        if (it == memory_.begin()) return false;
        --it;
        if (va >= it->second) return false;
        lo = it->first;
        hi = it->second;
        return true;
    }

    // A title that gives a descriptor heap its own backing memory (__XMemAllocationHooks__) writes descriptors there
    // itself, while shaders read the layer's heap. The layer that keeps that mapping registers a source: before a
    // bound table is scanned, it copies the slots the title wrote in its own memory over the layer's copy
    // ([real, real + bytes) is the layer heap's CPU memory). Returns how many slots it copied.
    using DescriptorSource = std::uint64_t (*)(void* context, std::uint8_t* real, std::size_t bytes);
    void set_descriptor_source(void* context, DescriptorSource source) {
        std::lock_guard lock(hook_mutex_);
        source_context_ = context;
        source_ = source;
    }
    void pull_descriptors(std::uint8_t* real, std::size_t bytes) {
        DescriptorSource source;
        void* context;
        {
            std::lock_guard lock(hook_mutex_);
            source = source_;
            context = source_context_;
        }
        if (source) pulled_.fetch_add(source(context, real, bytes), std::memory_order_relaxed);
    }
    std::uint64_t pulled() const { return pulled_.load(std::memory_order_relaxed); }

    // The first device to register keeps the hooks until it clears them.
    void set_hooks(void* context, Resolver resolver, TableScanner scanner) {
        std::lock_guard lock(hook_mutex_);
        if (hook_context_) return;
        hook_context_ = context;
        resolver_ = resolver;
        scanner_ = scanner;
    }
    void clear_hooks(void* context) {
        std::lock_guard lock(hook_mutex_);
        if (hook_context_ != context) return;
        hook_context_ = nullptr;
        resolver_ = nullptr;
        scanner_ = nullptr;
    }

    std::size_t size() const { return count_.load(std::memory_order_acquire); }
    std::uint64_t translated(Kind kind) const { return translated_[kind].load(std::memory_order_relaxed); }
    std::uint64_t untranslated(Kind kind) const { return untranslated_[kind].load(std::memory_order_relaxed); }

private:
    struct Range {
        std::uint64_t end;        // exclusive
        std::uint64_t shader_va;  // what the range's first byte becomes
    };

    void erase_overlapping(std::uint64_t begin, std::uint64_t end) {
        auto it = ranges_.lower_bound(begin);
        if (it != ranges_.begin()) {
            auto previous = std::prev(it);
            if (previous->second.end > begin) it = previous;
        }
        while (it != ranges_.end() && it->first < end) it = ranges_.erase(it);
    }

    mutable std::shared_mutex mutex_;
    std::map<std::uint64_t, Range> ranges_;  // by title VA
    std::atomic<std::size_t> count_{0};
    std::mutex hook_mutex_;
    void* hook_context_ = nullptr;
    Resolver resolver_ = nullptr;
    TableScanner scanner_ = nullptr;
    void* source_context_ = nullptr;
    DescriptorSource source_ = nullptr;
    std::atomic<std::uint64_t> pulled_{0};
    std::array<std::atomic<std::uint64_t>, kind_count> translated_{};
    std::array<std::atomic<std::uint64_t>, kind_count> untranslated_{};
    std::atomic<std::uint64_t> scans_{0}, scan_ns_{0}, scan_max_ns_{0};
    mutable std::shared_mutex memory_mutex_;
    std::map<std::uint64_t, std::uint64_t> memory_;  // the title's memory: base -> end
};

// The process's table: the device's title ranges and hooks, read by the command lists and descriptor writers.
inline TitleVaAliases& title_va_aliases() {
    static TitleVaAliases aliases;
    return aliases;
}

}  // namespace leonida
