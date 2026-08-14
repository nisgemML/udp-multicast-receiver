#pragma once
// pcap_replay.hpp — PCAP file reader + MoldUDP64 packet injector.
//
// ── Why this matters ──────────────────────────────────────────────────────────
//
// Production feed receivers are impossible to test against a live exchange
// feed in CI.  PCAP replay solves this: capture a real NASDAQ multicast feed
// (or generate a synthetic one), write it to a .pcap file, and replay it
// through the same receiver code at controlled timing.
//
// This is standard practice in every HFT firm:
//   1. Record a day's feed with `tcpdump -w feed.pcap -i eth0 udp port 15001`
//   2. Replay it against your receiver in CI to catch regressions
//   3. Inject deliberate gaps, duplicates, and reorders to test recovery
//
// ── PcapReader ────────────────────────────────────────────────────────────────
//
// Reads standard libpcap files (magic 0xa1b2c3d4, LINKTYPE_ETHERNET).
// Extracts UDP payloads from Ethernet+IP+UDP frames and feeds them to a
// GapBuffer via the same ingest() path used by the live receiver.
//
// Supports:
//   • Both byte-order variants of the pcap magic number
//   • VLAN-tagged frames (EtherType 0x8100)
//   • Replay at wall-clock speed or as-fast-as-possible
//
// ── PcapWriter ────────────────────────────────────────────────────────────────
//
// Writes synthetic pcap files for testing.  Used by the test suite to generate
// controlled packet sequences with known gaps, duplicates, and timing.

#include "feed/wire_format.hpp"
#include "feed/gap_buffer.hpp"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <netinet/in.h>   // IPPROTO_UDP
#include <time.h>         // clock_gettime, CLOCK_MONOTONIC

namespace feed {

// ── PcapWriter ────────────────────────────────────────────────────────────────
//
// Writes a pcap file containing UDP datagrams carrying MoldUDP64 packets.
// The Ethernet+IP+UDP headers are synthesised with fixed dummy addresses.

class PcapWriter {
public:
    explicit PcapWriter(const std::string& path) {
        fp_ = std::fopen(path.c_str(), "wb");
        if (!fp_) return;
        PcapGlobalHeader gh{};
        std::fwrite(&gh, sizeof(gh), 1, fp_);
    }
    ~PcapWriter() { if (fp_) std::fclose(fp_); }

    [[nodiscard]] bool is_open() const noexcept { return fp_ != nullptr; }

    // Write a single MoldUDP64 packet as a pcap record.
    // `ts_us` is the packet timestamp in microseconds since epoch.
    // `payload` is the raw MoldUDP64 bytes (header + messages).
    bool write_packet(uint64_t ts_us, const uint8_t* payload,
                      uint16_t payload_len) noexcept
    {
        if (!fp_) return false;

        // Build Ethernet(14) + IP(20) + UDP(8) + payload.
        const uint16_t udp_len  = 8 + payload_len;
        const uint16_t ip_len   = 20 + udp_len;
        const uint16_t frame_len = 14 + ip_len;

        uint8_t frame[14 + 20 + 8 + 65535];
        uint8_t* p = frame;

        // Ethernet header: dst(6) src(6) type(2)
        std::memset(p, 0xFF, 6); p += 6;               // dst = broadcast
        std::memset(p, 0x00, 6); p += 6;               // src = 00:00:00:00:00:00
        p[0] = 0x08; p[1] = 0x00; p += 2;             // EtherType = IPv4

        // IP header (minimal, no options)
        p[0]  = 0x45;                                   // version=4, IHL=5
        p[1]  = 0;                                      // DSCP/ECN
        put_be16(p + 2,  ip_len);
        put_be16(p + 4,  0);                            // ID
        put_be16(p + 6,  0);                            // flags/frag
        p[8]  = 64;                                     // TTL
        p[9]  = IPPROTO_UDP;
        put_be16(p + 10, 0);                            // checksum (0 = unchecked)
        // src = 192.168.0.1
        p[12] = 192; p[13] = 168; p[14] = 0; p[15] = 1;
        // dst = 239.1.1.1 (multicast)
        p[16] = 239; p[17] = 1; p[18] = 1; p[19] = 1;
        p += 20;

        // UDP header
        put_be16(p,     15001);                         // src port
        put_be16(p + 2, 15001);                         // dst port
        put_be16(p + 4, udp_len);
        put_be16(p + 6, 0);                             // checksum (0 = disabled)
        p += 8;

        // MoldUDP64 payload
        std::memcpy(p, payload, payload_len);

        PcapRecordHeader rh{};
        rh.ts_sec  = static_cast<uint32_t>(ts_us / 1'000'000);
        rh.ts_usec = static_cast<uint32_t>(ts_us % 1'000'000);
        rh.incl_len = rh.orig_len = frame_len;

        std::fwrite(&rh,    sizeof(rh),  1,         fp_);
        std::fwrite(frame,  1,           frame_len, fp_);
        ++packets_written_;
        return true;
    }

