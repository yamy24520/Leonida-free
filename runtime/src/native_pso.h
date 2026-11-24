#pragma once

// The description Xbox device slot 57 receives (native graphics pipelines) and how its stages find the game's own
// shader containers. Standard library only, so the parsing is tested on every platform.
//
// Layout (static read of the 1.0.2.1 title, LooperSalty, issue #7496 comment 7108014199): 0x108 bytes, the root
// signature at +0x00, pairs at +0x08 and +0x18, then seven blocks {code, size, records, size} at +0x28 + k * 0x20.
// Measured by KkelebekK (issue #7049 comment 7106627714): pair 0 is state blob A (380-388 bytes: blend, raster,
// depth, not decoded), pair 1 is state blob B (80-188 bytes: formats and input layout, decode_state_b), and each
// stage's records blob carries the source container's HASH digest after its 0x43 word (docs/fspx.md, kind 7).
// The block index is not trusted as the stage: the stage comes from the translator's index entry of the container.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace leonida::native_pso {

struct Pair {
    const void* data;
    std::uint64_t size;
};

struct StageBlock {
    Pair code;     // +0x00: the stage's native program
    Pair records;  // +0x10: kind-7 style records holding the container's HASH digest
};

inline constexpr std::size_t kStageBlocks = 7;

struct GraphicsDesc {
    const void* root_signature;  // +0x00
    Pair state_a;                // +0x08
    Pair state_b;                // +0x18
    StageBlock stages[kStageBlocks];  // +0x28
};

static_assert(sizeof(Pair) == 0x10 && sizeof(StageBlock) == 0x20, "pairs are {pointer, u64 size}");
static_assert(offsetof(GraphicsDesc, state_a) == 0x08 && offsetof(GraphicsDesc, state_b) == 0x18);
static_assert(offsetof(GraphicsDesc, stages) == 0x28 && sizeof(GraphicsDesc) == 0x108,
              "the slot-57 description is 0x108 bytes (issue #7496)");

// Far above the largest blob measured (a few KiB), and small enough for 32-bit offsets inside one.
inline constexpr std::uint64_t kPairLimit = 16u << 20;

// A pair is absent (both zero), present (both set, size within the limit), or malformed.
enum class PairState { absent, present, malformed };

// A table entry with size 0 is absent whatever its pointer (the title passes base + offset, measured on 1.0.2.1).
inline PairState pair_state(const Pair& p) {
    if (!p.size) return PairState::absent;
    if (!p.data || !p.size || p.size > kPairLimit) return PairState::malformed;
    return PairState::present;
}

inline std::span<const std::uint8_t> bytes(const Pair& p) {
    if (pair_state(p) != PairState::present) return {};
    return {static_cast<const std::uint8_t*>(p.data), static_cast<std::size_t>(p.size)};
}

inline std::uint32_t read_u32(std::span<const std::uint8_t> b, std::size_t at) {
    std::uint32_t v = 0;
    if (at + 4 <= b.size()) std::memcpy(&v, b.data() + at, 4);
    return v;
}

struct InputElement {
    std::string name;
    std::uint32_t semantic_index = 0;
    std::uint32_t format = 0;
    std::uint32_t input_slot = 0;
    std::uint32_t aligned_byte_offset = 0;
    std::uint32_t classification = 0;
    std::uint32_t step_rate = 0;
};

// State blob B (issue #7049 comment 7106627714). The u32 field widths after each name are a hypothesis that fits the
// measured sizes: 0x50 bytes with no element, and 188 bytes with POSITION, COLOR and TEXCOORD.
struct FixedState {
    std::array<std::uint8_t, 8> rtv_formats{};  // +0x10, DXGI_FORMAT values
    std::uint32_t sample_mask = 0;              // +0x1c
    std::uint8_t dsv_format = 0;                // +0x24
    std::uint8_t rt_count = 0;                  // +0x25
    std::uint8_t topology_type = 0;             // +0x26, low nibble
    std::uint8_t sample_count = 1;              // +0x28
    std::vector<InputElement> elements;         // count at +0x4c, packed from +0x50
};

inline constexpr std::size_t kStateBHeader = 0x50;
inline constexpr std::uint32_t kMaxInputElements = 32;  // D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT
inline constexpr std::uint32_t kMaxSemanticName = 256;

