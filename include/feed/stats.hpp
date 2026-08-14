#pragma once
// stats.hpp — Per-packet latency histogram and throughput tracker.
//
// Latency is measured as: kernel receive timestamp (SO_TIMESTAMPING) minus
// the ITCH timestamp embedded in the message body.  This measures:
//
//   (time packet entered socket recv queue) - (time the exchange sent it)
//
// In co-location this is predominantly NIC-to-NIC network latency (~2-4 µs
// for NASDAQ's matching engine to NY4).  On a VM or remote machine it includes
// OS scheduling jitter and is much higher.
//
// We use a 64-bucket log₂ histogram for O(1) update and O(64) query.
// This is the same approach as HDR Histogram but simplified for <1ms ranges.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <bit>

namespace feed {

class LatencyHistogram {
    static constexpr int kBuckets = 64;
    std::atomic<uint64_t> counts_[kBuckets]{};
    std::atomic<uint64_t> total_{0};
    std::atomic<uint64_t> sum_{0};

public:
    // Record a latency in nanoseconds.
    void record(uint64_t ns) noexcept {
        const int b = (ns == 0) ? 0 : std::min(63 - __builtin_clzll(ns), kBuckets-1);
        counts_[b].fetch_add(1, std::memory_order_relaxed);
        total_.fetch_add(1,  std::memory_order_relaxed);
        sum_.fetch_add(ns,   std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t percentile(double p) const noexcept {
        const uint64_t n = total_.load(std::memory_order_relaxed);
        if (!n) return 0;
        const uint64_t target = static_cast<uint64_t>(p * double(n));
        uint64_t cum = 0;
        for (int i = 0; i < kBuckets; ++i) {
            cum += counts_[i].load(std::memory_order_relaxed);
            if (cum > target) return i == 0 ? 0ULL : 1ULL << i;
        }
        return 1ULL << (kBuckets - 1);
    }

    [[nodiscard]] double mean_ns() const noexcept {
        const uint64_t n = total_.load(std::memory_order_relaxed);
        if (!n) return 0.0;
        return double(sum_.load(std::memory_order_relaxed)) / double(n);
    }

    [[nodiscard]] uint64_t count() const noexcept {
        return total_.load(std::memory_order_relaxed);
    }

    void print(const char* label = "") const noexcept {
        std::printf("%-20s  count=%7lu  mean=%6.0f ns  "
                    "p50=%5lu ns  p99=%6lu ns  p99.9=%7lu ns\n",
                    label, count(), mean_ns(),
                    percentile(0.50), percentile(0.99), percentile(0.999));
    }
};

// ── Throughput tracker (messages per second, sliding window) ──────────────────

class ThroughputTracker {
    static constexpr int kWindowSec = 5;
    static constexpr int kBuckets   = kWindowSec * 10;  // 100ms granularity

    struct Bucket { uint64_t ts_100ms = 0; uint64_t count = 0; };
    std::array<Bucket, kBuckets> ring_{};
    std::atomic<uint64_t> total_{0};

public:
    void record(uint64_t now_ns, uint64_t count = 1) noexcept {
        const uint64_t slot = (now_ns / 100'000'000ULL) % kBuckets;
        auto& b = ring_[slot];
        const uint64_t cur_window = now_ns / 100'000'000ULL;
        if (b.ts_100ms != cur_window) { b.ts_100ms = cur_window; b.count = 0; }
        b.count += count;
        total_.fetch_add(count, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t msgs_per_sec(uint64_t now_ns) const noexcept {
        const uint64_t cur = now_ns / 100'000'000ULL;
        uint64_t sum = 0;
        for (auto& b : ring_)
            if (b.ts_100ms + kBuckets > cur) sum += b.count;
        return sum / kWindowSec;
    }

    [[nodiscard]] uint64_t total() const noexcept {
        return total_.load(std::memory_order_relaxed);
    }
};

} // namespace feed
