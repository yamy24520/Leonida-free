// Host-only stalls and the guest Sleep (src/host_stall.h, shims/host_stall.cpp).
//
// Part 1 checks the ledger with times passed in, so it is exact. Part 2 runs the Sleep the registry serves on real
// threads: a Sleep that no stall overlaps returns as the host's does, a Sleep that a stall overlaps cannot return
// while the stall is open, and a loop built like a title's hang check (count Sleep(1000) rounds without progress,
// start again after a late wake-up) survives a stall longer than its limit and still fires without one.
// Synthetic threads only: no game files, no GPU.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "../src/host_stall.h"
#include "shims.h"

namespace {

using rt::host_stall::Ledger;
using rt::host_stall::Nanoseconds;
using std::chrono::milliseconds;

int failures = 0;
int checks = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

Nanoseconds ms(long long n) { return std::chrono::duration_cast<Nanoseconds>(milliseconds(n)); }

long long elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - start).count();
}

using SleepFn = void(WINAPI*)(DWORD);

SleepFn served_sleep(const char* dll) { return reinterpret_cast<SleepFn>(rt::shims::find(dll, "Sleep")); }

void ledger_checks() {
    Ledger l;
    check(!l.open() && l.stalled(ms(5)) == ms(0), "an empty ledger has no stall");
    l.begin(ms(10));
    check(l.open() && l.stalled(ms(10)) == ms(0) && l.stalled(ms(25)) == ms(15), "an open stall counts up to now");
    l.end(ms(40));
    check(!l.open() && l.stalled(ms(40)) == ms(30) && l.stalled(ms(900)) == ms(30), "a closed stall stops counting");

    // Nested and overlapping stalls count the covered time once.
    l.begin(ms(100));
    l.begin(ms(110));
    l.end(ms(120));
    check(l.open() && l.stalled(ms(130)) == ms(60), "the outer stall stays open when the inner one ends");
    l.end(ms(150));
    check(!l.open() && l.stalled(ms(150)) == ms(80), "two overlapping stalls count 50 ms, not 60");

    // An end without a begin changes nothing, and does not make a later begin close early.
    l.end(ms(160));
    check(!l.open() && l.stalled(ms(170)) == ms(80), "an unmatched end is ignored");
    l.begin(ms(200));
    check(l.open() && l.stalled(ms(230)) == ms(110), "a begin after an unmatched end opens a stall");
    l.end(ms(230));

    // A sleep of 1000 ms that started at 1000 with 110 ms on the ledger and woke at 2000.
    Ledger s = l;
    check(s.wake_time(ms(2000), ms(110), ms(2000)) == ms(2000), "no stall during the sleep: it returns when it woke");
    s.begin(ms(1500));
    check(s.wake_time(ms(2000), ms(110), ms(2000)) == ms(2500), "500 ms of an open stall so far are owed");
    s.end(ms(4000));
    check(s.wake_time(ms(2000), ms(110), ms(4000)) == ms(4500), "a 2500 ms stall moves the wake-up by 2500 ms");
    check(s.wake_time(ms(2000), ms(110), ms(9000)) == ms(4500), "and no further once it has closed");
}

void sleep_binding_checks() {
    const SleepFn api_set = served_sleep("api-ms-win-core-synch-l1-2-0.dll");
    check(api_set != nullptr, "Sleep is served under api-ms-win-core-synch-l1-2-0.dll");
    check(served_sleep("kernel32.dll") == api_set && served_sleep("kernelbase.dll") == api_set &&
              served_sleep("api-ms-win-core-synch-l1-2-1.dll") == api_set,
          "kernel32, kernelbase and the later API set serve the same Sleep");
    check(rt::shims::find("api-ms-win-core-synch-l1-2-0.dll", "SleepEx") == nullptr &&
              rt::shims::find("kernel32.dll", "SleepConditionVariableSRW") == nullptr,
          "SleepEx and the condition-variable sleeps stay the host's");
    check(!rt::shims::owns_dll("kernel32.dll") && !rt::shims::owns_dll("api-ms-win-core-synch-l1-2-0.dll"),
          "serving Sleep does not make the shims own kernel32 or the API set");
}