// Empty for a blob that is too short, names more than 8 targets, a topology type past PATCH (4), or an element
// list that runs past the blob or has a malformed name. Bytes after the last element are allowed.
inline std::optional<FixedState> decode_state_b(std::span<const std::uint8_t> b, const char** why = nullptr) {
    const auto fail = [&](const char* reason) {
        if (why) *why = reason;
        return std::nullopt;
    };
    if (b.size() < kStateBHeader) return fail("blob B shorter than its header");
    FixedState s;
    std::memcpy(s.rtv_formats.data(), b.data() + 0x10, 8);
    s.sample_mask = read_u32(b, 0x1c);
    s.dsv_format = b[0x24];
    s.rt_count = b[0x25];
    s.topology_type = b[0x26] & 0x0f;
    s.sample_count = b[0x28] ? b[0x28] : 1;
    if (s.rt_count > 8) return fail("blob B render target count over 8");
    if (s.topology_type > 4) return fail("blob B topology type over 4");
    const std::uint32_t count = read_u32(b, 0x4c);
    if (count > kMaxInputElements) return fail("blob B input element count over 32");
    std::size_t at = kStateBHeader;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (at + 4 > b.size()) return fail("blob B input element past the end");
        const std::uint32_t length = read_u32(b, at);
        at += 4;
        if (length < 2 || length > kMaxSemanticName || at + length + 24 > b.size())
            return fail("blob B semantic name length out of range");
        const auto* name = reinterpret_cast<const char*>(b.data() + at);
        if (name[length - 1] != '\0' || std::memchr(name, '\0', length - 1))
            return fail("blob B semantic name not one NUL-terminated string");
        InputElement e;
        e.name.assign(name, length - 1);
        at += length;
        e.semantic_index = read_u32(b, at);
        e.format = read_u32(b, at + 4);
        e.input_slot = read_u32(b, at + 8);
        e.aligned_byte_offset = read_u32(b, at + 12);
        e.classification = read_u32(b, at + 16);
        e.step_rate = read_u32(b, at + 20);
        at += 24;
        s.elements.push_back(std::move(e));
    }
    return s;
}

using Digest = std::array<std::uint8_t, 16>;

// Every 16 bytes that follow a 4-aligned 0x43 word, without duplicates. A register number can also read 0x43, so
// all candidates are returned and the map decides which one names a container.
inline std::vector<Digest> record_digests(std::span<const std::uint8_t> records) {
    std::vector<Digest> out;
    for (std::size_t at = 0; at + 4 + 16 <= records.size(); at += 4) {
        if (read_u32(records, at) != 0x43) continue;
        Digest d;
        std::memcpy(d.data(), records.data() + at + 4, 16);
        bool seen = false;
        for (const Digest& o : out) seen = seen || o == d;
        if (!seen) out.push_back(d);
    }
    return out;
}

inline std::string hex(const Digest& d) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    for (const std::uint8_t b : d) {
        s += kHex[b >> 4];
        s += kHex[b & 15];
    }
    return s;
}

