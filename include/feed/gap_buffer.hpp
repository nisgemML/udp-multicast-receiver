#pragma once
// gap_buffer.hpp — Sequence gap detection and out-of-order message buffering.
//
// ── The problem ───────────────────────────────────────────────────────────────
//
// UDP multicast is unreliable.  Packets can be:
//   • Dropped by a switch buffer overflow during traffic bursts
//   • Reordered by asymmetric routing (rare but happens in co-lo)
//   • Duplicated by network equipment
//
// The receiver must detect all three cases and handle them without
// blocking the hot path.  Delivery to downstream must be in-order and
// without duplicates.
//
// ── Design ────────────────────────────────────────────────────────────────────
//
// Gap detection: maintain `next_expected_seq`.  On each packet:
//   seq > next_expected  → gap: buffer this packet, request retransmit
//   seq == next_expected → in-order: deliver immediately, flush buffer
//   seq < next_expected  → duplicate: drop silently, count it
//
// Buffering: a fixed-size circular buffer keyed by sequence number.
// Slot index = seq % kCapacity.  On each in-order delivery we scan forward
// from next_expected to flush any already-received out-of-order messages.
//
// Retransmit requests: when a gap is first detected, record the gap start.
// If the gap is not filled within `gap_timeout_ns` nanoseconds, emit a
// RetransmitRequest.  We rate-limit requests to avoid flooding the server
// (one request per gap per `retry_interval_ns`).
//
// ── Capacity choice ───────────────────────────────────────────────────────────
//
// NASDAQ ITCH at peak is ~10M messages/second.  At 1µs retransmit RTT,
// a worst-case gap lasts ~10 messages.  We size the buffer at 4096 to
// handle multi-millisecond gaps without dropping newly-arrived messages.
// 4096 * sizeof(BufferedPacket) ≈ 4096 * 1500 bytes ≈ 6 MB — acceptable.

#include "feed/wire_format.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>
#include <cassert>

namespace feed {

// ── Gap event reported to the application ─────────────────────────────────────

struct GapEvent {
    uint64_t first_missing_seq;
    uint64_t last_missing_seq;   // inclusive
    uint64_t detected_at_ns;     // CLOCK_MONOTONIC nanoseconds
};

// ── Buffered packet slot ───────────────────────────────────────────────────────

static constexpr std::size_t kMaxPacketBytes = 1500;

struct BufferedPacket {
    uint64_t seq_num  = 0;
    uint16_t msg_count = 0;
    bool     occupied  = false;
    uint16_t data_len  = 0;
    uint8_t  data[kMaxPacketBytes];  // raw MoldUDP64 payload (after the 20-byte header)
};

// ── GapBuffer ─────────────────────────────────────────────────────────────────

class GapBuffer {
public:
    // Capacity must be a power of two for fast modulo via bitmask.
    static constexpr std::size_t kCapacity = 4096;
    static_assert((kCapacity & (kCapacity - 1)) == 0);
    static constexpr std::size_t kMask = kCapacity - 1;

    // Callback types.
    // OnMessage: called for each in-order MoldMessage delivered downstream.
    // OnGap: called once when a new gap is first detected.
    // OnRetransmitRequest: called when a RetransmitRequest should be sent.
    using OnMessage          = std::function<void(const MoldMessage&)>;
    using OnGap              = std::function<void(const GapEvent&)>;
    using OnRetransmitRequest = std::function<void(const RetransmitRequest&)>;

    struct Config {
        uint64_t gap_timeout_ns       = 100'000;   // 100 µs before first retransmit
        uint64_t retry_interval_ns    = 1'000'000; // 1 ms between retransmit retries
        uint16_t max_retransmit_count = 100;        // max messages per retransmit request
        char     session[10];

        Config() noexcept : gap_timeout_ns(100'000), retry_interval_ns(1'000'000),
                            max_retransmit_count(100) {
            std::memset(session, ' ', 10);
        }
    };

    explicit GapBuffer(Config cfg = {})
        : cfg_(cfg)
    {
        std::memcpy(session_, cfg_.session, 10);
        slots_.fill({});
    }

    void set_on_message(OnMessage cb)           { on_message_ = std::move(cb); }
    void set_on_gap(OnGap cb)                   { on_gap_     = std::move(cb); }
    void set_on_retransmit(OnRetransmitRequest cb) { on_retx_ = std::move(cb); }

    // ── Primary entry point ───────────────────────────────────────────────────
    //
    // Call this for every received MoldUDP64 datagram.
    // `now_ns` is the packet receive timestamp (CLOCK_MONOTONIC nanoseconds).
    // `payload` is the datagram bytes starting at the MoldUDP64 header.
    //
    // Returns: number of messages delivered downstream, -1 on parse error.

