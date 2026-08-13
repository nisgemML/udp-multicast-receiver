#pragma once
// wire_format.hpp — MoldUDP64 framing + ITCH 5.0 message layout.
//
// ── MoldUDP64 protocol ────────────────────────────────────────────────────────
//
// MoldUDP64 is NASDAQ's sequenced UDP multicast protocol.  Every datagram
// carries a fixed 20-byte header followed by zero or more ITCH messages:
//
//   [0..9]   Session (10 bytes, ASCII space-padded, NOT null-terminated)
//   [10..17] Sequence number of the FIRST message in this packet (uint64 BE)
//   [18..19] Message count (uint16 BE).  Zero = heartbeat.
//   Then for each of the `count` messages:
//     [0..1]  Message length in bytes, not including these 2 bytes (uint16 BE)
//     [2..N]  ITCH message body
//
// Gap detection: the receiver tracks `next_expected_seq`.  On each packet:
//   if header.seq_num > next_expected_seq  → gap of (seq_num - next_expected) messages
//   if header.seq_num < next_expected_seq  → duplicate / retransmit
//
// Recovery: send a RetransmitRequest over TCP to the MoldUDP64 retransmission
// server on port 1236 (convention).  The server replays the requested range
// as a sequence of MoldUDP64 packets over TCP.
//
// ── This file ─────────────────────────────────────────────────────────────────
//
// Pure header, no system headers beyond <cstdint> and <cstring>.
// All structs use #pragma pack(push,1) or explicit field layout to match
// the on-wire format exactly — no padding bytes, no alignment surprises.
// Big-endian helpers use __builtin_bswap* to avoid the glibc ntohl branch
// and to keep this header free of POSIX socket dependencies.

#include <cstdint>
#include <cstring>
#include <string_view>

namespace feed {

// ── Big-endian read/write helpers ─────────────────────────────────────────────

[[nodiscard]] inline uint16_t be16(const uint8_t* p) noexcept {
    uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v);
}
[[nodiscard]] inline uint32_t be32(const uint8_t* p) noexcept {
    uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v);
}
[[nodiscard]] inline uint64_t be64(const uint8_t* p) noexcept {
    uint64_t v; std::memcpy(&v, p, 8); return __builtin_bswap64(v);
}
// 48-bit big-endian (ITCH timestamp format: 6 bytes, nanoseconds since midnight)
[[nodiscard]] inline uint64_t be48(const uint8_t* p) noexcept {
    return (uint64_t(p[0]) << 40) | (uint64_t(p[1]) << 32) |
           (uint64_t(p[2]) << 24) | (uint64_t(p[3]) << 16) |
           (uint64_t(p[4]) <<  8) |  uint64_t(p[5]);
}
inline void put_be16(uint8_t* p, uint16_t v) noexcept {
    v = __builtin_bswap16(v); std::memcpy(p, &v, 2);
}
inline void put_be32(uint8_t* p, uint32_t v) noexcept {
    v = __builtin_bswap32(v); std::memcpy(p, &v, 4);
}
inline void put_be64(uint8_t* p, uint64_t v) noexcept {
    v = __builtin_bswap64(v); std::memcpy(p, &v, 8);
}

// ── MoldUDP64 header ──────────────────────────────────────────────────────────

static constexpr std::size_t kMoldHeaderSize = 20;

struct MoldHeader {
    char     session[10];   // ASCII, space-padded, NOT null-terminated
    uint64_t seq_num;       // native byte order after parse()
    uint16_t msg_count;     // 0 = heartbeat

    [[nodiscard]] bool parse(const uint8_t* buf, std::size_t len) noexcept {
        if (len < kMoldHeaderSize) return false;
        std::memcpy(session, buf, 10);
        seq_num   = be64(buf + 10);
        msg_count = be16(buf + 18);
        return true;
    }

    // Serialise into a 20-byte buffer (for generating test packets).
    void serialise(uint8_t* buf) const noexcept {
        std::memcpy(buf, session, 10);
        put_be64(buf + 10, seq_num);
        put_be16(buf + 18, msg_count);
    }

    [[nodiscard]] bool is_heartbeat() const noexcept { return msg_count == 0; }
    [[nodiscard]] std::string_view session_view() const noexcept {
        return { session, 10 };
    }
};

// A single decoded message extracted from a MoldUDP64 datagram.
struct MoldMessage {
    uint16_t       length;    // body length in bytes
    const uint8_t* body;      // pointer into the original buffer — zero-copy
    uint64_t       seq_num;   // global sequence number, assigned by the parser
};

// ── ITCH 5.0 message types ────────────────────────────────────────────────────

enum class ItchMsgType : uint8_t {
    SystemEvent        = 'S',
    StockDirectory     = 'R',
    AddOrderNoMpid     = 'A',
    AddOrderMpid       = 'F',
    OrderExecuted      = 'E',
    OrderExecutedPrice = 'C',
    OrderCancel        = 'X',
    OrderDelete        = 'D',
    OrderReplace       = 'U',
    Trade              = 'P',
    CrossTrade         = 'Q',
    BrokenTrade        = 'B',
    NOII               = 'I',
    Unknown            = 0xFF,
};

