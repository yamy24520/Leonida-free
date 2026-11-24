#pragma once

// One VkShaderModule per distinct SPIR-V, shared by every pipeline state built from it.
//
// A title creates tens of thousands of graphics pipeline states up front, most of them from a few thousand vertex and
// pixel shaders (measured on the retail boot under Wine: 50,468 graphics PSOs in the first tenth of the run, about
// 12.7 GB resident, #8851 A37/A42). A PipelineState keeps its modules for the variants it builds at draw time, so
// one module per PSO and stage kept a copy of the same shader per PSO. Here a module is created on the first acquire
// of its words and destroyed with the last release; a module that holds per-PSO edits (the depth24 patch, the f16
// link) has other words and gets its own module, so sharing never changes what a pipeline runs.
//
// Thread-safe: PSOs are created and released from any thread.

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace leonida {

class ShaderModuleCache {
public:
    // `create` returns VK_NULL_HANDLE when vkCreateShaderModule fails; `destroy` is vkDestroyShaderModule.
    using Create = std::function<VkShaderModule(const std::vector<std::uint32_t>& words)>;
    using Destroy = std::function<void(VkShaderModule)>;

    ShaderModuleCache(Create create, Destroy destroy) : create_(std::move(create)), destroy_(std::move(destroy)) {}
    ShaderModuleCache(const ShaderModuleCache&) = delete;
    ShaderModuleCache& operator=(const ShaderModuleCache&) = delete;

    // Releases nothing a caller still holds: every PipelineState releases its modules before the device goes.
    // Modules left over (a leaked PSO) are destroyed here, with the device still alive.
    ~ShaderModuleCache() {
        for (auto& [hash, bucket] : by_hash_)
            for (Entry& entry : bucket) destroy_(entry.module);
    }

    // The module for these words, created on first use. VK_NULL_HANDLE when creation fails (nothing is cached).
    VkShaderModule acquire(const std::vector<std::uint32_t>& words) {
        const std::uint64_t hash = hash_words(words);
        std::lock_guard lock(mutex_);
        std::vector<Entry>& bucket = by_hash_[hash];
        for (Entry& entry : bucket) {
            if (entry.words == words) {
                ++entry.references;
                ++shared_;
                return entry.module;
            }
        }
        const VkShaderModule module = create_(words);
        if (module == VK_NULL_HANDLE) {
            if (bucket.empty()) by_hash_.erase(hash);
            return VK_NULL_HANDLE;
        }
        bucket.push_back(Entry{words, module, 1});
        owner_[module] = hash;
        return module;
    }

    // One release per acquire that returned `module`; the last one destroys it. Unknown modules are ignored.
    void release(VkShaderModule module) {
        if (module == VK_NULL_HANDLE) return;
        std::lock_guard lock(mutex_);
        const auto owner = owner_.find(module);
        if (owner == owner_.end()) return;
        const auto bucket = by_hash_.find(owner->second);
        for (auto entry = bucket->second.begin(); entry != bucket->second.end(); ++entry) {
            if (entry->module != module) continue;
            if (--entry->references == 0) {
                destroy_(module);
                bucket->second.erase(entry);
                if (bucket->second.empty()) by_hash_.erase(bucket);
                owner_.erase(owner);
            }
            return;
        }
    }

    // Distinct modules alive, and acquires answered with an existing module (for the device's statistics line).
    std::size_t live() const {
        std::lock_guard lock(mutex_);
        return owner_.size();
    }
    std::uint64_t shared() const {
        std::lock_guard lock(mutex_);
        return shared_;
    }

private:
    struct Entry {
        std::vector<std::uint32_t> words;
        VkShaderModule module;
        std::uint32_t references;
    };

    static std::uint64_t hash_words(const std::vector<std::uint32_t>& words) {
        std::uint64_t hash = 14695981039346656037ull;  // FNV-1a over the words
        for (const std::uint32_t word : words) {
            hash ^= word;
            hash *= 1099511628211ull;
        }
        return hash ^ words.size();
    }

    Create create_;
    Destroy destroy_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::vector<Entry>> by_hash_;
    std::unordered_map<VkShaderModule, std::uint64_t> owner_;
    std::uint64_t shared_ = 0;
};

}  // namespace leonida