    // Write a MoldUDP64 heartbeat packet.
    bool write_heartbeat(uint64_t ts_us, const char* session) noexcept {
        uint8_t buf[kMoldHeaderSize];
        MoldHeader hdr{};
        std::memcpy(hdr.session, session, 10);
        hdr.seq_num   = 0;
        hdr.msg_count = 0;
        hdr.serialise(buf);
        return write_packet(ts_us, buf, kMoldHeaderSize);
    }

    // Write a MoldUDP64 packet carrying one raw ITCH message body.
    // Returns false if payload would exceed a UDP datagram.
    bool write_mold_packet(uint64_t ts_us, const char* session,
                           uint64_t seq_num, uint16_t msg_count,
                           const uint8_t* msg_body, uint16_t msg_body_len) noexcept
    {
        // MoldHeader(20) + per-message length(2) + body
        const uint16_t total = kMoldHeaderSize + 2 + msg_body_len;
        if (total > 65507) return false;

        std::vector<uint8_t> buf(total);
        MoldHeader hdr{};
        std::memcpy(hdr.session, session, 10);
        hdr.seq_num   = seq_num;
        hdr.msg_count = msg_count;
        hdr.serialise(buf.data());

        // Message envelope: length (2 bytes BE) + body.
        put_be16(buf.data() + kMoldHeaderSize, msg_body_len);
        std::memcpy(buf.data() + kMoldHeaderSize + 2, msg_body, msg_body_len);

        return write_packet(ts_us, buf.data(), total);
    }

    [[nodiscard]] uint64_t packets_written() const noexcept { return packets_written_; }

private:
    std::FILE* fp_             = nullptr;
    uint64_t   packets_written_ = 0;
};

// ── PcapReader ────────────────────────────────────────────────────────────────

struct ReplayStats {
    uint64_t packets_read      = 0;
    uint64_t udp_packets       = 0;
    uint64_t non_udp_skipped   = 0;
    uint64_t truncated_skipped = 0;
    uint64_t messages_ingested = 0;
};

class PcapReader {
public:
    struct Config {
        bool     realtime    = false;
        uint64_t speed_mult  = 1;
        uint16_t filter_port = 0;
    };

    explicit PcapReader() : cfg_() {}
    explicit PcapReader(Config cfg) : cfg_(std::move(cfg)) {}