    int ingest(const uint8_t* payload, std::size_t len, uint64_t now_ns) noexcept {
        MoldHeader hdr;
        if (!hdr.parse(payload, len)) return -1;

        // Sync session on first packet.
        if (next_expected_ == 0 && !session_synced_) {
            std::memcpy(session_, hdr.session, 10);
            next_expected_ = hdr.seq_num;
            session_synced_ = true;
        }

        if (hdr.is_heartbeat()) {
            ++stat_heartbeats_;
            check_gap_timeout(now_ns);
            return 0;
        }

        ++stat_packets_;
        stat_messages_ += hdr.msg_count;

        const uint64_t pkt_first = hdr.seq_num;
        const uint64_t pkt_last  = pkt_first + hdr.msg_count - 1;

        // Duplicate: entire packet already processed.
        if (pkt_last < next_expected_) {
            ++stat_duplicates_;
            return 0;
        }

        if (pkt_first > next_expected_) {
            // Gap detected.
            handle_gap(pkt_first, now_ns);
            buffer_packet(hdr, payload, len, now_ns);
            check_gap_timeout(now_ns);
            return 0;
        }

        // In-order (or partial overlap with duplicate prefix).
        int delivered = deliver_packet(hdr, payload, len);
        delivered    += flush_buffer();
        // Clear gap tracker if next_expected_ has advanced past all missing seqs.
        // next_expected_ only advances by delivering in-order messages, so when
        // it exceeds gap_start_, every sequence in [gap_start_, next_expected_)
        // has been delivered — the gap is fully resolved.
        if (gap_start_ != 0 && next_expected_ > gap_start_)
            gap_start_ = 0;
        return delivered;
    }

    // Periodic tick — call at ~100µs intervals to detect stalled gaps.
    void tick(uint64_t now_ns) noexcept { check_gap_timeout(now_ns); }

    // ── Statistics ────────────────────────────────────────────────────────────

    [[nodiscard]] uint64_t next_expected()   const noexcept { return next_expected_; }
    [[nodiscard]] uint64_t stat_packets()    const noexcept { return stat_packets_; }
    [[nodiscard]] uint64_t stat_messages()   const noexcept { return stat_messages_; }
    [[nodiscard]] uint64_t stat_gaps()       const noexcept { return stat_gaps_; }
    [[nodiscard]] uint64_t stat_duplicates() const noexcept { return stat_duplicates_; }
    [[nodiscard]] uint64_t stat_delivered()  const noexcept { return stat_delivered_; }
    [[nodiscard]] uint64_t stat_retransmit_requests() const noexcept { return stat_retx_; }
    [[nodiscard]] uint64_t stat_heartbeats() const noexcept { return stat_heartbeats_; }
    [[nodiscard]] bool     in_gap()          const noexcept { return gap_start_ != 0; }

private:
    // ── Gap handling ──────────────────────────────────────────────────────────

    void handle_gap(uint64_t received_seq, uint64_t now_ns) noexcept {
        if (gap_start_ == 0) {
            // First packet revealing this gap.
            gap_start_         = next_expected_;
            gap_detected_at_ns = now_ns;
            gap_last_retx_ns   = 0;
            ++stat_gaps_;

            if (on_gap_) {
                GapEvent ev;
                ev.first_missing_seq = gap_start_;
                ev.last_missing_seq  = received_seq - 1;
                ev.detected_at_ns    = now_ns;
                on_gap_(ev);
            }
        }
    }

    void check_gap_timeout(uint64_t now_ns) noexcept {
        if (gap_start_ == 0) return;

        const uint64_t since_detect = now_ns - gap_detected_at_ns;
        const uint64_t since_retx   = gap_last_retx_ns
                                      ? (now_ns - gap_last_retx_ns)
                                      : since_detect;

        const bool first_request  = (gap_last_retx_ns == 0) &&
                                    (since_detect >= cfg_.gap_timeout_ns);
        const bool retry_request  = (gap_last_retx_ns != 0) &&
                                    (since_retx >= cfg_.retry_interval_ns);

        if (!first_request && !retry_request) return;
        if (!on_retx_) return;

        // Count how many messages are still missing starting from gap_start_.
        // A sequence is missing if its slot is either empty or occupied by a
        // different sequence (wrap-around collision).
        uint64_t missing_count = 0;
        for (uint64_t s = gap_start_; s < gap_start_ + kCapacity; ++s) {
            const auto& slot = slots_[s & kMask];
            if (slot.occupied && slot.seq_num == s) break;  // already received
            ++missing_count;
            if (missing_count >= cfg_.max_retransmit_count) break;
        }
        if (missing_count == 0) { gap_start_ = 0; return; }

        RetransmitRequest req;
        std::memcpy(req.session, session_, 10);
        req.first_seq = gap_start_;
        req.count     = static_cast<uint16_t>(missing_count);
        on_retx_(req);

        gap_last_retx_ns = now_ns;
        ++stat_retx_;
    }

