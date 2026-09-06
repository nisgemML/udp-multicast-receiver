#pragma once
// decision_engine.hpp — closes the pipeline from decoded ITCH bytes to an
// emitted trading decision, with a latency measurement point at each end.
//
// ── What this is, and isn't ───────────────────────────────────────────────────
//
// This is NOT a matching engine or a trading strategy. It's a small,
// honestly-labeled stand-in that answers the otherwise-open question this
// repo used to leave hanging: "the receiver delivers gap-free ITCH
// bytes... then what?" DecisionEngine reacts to OrderBook top-of-book
// changes and applies one toy rule (quote one tick inside the market when
// the spread is wide enough), purely so the full pipeline — packet →
// GapBuffer → OrderBook → decision — can be wired together and measured
// end to end. A real system replaces this file's logic with an actual
// strategy or matching engine; nothing else in the pipeline changes.
//
// ── What "tick-to-trade" means here, precisely ────────────────────────────────
//
// The latency recorded by this class is:
//
//   decided_ns (CLOCK_MONOTONIC when the decision is emitted)
//   minus
//   recv_ns    (CLOCK_MONOTONIC when the triggering packet was handed to
//               GapBuffer::ingest(), threaded through unchanged via
//               MoldMessage::recv_ns)
//
// On the LIVE multicast path this is a real, meaningful number: recv_ns
// comes from SO_TIMESTAMPING (or a monotonic_ns() fallback) at actual
// packet arrival, so the delta is genuine software tick-to-trade latency
// on this machine.
//
// On PCAP REPLAY it is deliberately NOT computed from the timestamp stored
// in the pcap file — that timestamp is the original capture time, which
// has no relationship to "now" when the file is replayed later, and
// subtracting it from a live decided_ns would produce a meaningless
// number dressed up as a latency figure. tools/tick_to_trade_bench.cpp
// instead stamps each synthetic packet with the *current* monotonic clock
// immediately before feeding it to GapBuffer, so the measured delta is
// real: it is the actual time this process took to go from "bytes in
// hand" to "decision emitted" — the parse + book-update + decision-rule
// cost, on this CPU, right now. It explicitly does NOT include NIC/network
// wire transit time, since nothing here talks to a real NIC. Anywhere
// this number is quoted, it should be quoted as exactly that: a software
// pipeline latency, not a co-location tick-to-trade figure.

#include "feed/order_book.hpp"
#include "feed/stats.hpp"

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <string_view>

namespace feed {

struct Decision {
    std::string symbol;
    uint32_t    price;       // fixed-point * 10000, same convention as ITCH
    uint32_t    shares;
    char        side;        // 'B' or 'S' — side of the quote this decision emits
    uint64_t    recv_ns;      // arrival time of the packet that triggered this
    uint64_t    decided_ns;   // CLOCK_MONOTONIC when the decision was emitted
};

class DecisionEngine {
public:
    using OnDecision = std::function<void(const Decision&)>;

    struct Config {
        // Only quote if the current spread exceeds this many ticks — a
        // stand-in risk/edge filter, not a real signal.
        uint32_t min_spread_ticks = 2;
        uint32_t tick_size        = 100;   // 1 tick = $0.01 at ITCH's 10000x fixed point
        uint32_t quote_size       = 100;
        // A user-declared (if defaulted) constructor here is required, not
        // decorative: with a pure-aggregate Config, `Config cfg = {}` as a
        // default argument on DecisionEngine's own constructor fails to
        // compile under GCC 13 — the nested type's aggregate-init is
        // resolved before DecisionEngine is considered complete, inside its
        // own definition. GapBuffer::Config in this codebase sidesteps the
        // same issue with an explicit constructor for the same reason.
        Config() noexcept = default;
    };

    explicit DecisionEngine(Config cfg = {}) : cfg_(cfg) {}

    void set_on_decision(OnDecision cb) { on_decision_ = std::move(cb); }

    // Wire this directly as MultiSymbolBook::set_on_top_change's callback.
    void on_top_of_book(std::string_view symbol, const TopOfBook& top,
                         uint64_t recv_ns) noexcept {
        ++top_changes_seen_;
        if (!top.bid || !top.ask) return; // nothing to quote against yet

        const uint32_t spread = (top.ask->price > top.bid->price)
                                ? (top.ask->price - top.bid->price) : 0;
        if (spread <= cfg_.min_spread_ticks * cfg_.tick_size) return;

        Decision d;
        d.symbol     = std::string(symbol);
        d.price      = top.bid->price + cfg_.tick_size;
        d.shares     = cfg_.quote_size;
        d.side       = 'B';
        d.recv_ns    = recv_ns;
        d.decided_ns = monotonic_ns();

        // Guard against replay/test setups that don't stamp a monotonic
        // recv_ns (e.g. recv_ns == 0 or otherwise not comparable to
        // decided_ns) — record only when the delta is meaningful, so
        // latency() never silently accumulates garbage from callers that
        // didn't wire real timestamps through.
        if (d.decided_ns > d.recv_ns) {
            latency_.record(d.decided_ns - d.recv_ns);
        }

        ++decisions_emitted_;
        if (on_decision_) on_decision_(d);
    }

    [[nodiscard]] const LatencyHistogram& latency()          const noexcept { return latency_; }
    [[nodiscard]] uint64_t decisions_emitted()                const noexcept { return decisions_emitted_; }
    [[nodiscard]] uint64_t top_changes_seen()                 const noexcept { return top_changes_seen_; }

    static uint64_t monotonic_ns() noexcept {
        timespec ts;
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
    }

private:
    Config            cfg_;
    OnDecision        on_decision_;
    LatencyHistogram  latency_;
    uint64_t          decisions_emitted_ = 0;
    uint64_t          top_changes_seen_  = 0;
};

} // namespace feed
