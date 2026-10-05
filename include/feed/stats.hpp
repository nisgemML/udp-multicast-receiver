#pragma once
// stats.hpp — Per-packet latency histogram and throughput tracker.
//
// Two latency recorders, for two different jobs:
//
// LatencyHistogram — 64-bucket log2 histogram. O(1), allocation-free, safe to
//   call on the hot path of a live receiver. The price is resolution: a sample
//   is only known to lie in [2^i, 2^(i+1)). percentile() therefore returns the
//   bucket's EXCLUSIVE UPPER BOUND, so "p99 < 1024 ns" is a true statement
//   about the data. (An earlier version returned the lower bound 2^i, which
//   understated every percentile by up to 2x and could print p99.9 below the
//   mean.) Coarser than HDR Histogram, which subdivides each power of two.
//
// SampleRecorder — stores every sample in a pre-reserved vector and sorts on
//   report. Exact percentiles, O(n log n) query. Used by the offline
//   benchmarks (tools/), where allocation-free recording does not matter and
//   exact numbers do.

#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <bit>
#include <algorithm>
#include <cmath>
#include <vector>

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
            if (cum > target) return (i >= 63) ? ~0ULL : (1ULL << (i + 1));
        }
        return ~0ULL;
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
                    "p50<%5lu ns  p99<%6lu ns  p99.9<%7lu ns  (log2 buckets)\n",
                    label, count(), mean_ns(),
                    percentile(0.50), percentile(0.99), percentile(0.999));
    }
};

// ── Exact-percentile recorder (offline benchmarks only) ─────────────────────

class SampleRecorder {
    std::vector<uint64_t> samples_;
public:
    explicit SampleRecorder(std::size_t reserve = 0) { samples_.reserve(reserve); }
    void record(uint64_t ns) { samples_.push_back(ns); }
    [[nodiscard]] std::size_t count() const noexcept { return samples_.size(); }

    struct Summary { std::size_t n; double mean; uint64_t min, p50, p90, p99, p999, max; };

    // Nearest-rank percentiles on a sorted copy.
    [[nodiscard]] Summary summarize() const {
        Summary r{samples_.size(), 0.0, 0, 0, 0, 0, 0, 0};
        if (samples_.empty()) return r;
        std::vector<uint64_t> v(samples_);
        std::sort(v.begin(), v.end());
        const auto at = [&](double p) {
            std::size_t k = static_cast<std::size_t>(std::ceil(p * double(v.size())));
            return v[k == 0 ? 0 : k - 1];
        };
        long double sum = 0;
        for (auto x : v) sum += x;
        r.mean = double(sum / v.size());
        r.min = v.front(); r.max = v.back();
        r.p50 = at(0.50); r.p90 = at(0.90); r.p99 = at(0.99); r.p999 = at(0.999);
        return r;
    }

    void print(const char* label = "") const {
        const auto s = summarize();
        std::printf("%s\n  n=%zu  mean=%.0f  min=%lu  p50=%lu  p90=%lu  p99=%lu  p99.9=%lu  max=%lu  (ns, exact)\n",
                    label, s.n, s.mean, s.min, s.p50, s.p90, s.p99, s.p999, s.max);
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
