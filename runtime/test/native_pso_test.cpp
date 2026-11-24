// Slot-57 description parsing (runtime/src/native_pso.h) on hand-built blobs: no game data, no D3D12, any platform.
#include "../src/native_pso.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace np = leonida::native_pso;

int g_failures = 0;
#define EXPECT(c) do { if (!(c)) { std::fprintf(stderr, "native_pso_test line %d: %s\n", __LINE__, #c); ++g_failures; } } while (0)

void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    if (b.size() < at + 4) b.resize(at + 4);
    std::memcpy(b.data() + at, &v, 4);
}

void append32(std::vector<std::uint8_t>& b, std::uint32_t v) { put32(b, b.size(), v); }

struct Element {
    const char* name;
    std::uint32_t index, format, slot, offset, classification, step;
};

// Blob B as issue #7049 describes it.
std::vector<std::uint8_t> state_b(std::vector<Element> elements, std::uint8_t rt_count = 1, std::uint8_t topology = 3) {
    std::vector<std::uint8_t> b(np::kStateBHeader, 0);
    b[0x10] = 29;  // R8G8B8A8_UNORM_SRGB
    b[0x11] = 28;
    put32(b, 0x1c, 0xffffffffu);
    b[0x24] = 45;  // D24_UNORM_S8_UINT
    b[0x25] = rt_count;
    b[0x26] = static_cast<std::uint8_t>(0xa0 | topology);
    b[0x28] = 1;
    put32(b, 0x4c, static_cast<std::uint32_t>(elements.size()));
    for (const Element& e : elements) {
        const std::size_t length = std::strlen(e.name) + 1;
        append32(b, static_cast<std::uint32_t>(length));
        b.insert(b.end(), e.name, e.name + length);
        for (const std::uint32_t v : {e.index, e.format, e.slot, e.offset, e.classification, e.step}) append32(b, v);
    }
    return b;
}

const std::vector<Element> kThree = {{"POSITION", 0, 6, 0, 0, 0, 0},
                                     {"COLOR", 0, 28, 0, 12, 0, 0},
                                     {"TEXCOORD", 0, 16, 0, 16, 0, 0}};

std::vector<std::uint8_t> records(const np::Digest& d, bool decoy = false) {
    std::vector<std::uint8_t> b;
    append32(b, 0x403);
    append32(b, 0x41);
    append32(b, 0);
    append32(b, 0);
    append32(b, decoy ? 0x43 : 7);  // a register number that reads 0x43
    append32(b, 0x43);
    b.insert(b.end(), d.begin(), d.end());
    append32(b, 0);
    return b;
}

np::Digest digest(std::uint8_t seed) {
    np::Digest d;
    for (std::size_t i = 0; i < d.size(); ++i) d[i] = static_cast<std::uint8_t>(seed + i * 17);
    return d;
}

std::string map_line(const np::Digest& d, const char* stage, const char* path) {
    return np::hex(d) + "\t" + stage + "\t" + path + "\n";
}

void test_state_b() {
    const auto b = state_b(kThree);
    EXPECT(b.size() == 188);  // the largest blob B measured
    const auto s = np::decode_state_b(b);
    EXPECT(s.has_value());
    if (!s) return;
    EXPECT(s->rtv_formats[0] == 29 && s->rtv_formats[1] == 28 && s->rt_count == 1);
    EXPECT(s->sample_mask == 0xffffffffu && s->dsv_format == 45 && s->topology_type == 3 && s->sample_count == 1);
    EXPECT(s->elements.size() == 3);
    EXPECT(s->elements[1].name == "COLOR" && s->elements[1].format == 28 && s->elements[1].aligned_byte_offset == 12);
    EXPECT(s->elements[2].name == "TEXCOORD" && s->elements[2].aligned_byte_offset == 16);

    const auto none = np::decode_state_b(state_b({}));
    EXPECT(none && none->elements.empty());
    EXPECT(state_b({}).size() == 80);  // the smallest blob B measured

    auto zero_samples = state_b({});
    zero_samples[0x28] = 0;
    EXPECT(np::decode_state_b(zero_samples)->sample_count == 1);

    // Truncations at every length refuse the blob.
    for (std::size_t n = 0; n < b.size(); ++n) {
        if (n == np::kStateBHeader) continue;
        const std::vector<std::uint8_t> cut(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n));
        if (np::decode_state_b(cut)) {
            std::fprintf(stderr, "native_pso_test: truncated blob of %zu bytes accepted\n", n);
            ++g_failures;
        }
    }
    auto trailing = b;
    trailing.resize(b.size() + 8);
    EXPECT(np::decode_state_b(trailing).has_value());

    EXPECT(!np::decode_state_b(state_b({}, 9)));
    EXPECT(!np::decode_state_b(state_b({}, 1, 5)));
    auto many = state_b({});
    put32(many, 0x4c, np::kMaxInputElements + 1);
    EXPECT(!np::decode_state_b(many));
    auto unterminated = state_b({{"POS", 0, 6, 0, 0, 0, 0}});
    unterminated[0x50 + 4 + 3] = 'X';
    EXPECT(!np::decode_state_b(unterminated));
    auto interior = state_b({{"POS", 0, 6, 0, 0, 0, 0}});
    interior[0x50 + 4 + 1] = '\0';
    EXPECT(!np::decode_state_b(interior));
    auto empty_name = state_b({});
    put32(empty_name, 0x4c, 1);
    append32(empty_name, 1);
    empty_name.resize(empty_name.size() + 25);
    EXPECT(!np::decode_state_b(empty_name));
}