    // Read a pcap file and feed all UDP payloads to `gap_buf`.
    // `on_packet` is called for each extracted payload (optional).
    // Returns stats, or empty stats with error message on failure.
    ReplayStats replay(const std::string& path, GapBuffer& gap_buf,
                       std::function<void(const uint8_t*, std::size_t, uint64_t)>
                           on_packet = nullptr)
    {
        ReplayStats stats{};

        std::FILE* fp = std::fopen(path.c_str(), "rb");
        if (!fp) {
            std::fprintf(stderr, "[PcapReader] Cannot open %s\n", path.c_str());
            return stats;
        }

        // Read global header.
        PcapGlobalHeader gh{};
        if (std::fread(&gh, sizeof(gh), 1, fp) != 1) {
            std::fclose(fp); return stats;
        }

        const bool swap_bytes = (gh.magic_number == kPcapMagicBE);
        if (gh.magic_number != kPcapMagicLE && !swap_bytes) {
            std::fprintf(stderr, "[PcapReader] Unknown pcap magic 0x%08X\n",
                         gh.magic_number);
            std::fclose(fp); return stats;
        }

        auto maybe_swap32 = [swap_bytes](uint32_t v) {
            return swap_bytes ? __builtin_bswap32(v) : v;
        };

        uint8_t frame_buf[65535 + 16];
        uint64_t first_ts_us  = 0;
        uint64_t first_real_ns = 0;

        while (true) {
            PcapRecordHeader rh{};
            if (std::fread(&rh, sizeof(rh), 1, fp) != 1) break;

            const uint32_t incl  = maybe_swap32(rh.incl_len);
            const uint32_t ts_s  = maybe_swap32(rh.ts_sec);
            const uint32_t ts_us = maybe_swap32(rh.ts_usec);

            if (incl > sizeof(frame_buf)) {
                ++stats.truncated_skipped;
                std::fseek(fp, incl, SEEK_CUR);
                continue;
            }
            if (std::fread(frame_buf, 1, incl, fp) != incl) break;

            ++stats.packets_read;

            const uint64_t pkt_ts_us = uint64_t(ts_s) * 1'000'000ULL + ts_us;

            // Real-time pacing.
            if (cfg_.realtime && cfg_.speed_mult > 0) {
                const uint64_t now_ns = monotonic_ns();
                if (first_ts_us == 0) {
                    first_ts_us   = pkt_ts_us;
                    first_real_ns = now_ns;
                } else {
                    const uint64_t pcap_delta_ns =
                        (pkt_ts_us - first_ts_us) * 1000ULL;
                    const uint64_t target_ns =
                        first_real_ns + pcap_delta_ns / cfg_.speed_mult;
                    while (monotonic_ns() < target_ns)
                        __builtin_ia32_pause();
                }
            }

            // Extract UDP payload.
            const uint8_t* payload = nullptr;
            uint16_t        pay_len = 0;
            uint16_t        dst_port = 0;

            if (!extract_udp(frame_buf, incl, &payload, &pay_len, &dst_port)) {
                ++stats.non_udp_skipped;
                continue;
            }

            if (cfg_.filter_port && dst_port != cfg_.filter_port) {
                ++stats.non_udp_skipped;
                continue;
            }

            ++stats.udp_packets;

            const uint64_t ts_ns = pkt_ts_us * 1000ULL;
            if (on_packet) on_packet(payload, pay_len, ts_ns);

            const int ingested = gap_buf.ingest(payload, pay_len, ts_ns);
            if (ingested > 0) stats.messages_ingested += ingested;
        }

        std::fclose(fp);
        return stats;
    }

private:
    // Extract UDP payload from an Ethernet frame.  Handles VLAN tags.
    // Returns true and sets *payload_out, *len_out, *dst_port_out on success.
    static bool extract_udp(const uint8_t* frame, uint32_t frame_len,
                              const uint8_t** payload_out, uint16_t* len_out,
                              uint16_t* dst_port_out) noexcept
    {
        if (frame_len < 14) return false;

        // Ethernet header.
        uint16_t ethertype = uint16_t(frame[12]) << 8 | frame[13];
        const uint8_t* ip  = frame + 14;
        std::size_t remaining = frame_len - 14;

        // Handle 802.1Q VLAN tag.
        if (ethertype == 0x8100) {
            if (remaining < 4) return false;
            ethertype = uint16_t(ip[2]) << 8 | ip[3];
            ip        += 4;
            remaining -= 4;
        }

        if (ethertype != 0x0800) return false; // not IPv4
        if (remaining < 20)      return false;

        const uint8_t  ihl     = (ip[0] & 0x0F) * 4;
        const uint8_t  proto   = ip[9];
        if (proto != IPPROTO_UDP) return false;
        if (remaining < std::size_t(ihl) + 8) return false;

        const uint8_t* udp = ip + ihl;
        *dst_port_out      = uint16_t(udp[2]) << 8 | udp[3];
        const uint16_t udp_len = uint16_t(udp[4]) << 8 | udp[5];
        if (udp_len < 8) return false;

        *payload_out = udp + 8;
        *len_out     = udp_len - 8;
        return true;
    }

    static uint64_t monotonic_ns() noexcept {
        struct timespec ts;
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
    }

    Config cfg_;
};

} // namespace feed