inline std::optional<Digest> parse_digest(std::string_view text) {
    if (text.size() != 32) return std::nullopt;
    Digest d{};
    for (std::size_t i = 0; i < 32; ++i) {
        const char c = text[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return std::nullopt;
        d[i / 2] = static_cast<std::uint8_t>(d[i / 2] << 4 | v);
    }
    return d;
}

struct ShaderRef {
    std::string stage;  // the translator's stage: "vs", "ps", "gs", "ms", "cs"
    std::string path;   // the container, relative to the map file's folder, '/' separated
};

// native_hashes.txt, written by tools/native_pso_hashes.py next to the translator's index.json: a first line
// "# native_pso_hashes 1", then one "<32 hex digest>\t<stage>\t<relative container path>" line per digest.
class HashMap {
public:
    static constexpr std::string_view kHeader = "# native_pso_hashes 1";

    bool parse(std::string_view text, std::string* error = nullptr) {
        const auto fail = [&](std::string why) {
            if (error) *error = std::move(why);
            map_.clear();
            return false;
        };
        map_.clear();
        std::size_t line_no = 0;
        bool header = false;
        while (!text.empty()) {
            const std::size_t end = text.find('\n');
            std::string_view line = text.substr(0, end);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
            ++line_no;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line_no == 1) {
                if (line != kHeader) return fail("native_hashes.txt: unknown header");
                header = true;
                continue;
            }
            if (line.empty() || line.front() == '#') continue;
            const std::size_t t1 = line.find('\t');
            const std::size_t t2 = t1 == std::string_view::npos ? t1 : line.find('\t', t1 + 1);
            if (t2 == std::string_view::npos || t2 + 1 >= line.size())
                return fail("native_hashes.txt line " + std::to_string(line_no) + ": expected three fields");
            const auto digest = parse_digest(line.substr(0, t1));
            const std::string_view stage = line.substr(t1 + 1, t2 - t1 - 1);
            if (!digest || stage.empty())
                return fail("native_hashes.txt line " + std::to_string(line_no) + ": bad digest or stage");
            map_.try_emplace(*digest, ShaderRef{std::string(stage), std::string(line.substr(t2 + 1))});
        }
        if (!header) return fail("native_hashes.txt is empty");
        return true;
    }

    const ShaderRef* find(const Digest& d) const {
        const auto it = map_.find(d);
        return it == map_.end() ? nullptr : &it->second;
    }

    std::size_t size() const { return map_.size(); }

private:
    std::map<Digest, ShaderRef> map_;
};

struct PlannedStage {
    std::size_t block = 0;
    const ShaderRef* shader = nullptr;
};

enum class PlanStatus {
    ok,
    malformed,    // a pair is malformed or blob B does not decode
    missing,      // a stage has no records, or its digest names no translated container
    ambiguous,    // a stage's records name two different containers
    unsupported,  // a stage kind this path does not build (hs, ds, cs), two of one kind, or no VS/MS
};

struct Plan {
    PlanStatus status = PlanStatus::malformed;
    FixedState state;
    std::vector<PlannedStage> stages;  // in block order
    std::size_t failed_block = 0;      // for missing/ambiguous/unsupported/malformed blocks
    const char* reason = "";           // why a description is malformed
    bool mesh = false;                 // an MS stage: the pipeline goes through a pipeline state stream

    const ShaderRef* stage(std::string_view name) const {
        for (const PlannedStage& s : stages)
            if (s.shader->stage == name) return s.shader;
        return nullptr;
    }
};

inline Plan plan(const GraphicsDesc& desc, const HashMap& map) {
    Plan p;
    if (pair_state(desc.state_a) == PairState::malformed) return p.reason = "blob A pair malformed", p;
    if (pair_state(desc.state_b) != PairState::present) return p.reason = "blob B pair absent or malformed", p;
    auto state = decode_state_b(bytes(desc.state_b), &p.reason);
    if (!state) return p;
    p.state = std::move(*state);
    for (std::size_t k = 0; k < kStageBlocks; ++k) {
        const StageBlock& block = desc.stages[k];
        const PairState code = pair_state(block.code), records = pair_state(block.records);
        p.failed_block = k;
        if (code == PairState::malformed || records == PairState::malformed)
            return p.reason = "a stage pair malformed", p;
        if (code == PairState::absent && records == PairState::absent) continue;
        if (records != PairState::present) {  // code without records: nothing names its container
            p.status = PlanStatus::missing;
            return p;
        }
        const ShaderRef* found = nullptr;
        for (const Digest& d : record_digests(bytes(block.records))) {
            const ShaderRef* ref = map.find(d);
            if (!ref) continue;
            if (found && found != ref) {
                p.status = PlanStatus::ambiguous;
                p.failed_block = k;
                return p;
            }
            found = ref;
        }
        p.failed_block = k;
        if (!found) {
            p.status = PlanStatus::missing;
            return p;
        }
        const std::string& s = found->stage;
        if (!(s == "vs" || s == "ps" || s == "gs" || s == "ms") || p.stage(s)) {
            p.status = PlanStatus::unsupported;
            return p;
        }
        p.stages.push_back({k, found});
    }
    const bool vs = p.stage("vs"), ms = p.stage("ms");
    if (vs == ms || (ms && p.stage("gs"))) {
        p.status = PlanStatus::unsupported;
        return p;
    }
    p.mesh = ms;
    p.status = PlanStatus::ok;
    return p;
}

}  // namespace leonida::native_pso