    // ── Packet buffering ──────────────────────────────────────────────────────

    void buffer_packet(const MoldHeader& hdr, const uint8_t* payload,
                       std::size_t len, uint64_t /*now_ns*/) noexcept
    {
        const uint64_t slot_idx = hdr.seq_num & kMask;
        auto& slot = slots_[slot_idx];

        // If the slot is already occupied by a different sequence (wrap-around
        // collision), we have a bigger problem — the buffer is full.
        // In practice this means the gap is > 4096 messages: log and drop.
        if (slot.occupied && slot.seq_num != hdr.seq_num) {
            ++stat_buffer_overflows_;
            return;
        }

        slot.occupied  = true;
        slot.seq_num   = hdr.seq_num;
        slot.msg_count = hdr.msg_count;
        // Copy the message payload (everything after the 20-byte MoldHeader).
        const std::size_t body_offset = kMoldHeaderSize;
        const std::size_t body_len    = (len > body_offset)
                                        ? (len - body_offset) : 0;
        slot.data_len = static_cast<uint16_t>(
            std::min(body_len, sizeof(slot.data)));
        std::memcpy(slot.data, payload + body_offset, slot.data_len);
    }

    // ── In-order delivery ─────────────────────────────────────────────────────

    // Deliver all messages in a freshly-received in-order packet.
    int deliver_packet(const MoldHeader& hdr, const uint8_t* payload,
                       std::size_t len) noexcept
    {
        int count = 0;
        const uint8_t* cursor = payload + kMoldHeaderSize;
        const uint8_t* end    = payload + len;
        uint64_t seq = hdr.seq_num;

        for (uint16_t i = 0; i < hdr.msg_count && cursor + 2 <= end; ++i) {
            const uint16_t msg_len = be16(cursor);
            cursor += 2;
            if (cursor + msg_len > end) break;

            if (seq >= next_expected_) {
                MoldMessage msg{ msg_len, cursor, seq };
                if (on_message_) on_message_(msg);
                ++count;
                ++stat_delivered_;
            }
            cursor += msg_len;
            ++seq;
        }

        // Advance next_expected past everything in this packet.
        if (seq > next_expected_) next_expected_ = seq;
        return count;
    }

    // Flush buffered out-of-order packets that are now in-sequence.
    int flush_buffer() noexcept {
        int count = 0;
        for (;;) {
            auto& slot = slots_[next_expected_ & kMask];
            if (!slot.occupied || slot.seq_num != next_expected_) break;

            // Reconstruct a fake payload to reuse deliver_packet's walk.
            // Build: MoldHeader (20 bytes) + slot.data
            uint8_t buf[kMoldHeaderSize + kMaxPacketBytes];
            MoldHeader hdr;
            std::memcpy(hdr.session, session_, 10);
            hdr.seq_num   = slot.seq_num;
            hdr.msg_count = slot.msg_count;
            hdr.serialise(buf);
            std::memcpy(buf + kMoldHeaderSize, slot.data, slot.data_len);

            count += deliver_packet(hdr, buf, kMoldHeaderSize + slot.data_len);

            slot.occupied = false;

        }
        return count;
    }

    // ── State ─────────────────────────────────────────────────────────────────

    Config   cfg_;
    char     session_[10]     = {};
    bool     session_synced_  = false;
    uint64_t next_expected_   = 0;

    // Gap tracking
    uint64_t gap_start_         = 0;
    uint64_t gap_detected_at_ns = 0;
    uint64_t gap_last_retx_ns   = 0;

    // Callbacks
    OnMessage           on_message_;
    OnGap               on_gap_;
    OnRetransmitRequest on_retx_;

    // Out-of-order buffer
    std::array<BufferedPacket, kCapacity> slots_;

    // Statistics
    uint64_t stat_packets_          = 0;
    uint64_t stat_messages_         = 0;
    uint64_t stat_gaps_             = 0;
    uint64_t stat_duplicates_       = 0;
    uint64_t stat_delivered_        = 0;
    uint64_t stat_retx_             = 0;
    uint64_t stat_heartbeats_       = 0;
    uint64_t stat_buffer_overflows_ = 0;
};

} // namespace feed
