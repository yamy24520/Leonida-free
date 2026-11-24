// Host-only stalls: time a title thread spends inside the layer on work the console never does, and what a guest
// Sleep does about it.
//
// The console creates a pipeline from native code in well under a millisecond. Here the same call compiles
// translated SPIR-V in the GPU driver, which takes 25 to 60 s for the 52 programs of XboxBvhBuilder.dll on a cold
// driver cache (docs/retail_boot_chain.md, "The startup hang check"). The retail title runs a hang check next to it:
// a thread that loops on Sleep(1000) and ends the process after 30 loops in which the main thread reported no
// progress. The same loop restarts its count when one Sleep returns more than 2 s late by QueryPerformanceCounter,
// which is what a suspended console looks like to it.
//
// So the layer brackets such work with a Scope, and the Sleep shim (shims/host_stall.cpp) keeps a guest Sleep from
// returning while a stall is open, and for as long afterwards as stalls overlapped it: a guest Sleep(n) lasts n
// milliseconds of the time in which no stall was open. A Sleep that no stall overlaps is the host's Sleep, unchanged.
// Nothing is skipped and no check of the title is switched off; the title sees a late wake-up and applies its own
// rule. QueryPerformanceCounter and the other waits stay the host's.
//
// Header-only and free of Windows headers, so the layer, the shim and the unit test share one definition.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace rt::host_stall {

using Nanoseconds = std::chrono::nanoseconds;

// The accounting, with the time passed in: how long at least one stall has been open, up to `now`. Stalls nest and
// overlap (two threads compiling at once count once). `now` never goes backwards between calls.
class Ledger {
public:
    void begin(Nanoseconds now) {
        if (open_++ == 0) opened_at_ = now;
    }
    void end(Nanoseconds now) {
        if (open_ == 0) return;  // an end without a begin changes nothing
        if (--open_ == 0) closed_ += now - opened_at_;
    }
    bool open() const { return open_ != 0; }
    Nanoseconds stalled(Nanoseconds now) const { return open_ ? closed_ + (now - opened_at_) : closed_; }

    // When a sleep that the host finished at `woke`, and that started with stalled() == `stalled_at_start`, may
    // return: `woke` plus the stall time that fell into it. Later than `now` while more is owed.
    Nanoseconds wake_time(Nanoseconds woke, Nanoseconds stalled_at_start, Nanoseconds now) const {
        return woke + (stalled(now) - stalled_at_start);
    }

private:
    std::uint32_t open_ = 0;
    Nanoseconds opened_at_{0};
    Nanoseconds closed_{0};
};

// The process-wide ledger behind a lock, on the steady clock.
class Gate {
public:
    void begin() {
        std::lock_guard<std::mutex> lock(mutex_);
        ledger_.begin(now());
    }
    void end() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ledger_.end(now());
        }
        changed_.notify_all();
    }

    // Read before a sleep starts; passed to hold() when the host's sleep is over.
    Nanoseconds mark() {
        std::lock_guard<std::mutex> lock(mutex_);
        return ledger_.stalled(now());
    }

    // Called when the host's sleep has returned. Blocks while a stall is open and until the stall time that fell
    // into the sleep has passed again; returns how long it blocked (zero when no stall overlapped the sleep).
    Nanoseconds hold(Nanoseconds mark_before) {
        std::unique_lock<std::mutex> lock(mutex_);
        const Nanoseconds woke = now();
        for (;;) {
            if (ledger_.open()) {
                changed_.wait(lock, [this] { return !ledger_.open(); });
                continue;
            }
            const Nanoseconds current = now();
            const Nanoseconds wake = ledger_.wake_time(woke, mark_before, current);
            if (current >= wake) return current - woke;
            changed_.wait_for(lock, wake - current);  // a stall that opens meanwhile is seen by the next round
        }
    }

    static Nanoseconds now() {
        return std::chrono::duration_cast<Nanoseconds>(std::chrono::steady_clock::now().time_since_epoch());
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    Ledger ledger_;
};

inline Gate& gate() {
    static Gate instance;
    return instance;
}

// Brackets host-only work on behalf of a title call that returns at once on the console.
class Scope {
public:
    Scope() { gate().begin(); }
    ~Scope() { gate().end(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}  // namespace rt::host_stall