[[nodiscard]] inline ItchMsgType itch_type(const uint8_t* body) noexcept {
    return static_cast<ItchMsgType>(body[0]);
}

// ITCH timestamps are 6-byte big-endian nanoseconds since midnight.
// All message bodies start: type(1) + timestamp(6).
[[nodiscard]] inline uint64_t itch_timestamp_ns(const uint8_t* body) noexcept {
    return be48(body + 1);
}

// ── ITCH decoded message types ────────────────────────────────────────────────

// Add Order (no MPID, 'A').  Body offsets after type byte:
//   ts(6) order_ref(8) side(1) shares(4) stock(8) price(4) = 31 bytes + type = 36 - 1 = 35
struct ItchAddOrder {
    uint64_t seq_num;
    uint64_t timestamp_ns;
    uint64_t order_ref;
    uint32_t shares;
    uint32_t price;      // fixed-point * 10000 ($12.3456 = 123456)
    char     stock[9];   // 8 chars + null terminator for convenience
    char     side;       // 'B' or 'S'
    bool     has_mpid;

    [[nodiscard]] static bool parse(const uint8_t* body, std::size_t len,
                                     ItchAddOrder& out) noexcept {
        if (len < 36) return false;
        out.timestamp_ns = be48(body + 1);
        out.order_ref    = be64(body + 7);
        out.side         = char(body[15]);
        out.shares       = be32(body + 16);
        std::memcpy(out.stock, body + 20, 8);
        out.stock[8]     = '\0';
        out.price        = be32(body + 28);
        out.has_mpid     = (body[0] == uint8_t('F'));
        return true;
    }
};

// Order Delete ('D').  Body: type(1) ts(6) locate(2) tracking(2) order_ref(8) = 19
struct ItchDeleteOrder {
    uint64_t seq_num;
    uint64_t timestamp_ns;
    uint64_t order_ref;

    [[nodiscard]] static bool parse(const uint8_t* body, std::size_t len,
                                     ItchDeleteOrder& out) noexcept {
        if (len < 19) return false;
        out.timestamp_ns = be48(body + 1);
        out.order_ref    = be64(body + 11);
        return true;
    }
};

// Order Executed ('E').  Body: type(1) ts(6) locate(2) tracking(2) order_ref(8) shares(4) match(8) = 31
struct ItchOrderExecuted {
    uint64_t seq_num;
    uint64_t timestamp_ns;
    uint64_t order_ref;
    uint32_t executed_shares;
    uint64_t match_number;

    [[nodiscard]] static bool parse(const uint8_t* body, std::size_t len,
                                     ItchOrderExecuted& out) noexcept {
        if (len < 31) return false;
        out.timestamp_ns    = be48(body + 1);
        out.order_ref       = be64(body + 11);
        out.executed_shares = be32(body + 19);
        out.match_number    = be64(body + 23);
        return true;
    }
};

// ── MoldUDP64 retransmit request ──────────────────────────────────────────────
//
// Sent over TCP to the retransmission server when a sequence gap is detected.
// Format: session(10) + first_seq_requested(8 BE) + count(2 BE) = 20 bytes.
// The server replays [first_seq, first_seq + count) as MoldUDP64 packets over TCP.

struct RetransmitRequest {
    char     session[10];
    uint64_t first_seq;
    uint16_t count;

    void serialise(uint8_t* buf) const noexcept {
        std::memcpy(buf, session, 10);
        put_be64(buf + 10, first_seq);
        put_be16(buf + 18, count);
    }
    static constexpr std::size_t kSize = 20;
};

// ── PCAP file format ───────────────────────────────────────────────────────────
//
// Standard libpcap global header (24 bytes) + per-packet records.
// All fields are in host byte order (magic number determines endianness).

#pragma pack(push, 1)
struct PcapGlobalHeader {
    uint32_t magic_number  = 0xa1b2c3d4;
    uint16_t version_major = 2;
    uint16_t version_minor = 4;
    int32_t  thiszone      = 0;
    uint32_t sigfigs       = 0;
    uint32_t snaplen       = 65535;
    uint32_t network       = 1;   // LINKTYPE_ETHERNET
};
struct PcapRecordHeader {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
};
#pragma pack(pop)

static constexpr uint32_t kPcapMagicLE       = 0xa1b2c3d4;
static constexpr uint32_t kPcapMagicBE       = 0xd4c3b2a1;
static constexpr uint32_t kPcapMagicNsLE     = 0xa1b23c4d; // nanosecond variant
static constexpr std::size_t kEtherIpUdpHdrSize = 42;      // Eth(14)+IP(20)+UDP(8)

} // namespace feed