void test_digests_and_map() {
    const np::Digest a = digest(1), b = digest(2);
    EXPECT(np::record_digests(records(a)).size() == 1 && np::record_digests(records(a))[0] == a);
    EXPECT(np::record_digests(records(a, true)).size() == 2);
    auto cut = records(a);
    cut.resize(cut.size() - 8);
    EXPECT(np::record_digests(cut).empty());
    EXPECT(np::parse_digest(np::hex(a)) == a);
    EXPECT(!np::parse_digest("00") && !np::parse_digest(std::string(32, 'G')) && !np::parse_digest(std::string(32, 'A')));

    np::HashMap map;
    std::string error;
    EXPECT(map.parse(std::string(np::HashMap::kHeader) + "\r\n# comment\n" + map_line(a, "vs", "../dxbc/a.dxbc") +
                     "\n" + map_line(b, "ps", "../dxbc/b c.dxbc"), &error));
    EXPECT(map.size() == 2 && map.find(a) && map.find(a)->stage == "vs" && map.find(b)->path == "../dxbc/b c.dxbc");
    EXPECT(!map.find(digest(3)));
    EXPECT(!map.parse("", &error) && map.size() == 0);
    EXPECT(!map.parse("# native_pso_hashes 2\n", &error));
    EXPECT(!map.parse(std::string(np::HashMap::kHeader) + "\n" + np::hex(a) + "\tvs\n", &error));
    EXPECT(!map.parse(std::string(np::HashMap::kHeader) + "\nxyz\tvs\tp\n", &error));
    EXPECT(!map.parse(std::string(np::HashMap::kHeader) + "\n" + np::hex(a) + "\t\tp\n", &error));
}

struct Fixture {
    std::vector<std::uint8_t> a, b, code[np::kStageBlocks], rec[np::kStageBlocks];
    np::GraphicsDesc desc{};

    Fixture() {
        a.assign(384, 0);
        b = state_b(kThree);
        desc.root_signature = reinterpret_cast<const void*>(0x1000);
        desc.state_a = {a.data(), a.size()};
        desc.state_b = {b.data(), b.size()};
    }
    void stage(std::size_t k, const np::Digest& d) {
        code[k].assign(256, 0xbf);
        rec[k] = records(d);
        desc.stages[k] = {{code[k].data(), code[k].size()}, {rec[k].data(), rec[k].size()}};
    }
};

