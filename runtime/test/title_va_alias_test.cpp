// leonida::TitleVaAliases (src/title_va_alias.h): where an address inside a title-placed buffer (Xbox device slot
// 133) is translated to, and that everything else is left alone. No GPU.
#include "../src/title_va_alias.h"

#include <cstdint>
#include <cstdio>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

int main() {
    leonida::TitleVaAliases a;
    constexpr std::uint64_t title = 0x20000000000ull;  // a synthetic user-mode VA far above 4 GiB
    constexpr std::uint64_t size = 0x10000;
    constexpr std::uint64_t arena = 0x2340000;
    // Empty: identity everywhere.
    EXPECT(a.size() == 0 && a.shader_va(title) == title && a.shader_va(0) == 0);
    EXPECT(!a.add(title, 0, arena));                  // empty range
    EXPECT(!a.add(UINT64_MAX - 4, 16, arena));        // wraps
    EXPECT(a.add(title, size, arena) && a.size() == 1);
    // Inside: the same offset in the copy, first and last byte included.
    EXPECT(a.shader_va(title) == arena);
    EXPECT(a.shader_va(title + 0x123) == arena + 0x123);
    EXPECT(a.shader_va(title + size - 1) == arena + size - 1);
    // Negative controls: one byte before, the end itself, and an arena VA are untouched.
    EXPECT(a.shader_va(title - 1) == title - 1);
    EXPECT(a.shader_va(title + size) == title + size);
    EXPECT(a.shader_va(arena) == arena);
    // The free span around an address: between the ranges, so a window made for it never covers one.
    std::uint64_t lo = 1, hi = 1;
    a.gap(title + size + 5, lo, hi);
    EXPECT(lo == title + size && hi == UINT64_MAX);
    a.gap(title - 5, lo, hi);
    EXPECT(lo == 0 && hi == title);
    // Without a device's resolver, resolve only translates what was added, and counts it.
    EXPECT(a.resolve(title + 4, 4, leonida::TitleVaAliases::root_view) == arena + 4);
    EXPECT(a.resolve(title - 4, 4, leonida::TitleVaAliases::root_view) == title - 4);
    EXPECT(a.translated(leonida::TitleVaAliases::root_view) == 1);
    a.note(title + 8, leonida::TitleVaAliases::vertex_view);
    a.note(title - 8, leonida::TitleVaAliases::vertex_view);
    EXPECT(a.untranslated(leonida::TitleVaAliases::vertex_view) == 1);
    // The title's memory: a commit inside a reservation is one range; outside every range is not the title's.
    std::uint64_t mlo = 0, mhi = 0;
    a.add_title_memory(0x40000000, 0x100000);
    a.add_title_memory(0x40010000, 0x1000);
    EXPECT(a.title_memory(0x40080000, mlo, mhi) && mlo == 0x40000000 && mhi == 0x40100000);
    EXPECT(!a.title_memory(0x40100000, mlo, mhi) && !a.title_memory(0x3FFFFFFF, mlo, mhi));
    a.remove_title_memory(0x40000000);
    EXPECT(!a.title_memory(0x40080000, mlo, mhi));
    // A wrapping target (an import below heap_base) is kept as is: modular arithmetic.
    constexpr std::uint64_t below = 0xFFFFFFFFFFF00000ull;
    EXPECT(a.add(title + 0x100000, size, below) && a.shader_va(title + 0x100010) == below + 0x10);
    // A new buffer over part of a live range replaces it; the old range's removal does not touch the new one.
    EXPECT(a.add(title + 0x8000, size, 0x5000000));
    EXPECT(a.shader_va(title) == title);  // the replaced range is gone entirely
    EXPECT(a.shader_va(title + 0x8000) == 0x5000000);
    a.remove(title, arena);  // stale: nothing starts there any more
    EXPECT(a.shader_va(title + 0x8000) == 0x5000000);
    a.remove(title + 0x8000, 0x1234);  // another target: kept
    EXPECT(a.shader_va(title + 0x8000) == 0x5000000);
    a.remove(title + 0x8000, 0x5000000);
    EXPECT(a.shader_va(title + 0x8000) == title + 0x8000);
    a.remove(title + 0x100000, below);
    EXPECT(a.size() == 0 && a.shader_va(title + 0x100010) == title + 0x100010);
    std::printf("title_va_alias: translation inside a title-placed range, identity outside it and after release\n");
    return 0;
}
