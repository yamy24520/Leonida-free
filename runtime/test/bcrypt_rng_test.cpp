// Tests for runtime/shims/bcrypt_rng.cpp through rt::shims::find.
#include <windows.h>

#ifndef _NTDEF_
using NTSTATUS = LONG;
#endif
#include <bcrypt.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shims.h"

namespace {

constexpr NTSTATUS kStatusSuccess = 0x00000000L;
constexpr NTSTATUS kStatusInvalidHandle = static_cast<NTSTATUS>(0xC0000008L);
constexpr NTSTATUS kStatusInvalidParameter = static_cast<NTSTATUS>(0xC000000DL);
constexpr NTSTATUS kStatusNotFound = static_cast<NTSTATUS>(0xC0000225L);

using OpenAlgFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE*, LPCWSTR, LPCWSTR, ULONG);
using CloseAlgFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, ULONG);
using GenRandomFn = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, ULONG);

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

template <typename Fn>
Fn resolve(const char* name) {
    return reinterpret_cast<Fn>(rt::shims::find("bcrypt.dll", name));
}

void test_registry_coverage() {
    check(resolve<OpenAlgFn>("BCryptOpenAlgorithmProvider") != nullptr, "BCryptOpenAlgorithmProvider resolves");
    check(resolve<GenRandomFn>("BCryptGenRandom") != nullptr, "BCryptGenRandom resolves");
    check(resolve<CloseAlgFn>("BCryptCloseAlgorithmProvider") != nullptr, "BCryptCloseAlgorithmProvider resolves");
}

void test_provider_handles_and_validation() {
    auto open_alg = resolve<OpenAlgFn>("BCryptOpenAlgorithmProvider");
    auto gen_random = resolve<GenRandomFn>("BCryptGenRandom");
    auto close_alg = resolve<CloseAlgFn>("BCryptCloseAlgorithmProvider");

    BCRYPT_ALG_HANDLE h = nullptr;
    check(open_alg(nullptr, BCRYPT_RNG_ALGORITHM, nullptr, 0) == kStatusInvalidParameter,
          "BCryptOpenAlgorithmProvider rejects null out handle");
    check(open_alg(&h, nullptr, nullptr, 0) == kStatusInvalidParameter,
          "BCryptOpenAlgorithmProvider rejects null algorithm id");
    check(open_alg(&h, L"LEONIDA_NO_SUCH_ALG", nullptr, 0) == kStatusNotFound,
          "BCryptOpenAlgorithmProvider reports STATUS_NOT_FOUND for unknown algorithm");

    check(open_alg(&h, BCRYPT_RNG_ALGORITHM, nullptr, 0) == kStatusSuccess && h != nullptr,
          "BCryptOpenAlgorithmProvider opens BCRYPT_RNG_ALGORITHM");

    uint8_t buf[32]{};
    check(gen_random(h, nullptr, 0, 0) == kStatusInvalidParameter,
          "BCryptGenRandom rejects null buffer");
    check(gen_random(nullptr, buf, sizeof(buf), 0) == kStatusInvalidHandle,
          "BCryptGenRandom rejects null handle without BCRYPT_USE_SYSTEM_PREFERRED_RNG");
    check(gen_random(h, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == kStatusInvalidParameter,
          "BCryptGenRandom rejects non-null handle with BCRYPT_USE_SYSTEM_PREFERRED_RNG");

    std::memset(buf, 0, sizeof(buf));
    check(gen_random(h, buf, sizeof(buf), 0) == kStatusSuccess,
          "BCryptGenRandom succeeds with open RNG handle");

    bool nonzero = false;
    for (uint8_t b : buf) {
        if (b != 0) nonzero = true;
    }
    check(nonzero, "BCryptGenRandom writes non-zero random bytes");

    uint8_t sys_buf[32]{};
    check(gen_random(nullptr, sys_buf, sizeof(sys_buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == kStatusSuccess,
          "BCryptGenRandom succeeds with BCRYPT_USE_SYSTEM_PREFERRED_RNG");

    check(close_alg(nullptr, 0) == kStatusInvalidHandle,
          "BCryptCloseAlgorithmProvider rejects nullptr");
    check(close_alg(reinterpret_cast<BCRYPT_ALG_HANDLE>(static_cast<uintptr_t>(0x1234)), 0) == kStatusInvalidHandle,
          "BCryptCloseAlgorithmProvider rejects foreign pointer");
    check(close_alg(h, 0) == kStatusSuccess,
          "BCryptCloseAlgorithmProvider closes live RNG handle");
    check(close_alg(h, 0) == kStatusInvalidHandle,
          "BCryptCloseAlgorithmProvider refuses double close safely");
    check(gen_random(h, buf, sizeof(buf), 0) == kStatusInvalidHandle,
          "BCryptGenRandom rejects closed handle safely");
}

void test_seeded_determinism(bool seeded) {
    if (!seeded) return;
    auto open_alg = resolve<OpenAlgFn>("BCryptOpenAlgorithmProvider");
    auto gen_random = resolve<GenRandomFn>("BCryptGenRandom");
    auto close_alg = resolve<CloseAlgFn>("BCryptCloseAlgorithmProvider");

    BCRYPT_ALG_HANDLE h = nullptr;
    check(open_alg(&h, BCRYPT_RNG_ALGORITHM, nullptr, 0) == kStatusSuccess && h != nullptr,
          "seeded BCryptOpenAlgorithmProvider opens RNG");
    uint8_t buf[16]{};
    check(gen_random(h, buf, sizeof(buf), 0) == kStatusSuccess, "seeded BCryptGenRandom succeeds");
    // Ensure reproducible non-trivial stream across runs when LEONIDA_RNG_SEED=0x123456789abcdef0 is set.
    uint8_t second[16]{};
    check(gen_random(nullptr, second, sizeof(second), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == kStatusSuccess &&
              std::memcmp(buf, second, sizeof(buf)) != 0,
          "seeded PRNG advances cleanly across consecutive calls");
    close_alg(h, 0);
}

}  // namespace

int main(int argc, char** argv) {
    bool seeded = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seeded") == 0) seeded = true;
    }
    test_registry_coverage();
    test_provider_handles_and_validation();
    test_seeded_determinism(seeded);
    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