void sleep_without_a_stall(SleepFn guest_sleep) {
    const auto start = std::chrono::steady_clock::now();
    guest_sleep(50);
    const long long took = elapsed_ms(start);
    check(took >= 45 && took < 1000, "a Sleep(50) no stall overlaps takes about 50 ms");
    const auto yield_start = std::chrono::steady_clock::now();
    guest_sleep(0);
    check(elapsed_ms(yield_start) < 1000, "Sleep(0) yields and returns");
}

void sleep_held_by_a_stall(SleepFn guest_sleep) {
    auto& gate = rt::host_stall::gate();
    std::atomic<bool> returned{false};
    gate.begin();
    const auto start = std::chrono::steady_clock::now();
    long long sleeper_ms = 0;
    std::thread sleeper([&] {
        guest_sleep(20);
        sleeper_ms = elapsed_ms(start);
        returned = true;
    });
    // Far longer than the Sleep(20): it can only still be asleep because the stall holds it.
    std::this_thread::sleep_for(milliseconds(400));
    check(!returned.load(), "a Sleep(20) does not return while a stall is open");
    {
        const rt::host_stall::Scope nested;  // a second, nested stall ends first and releases nothing
    }
    std::this_thread::sleep_for(milliseconds(100));
    check(!returned.load(), "a nested stall that ends leaves the Sleep held");
    gate.end();
    sleeper.join();
    check(returned.load() && sleeper_ms >= 500, "the Sleep returns once the stall is over, not before");

    // A Sleep that starts after the stall is over owes nothing.
    const auto after = std::chrono::steady_clock::now();
    guest_sleep(30);
    check(elapsed_ms(after) < 1000, "a later Sleep is not held by a closed stall");
}

// A stand-in for the kind of check the retail title runs: every round sleeps `round_ms`; a round that returns more
// than `late_ms` late starts the count again; `limit` rounds without progress fire. Returns whether it fired.
bool hang_check_fires(SleepFn guest_sleep, const std::atomic<bool>& done, DWORD round_ms, long long late_ms, int limit) {
    int left = limit;
    while (!done.load()) {
        const auto before = std::chrono::steady_clock::now();
        guest_sleep(round_ms);
        if (elapsed_ms(before) > late_ms) {
            left = limit;
            continue;
        }
        if (--left == 0) return true;
    }
    return false;
}

void hang_check_stand_in(SleepFn guest_sleep) {
    // 5 rounds of 40 ms: the limit is 200 ms without progress. The "compile" takes 1000 ms, five times the limit.
    constexpr DWORD kRound = 40;
    constexpr long long kLate = 150;
    constexpr int kLimit = 5;

    {
        std::atomic<bool> done{false};
        std::atomic<bool> fired{false};
        std::thread checker;
        // Twenty stalls of 50 ms back to back, like one pipeline after another: none is long by itself. The checker
        // starts inside the first and is told to stop inside the last, so no round runs outside a stall.
        for (int i = 0; i < 20; ++i) {
            const rt::host_stall::Scope compiling;
            if (i == 0) checker = std::thread([&] { fired = hang_check_fires(guest_sleep, done, kRound, kLate, kLimit); });
            std::this_thread::sleep_for(milliseconds(50));
            if (i == 19) done = true;
        }
        checker.join();
        check(!fired.load(), "the stand-in hang check survives 1000 ms of back-to-back stalls (limit 200 ms)");
    }
    {
        // The same wait without a stall: the check must still fire, so nothing was switched off.
        std::atomic<bool> done{false};
        std::atomic<bool> fired{false};
        std::thread checker([&] { fired = hang_check_fires(guest_sleep, done, kRound, kLate, kLimit); });
        for (int i = 0; i < 100 && !fired.load(); ++i) std::this_thread::sleep_for(milliseconds(50));
        done = true;
        checker.join();
        check(fired.load(), "without a stall the stand-in hang check fires");
    }
}

}  // namespace

int main() {
    ledger_checks();
    sleep_binding_checks();
    if (const SleepFn guest_sleep = served_sleep("api-ms-win-core-synch-l1-2-0.dll")) {
        sleep_without_a_stall(guest_sleep);
        sleep_held_by_a_stall(guest_sleep);
        hang_check_stand_in(guest_sleep);
    }
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
