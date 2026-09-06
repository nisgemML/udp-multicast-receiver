#pragma once
// feed_arbitrator.hpp — A/B (primary/secondary) feed line arbitration.
//
// ── Why exchanges publish two lines ───────────────────────────────────────────
//
// NASDAQ ITCH (and most major venues) publish the *same* sequenced stream
// over two independent multicast lines — conventionally "A" and "B" —
// fed from redundant infrastructure and, in co-location, often routed over
// physically separate switches. Both lines carry identical sequence
// numbers for identical messages. The two lines are not a primary/failover
// pair where B only matters if A is down; a well-behaved consumer merges
// both continuously, because switch-level packet loss on one line is
// common and largely uncorrelated with loss on the other.
//
// A single-line receiver (GapBuffer + MulticastReceiver alone) treats every
// line-A drop as a gap: it stalls, waits `gap_timeout_ns`, and issues a
// retransmit request over TCP — a real, measurable latency hit even when
// line B had the missing packet the entire time. Arbitrating both lines
// against the same sequence space eliminates that stall in the common case:
// a drop on one line is invisible as long as the other line delivered the
// packet, and a retransmit is only needed when a sequence is missing on
// BOTH lines past the timeout.
//
// ── Design: arbitrate for free by sharing one GapBuffer ───────────────────────
//
// GapBuffer already does exactly the dedup/ordering work arbitration needs:
// seq < next_expected is dropped as a duplicate, seq == next_expected is
// delivered immediately, seq > next_expected is buffered pending the gap.
// Feeding both lines' packets into the *same* GapBuffer instance means:
//   • Whichever line's copy of sequence N arrives first is delivered.
//   • The other line's copy of the same sequence N is automatically a
//     duplicate — dropped by existing logic, no new code needed.
//   • A gap only fires (and a retransmit only fires) once neither line has
//     delivered the missing sequence within the timeout.
//
// FeedArbitrator is therefore a thin instrumentation layer, not a second
// state machine: it forwards each line's packets to a shared GapBuffer and
// uses GapBuffer::ingest()'s own delivered-message return value to
// attribute which line supplied the call that delivered — purely for
// observability (which line is healthier right now, how often each line
// "wins" a race). It does not duplicate GapBuffer's correctness logic, so
// there is nothing here that can disagree with it.
//
// ── What this does not do ─────────────────────────────────────────────────────
//
// It does not open two live sockets itself — pair it with two
// MulticastReceiver instances (or one on multicast group A, one on B, both
// constructed over the same GapBuffer&) or two PcapReader replays for
// offline testing. It does not implement a "prefer A unless A is stale"
// policy — plain first-valid-wins is what real A/B consumers use, because
// per-sequence latency, not line identity, decides which copy is faster on
// any given packet.

#include "feed/gap_buffer.hpp"
#include "feed/wire_format.hpp"

#include <cstdint>
#include <array>

namespace feed {

enum class FeedLine : uint8_t { A = 0, B = 1 };

class FeedArbitrator {
public:
    explicit FeedArbitrator(GapBuffer& shared_gap_buf) : gap_buf_(shared_gap_buf) {}

    // Feed a MoldUDP64 packet received on line A or line B. Semantics are
    // identical to GapBuffer::ingest — same return value, same effect on
    // next_expected() — this only adds attribution bookkeeping around it.
    int ingest_a(const uint8_t* payload, std::size_t len, uint64_t now_ns) noexcept {
        return ingest_line(FeedLine::A, payload, len, now_ns);
    }
    int ingest_b(const uint8_t* payload, std::size_t len, uint64_t now_ns) noexcept {
        return ingest_line(FeedLine::B, payload, len, now_ns);
    }

    // Periodic tick — forwards to the shared GapBuffer. Call this even if
    // neither line has delivered a packet recently, so a gap missing on
    // both lines still times out and triggers a retransmit request.
    void tick(uint64_t now_ns) noexcept { gap_buf_.tick(now_ns); }

    // ── Per-line observability ────────────────────────────────────────────────

    [[nodiscard]] uint64_t stat_packets(FeedLine l)   const noexcept { return stat_packets_[idx(l)]; }
    [[nodiscard]] uint64_t stat_seq_won(FeedLine l)    const noexcept { return stat_seq_won_[idx(l)]; }
    [[nodiscard]] uint64_t stat_redundant(FeedLine l)  const noexcept { return stat_redundant_[idx(l)]; }

    // Fraction of in-order sequence advances attributed to line A, in
    // [0, 1]. Useful as a running health signal: if this collapses to 0 or
    // 1, one line has effectively gone dark and you're running on a single
    // line's worth of redundancy.
    [[nodiscard]] double a_win_share() const noexcept {
        const uint64_t total = stat_seq_won_[idx(FeedLine::A)] + stat_seq_won_[idx(FeedLine::B)];
        if (total == 0) return 0.0;
        return double(stat_seq_won_[idx(FeedLine::A)]) / double(total);
    }

    // True if this line has not supplied the winning copy of any sequence
    // for at least `stale_after_ns`, while the other line has been active —
    // i.e. this line looks dark. `now_ns` should be CLOCK_MONOTONIC.
    [[nodiscard]] bool line_looks_dark(FeedLine l, uint64_t now_ns,
                                        uint64_t stale_after_ns = 5'000'000) const noexcept {
        const auto i = idx(l);
        if (stat_packets_[i] == 0) return false; // never connected != dark
        if (last_win_ns_[i] == 0) return now_ns > stale_after_ns;
        return (now_ns - last_win_ns_[i]) > stale_after_ns;
    }

    [[nodiscard]] const GapBuffer& gap_buffer() const noexcept { return gap_buf_; }

private:
    static constexpr std::size_t idx(FeedLine l) noexcept { return std::size_t(l); }

    int ingest_line(FeedLine line, const uint8_t* payload, std::size_t len,
                     uint64_t now_ns) noexcept
    {
        const std::size_t i = idx(line);
        ++stat_packets_[i];

        // Use GapBuffer's own delivered-count return value rather than
        // diffing next_expected() before/after: next_expected() starts at a
        // 0 sentinel and is session-synced to the first real sequence
        // number *inside* the first ingest() call, which would make a
        // before/after diff double-count that first packet (the sync jump
        // plus the delivery advance). The return value has no such
        // sentinel and is exactly "how many messages this call delivered".
        const int delivered = gap_buf_.ingest(payload, len, now_ns);

        if (delivered > 0) {
            // This call is what delivered these messages downstream —
            // whether they arrived fresh and in-order, or this call's
            // packet was the one that plugged a gap and triggered a flush
            // of already-buffered out-of-order data from earlier. Either
            // way, this line's copy is the one that mattered.
            stat_seq_won_[i] += uint64_t(delivered);
            last_win_ns_[i]   = now_ns;
        } else if (delivered == 0) {
            // A true duplicate (the other line already delivered this
            // sequence), a heartbeat, or a packet that opened/extended a
            // gap and is now sitting in the out-of-order buffer awaiting
            // the missing sequence. We can't cheaply distinguish those
            // here without re-parsing the header, and the distinction
            // doesn't change any decision this class makes — none of them
            // is "the delivery that mattered" for this call.
            ++stat_redundant_[i];
        }

        return delivered;
    }

    GapBuffer& gap_buf_;

    std::array<uint64_t, 2> stat_packets_   {};
    std::array<uint64_t, 2> stat_seq_won_   {};
    std::array<uint64_t, 2> stat_redundant_ {};
    std::array<uint64_t, 2> last_win_ns_    {};
};

} // namespace feed