void test_plan() {
    const np::Digest vs = digest(10), ps = digest(20), ms = digest(30), gs = digest(40), cs = digest(50), ds = digest(60);
    np::HashMap map;
    EXPECT(map.parse(std::string(np::HashMap::kHeader) + "\n" + map_line(vs, "vs", "v") + map_line(ps, "ps", "p") +
                     map_line(ms, "ms", "m") + map_line(gs, "gs", "g") + map_line(cs, "cs", "c") +
                     map_line(ds, "ds", "d")));

    {
        Fixture f;
        f.stage(0, vs);
        f.stage(1, ps);
        const auto p = np::plan(f.desc, map);
        EXPECT(p.status == np::PlanStatus::ok && !p.mesh && p.stages.size() == 2);
        EXPECT(p.stage("vs") && p.stage("vs")->path == "v" && p.stage("ps")->path == "p");
        EXPECT(p.state.elements.size() == 3);
    }
    {
        // The stage comes from the container, not from the block index.
        Fixture f;
        f.stage(1, vs);
        f.stage(6, ps);
        const auto p = np::plan(f.desc, map);
        EXPECT(p.status == np::PlanStatus::ok && p.stages[0].block == 1 && p.stages[1].block == 6);
    }
    {
        Fixture f;
        f.stage(5, ps);
        f.stage(6, ms);
        const auto p = np::plan(f.desc, map);
        EXPECT(p.status == np::PlanStatus::ok && p.mesh);
    }
    {
        Fixture f;
        f.stage(0, vs);
        f.stage(4, gs);
        f.stage(1, ps);
        EXPECT(np::plan(f.desc, map).status == np::PlanStatus::ok);
    }
    {
        Fixture f;
        f.stage(0, vs);
        f.stage(1, digest(99));
        const auto p = np::plan(f.desc, map);
        EXPECT(p.status == np::PlanStatus::missing && p.failed_block == 1);
        EXPECT(np::plan(f.desc, np::HashMap{}).status == np::PlanStatus::missing);
    }
    {
        // A decoy 0x43 word whose following bytes name another container.
        Fixture f;
        f.stage(0, vs);
        f.rec[0] = records(vs);
        const auto r = records(ps);
        f.rec[0].insert(f.rec[0].end(), r.begin(), r.end());
        f.desc.stages[0].records = {f.rec[0].data(), f.rec[0].size()};
        EXPECT(np::plan(f.desc, map).status == np::PlanStatus::ambiguous);
    }
    for (const np::Digest& bad : {cs, ds}) {
        Fixture f;
        f.stage(0, vs);
        f.stage(1, bad);
        EXPECT(np::plan(f.desc, map).status == np::PlanStatus::unsupported);
    }
    {
        Fixture f;
        f.stage(0, vs);
        f.stage(1, vs);
        EXPECT(np::plan(f.desc, map).status == np::PlanStatus::unsupported);
    }
    {
        Fixture f;
        f.stage(0, vs);
        f.stage(1, ms);
        EXPECT(np::plan(f.desc, map).status == np::PlanStatus::unsupported);
    }
    {
        Fixture f;
        f.stage(1, ps);
        EXPECT(np::plan(f.desc, map).status == np::PlanStatus::unsupported);
    }
    {
        Fixture f;
        f.stage(0, ms);
        f.stage(1, gs);
        EXPECT(np::plan(f.desc, map).status == np::PlanStatus::unsupported);
    }
    {
        Fixture f;
        f.stage(0, vs);
        f.desc.stages[0].records = {};
        EXPECT(np::plan(f.desc, map).status == np::PlanStatus::missing);
        f.desc.stages[0].records.size = 0;  // a table entry of size 0 with a pointer, as the title passes
        f.desc.stages[0].records.data = f.rec[0].data();
        EXPECT(np::plan(f.desc, map).status == np::PlanStatus::missing);
    }
    {
        Fixture f;
        f.stage(0, vs);
        f.desc.state_b = {};
        const auto p = np::plan(f.desc, map);
        EXPECT(p.status == np::PlanStatus::malformed && std::string(p.reason) == "blob B pair absent or malformed");
    }
    {
        Fixture f;
        f.stage(0, vs);
        f.desc.state_a.size = 0;  // size 0 is absent whatever the pointer
        EXPECT(np::plan(f.desc, map).status == np::PlanStatus::ok);
        f.desc.state_a = {nullptr, 384};
        const auto p = np::plan(f.desc, map);
        EXPECT(p.status == np::PlanStatus::malformed && std::string(p.reason) == "blob A pair malformed");
    }
    {
        Fixture f;
        f.stage(0, vs);
        f.desc.stages[0].code.size = np::kPairLimit + 1;
        EXPECT(np::plan(f.desc, map).status == np::PlanStatus::malformed);
    }
    {
        Fixture f;
        f.stage(0, vs);
        f.desc.state_a = {};  // blob A is optional: not decoded
        EXPECT(np::plan(f.desc, map).status == np::PlanStatus::ok);
    }
}

}  // namespace

int main() {
    test_state_b();
    test_digests_and_map();
    test_plan();
    if (g_failures) {
        std::fprintf(stderr, "native_pso_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("native_pso_test: ok\n");
    return 0;
}
