// test_pcap_replay.cpp — End-to-end PCAP write + replay tests.
//
// This test:
//   1. Generates a synthetic MoldUDP64 PCAP file with known content,
//      including deliberate sequence gaps and heartbeats.
//   2. Replays it through PcapReader → GapBuffer.
//   3. Verifies the correct messages are delivered in the correct order.
//
// This validates the full pipeline without requiring a live network feed.

#include "feed/wire_format.hpp"
#include "feed/gap_buffer.hpp"
#include "feed/pcap_replay.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <cstdlib>
#include <unistd.h>  // close(), mkstemp

using namespace feed;

static int passed = 0, failed = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++failed; } else { ++passed; } \
    } while(0)

// ── Helpers ───────────────────────────────────────────────────────────────────

// A temporary file that is deleted on destruction.
struct TempFile {
    char path[64];
    TempFile() {
        std::strcpy(path, "/tmp/test_pcap_XXXXXX");
        // mkstemp creates and opens the file; close the fd, we just need the path.
        int fd = mkstemp(path);
        if (fd >= 0) close(fd);
        // Append .pcap so PcapReader recognises it (not required, just cleaner).
        std::strcat(path, ".pcap");
        // Rename the fd-opened temp to the .pcap name.
        std::rename(path - 5 < path ? path : path, path);
    }
    ~TempFile() { std::remove(path); }
};

// Build a single raw ITCH 'A' (Add Order) message body.
static std::vector<uint8_t> make_itch_add(uint64_t order_ref, char side,
                                           uint32_t shares, uint32_t price)
{
    std::vector<uint8_t> body(36, 0);
    body[0] = uint8_t('A');
    // timestamp = 0 for simplicity
    put_be64(body.data() + 7, order_ref);
    body[15] = uint8_t(side);
    put_be32(body.data() + 16, shares);
    std::memcpy(body.data() + 20, "AAPL    ", 8);
    put_be32(body.data() + 28, price);
    return body;
}

// ── Test 1: clean sequential stream ───────────────────────────────────────────

static void test_clean_stream() {
    // Write 10 packets, each with 1 message, seqs 1..10.
    char tmppath[128] = "/tmp/test_clean_XXXXXX";
    int fd = mkstemp(tmppath); close(fd);
    std::strcat(tmppath, ".pcap");

    {
        PcapWriter w(tmppath);
        CHECK(w.is_open(), "writer opened");

        const char* sess = "SESSIONA  ";
        for (uint64_t seq = 1; seq <= 10; ++seq) {
            auto body = make_itch_add(seq, 'B', 100, 1'500'000);
            w.write_mold_packet(seq * 1000, sess, seq, 1,
                                body.data(), uint16_t(body.size()));
        }
        CHECK(w.packets_written() == 10, "10 packets written");
    }

    GapBuffer gb;
    std::vector<uint64_t> delivered;
    gb.set_on_message([&](const MoldMessage& m) {
        delivered.push_back(m.seq_num);
    });

    PcapReader reader;
    auto stats = reader.replay(tmppath, gb);

    CHECK(stats.packets_read    == 10, "10 packets read");
    CHECK(stats.udp_packets     == 10, "10 UDP packets");
    CHECK(stats.messages_ingested == 10, "10 messages ingested");
    CHECK(delivered.size() == 10, "10 messages delivered");
    for (int i = 0; i < 10; ++i)
        CHECK(delivered[i] == uint64_t(i + 1), "correct sequence");

    std::remove(tmppath);
}

// ── Test 2: gap in PCAP — messages arrive out of order ────────────────────────

static void test_gap_in_pcap() {
    char tmppath[128] = "/tmp/test_gap_XXXXXX";
    int fd = mkstemp(tmppath); close(fd);
    std::strcat(tmppath, ".pcap");

    // Write seqs 1, 2, 4, 5 (gap at 3).
    {
        PcapWriter w(tmppath);
        const char* sess = "SESSIONA  ";
        for (uint64_t seq : {1ULL, 2ULL, 4ULL, 5ULL}) {
            auto body = make_itch_add(seq, 'S', 200, 1'600'000);
            w.write_mold_packet(seq * 1000, sess, seq, 1,
                                body.data(), uint16_t(body.size()));
        }
    }

    GapBuffer gb;
    std::vector<uint64_t> delivered;
    std::vector<GapEvent> gaps;
    gb.set_on_message([&](const MoldMessage& m) { delivered.push_back(m.seq_num); });
    gb.set_on_gap([&](const GapEvent& g)         { gaps.push_back(g); });

    PcapReader reader;
    reader.replay(tmppath, gb);

    // Seqs 1 and 2 are delivered immediately; 4 and 5 are buffered.
    CHECK(delivered.size() == 2, "only 2 delivered (gap at 3)");
    CHECK(gaps.size() == 1,      "one gap detected");
    CHECK(gaps[0].first_missing_seq == 3, "gap at seq 3");
    CHECK(gb.in_gap(),           "still in gap after replay");

    std::remove(tmppath);
}

// ── Test 3: heartbeat packets are not delivered as messages ───────────────────

static void test_heartbeat_in_pcap() {
    char tmppath[128] = "/tmp/test_hb_XXXXXX";
    int fd = mkstemp(tmppath); close(fd);
    std::strcat(tmppath, ".pcap");

    {
        PcapWriter w(tmppath);
        const char* sess = "SESSIONA  ";

        // seq 1, heartbeat, seq 2
        auto b1 = make_itch_add(1, 'B', 100, 1'500'000);
        w.write_mold_packet(1000, sess, 1, 1, b1.data(), uint16_t(b1.size()));
        w.write_heartbeat(2000, sess);
        auto b2 = make_itch_add(2, 'B', 200, 1'510'000);
        w.write_mold_packet(3000, sess, 2, 1, b2.data(), uint16_t(b2.size()));
    }

    GapBuffer gb;
    int delivered = 0;
    gb.set_on_message([&](const MoldMessage&) { ++delivered; });

    PcapReader reader;
    auto stats = reader.replay(tmppath, gb);

    CHECK(stats.packets_read == 3,    "3 packets read (including heartbeat)");
    CHECK(delivered == 2,             "only 2 messages (heartbeat not a message)");
    CHECK(gb.stat_heartbeats() == 1,  "1 heartbeat counted");

    std::remove(tmppath);
}

// ── Test 4: multi-message packets survive write/read round-trip ───────────────

static void test_multi_message_roundtrip() {
    char tmppath[128] = "/tmp/test_multi_XXXXXX";
    int fd = mkstemp(tmppath); close(fd);
    std::strcat(tmppath, ".pcap");

    {
        PcapWriter w(tmppath);
        const char* sess = "SESSIONA  ";

        // One packet with 3 messages: build concatenated bodies.
        // For simplicity: 3 minimal 4-byte dummy messages.
        uint8_t bodies[3 * (2 + 4)] = {};
        for (int i = 0; i < 3; ++i) {
            put_be16(bodies + i * 6, 4);
            std::memset(bodies + i * 6 + 2, uint8_t(0xA0 + i), 4);
        }
        // We'll write three separate single-message packets for simplicity.
        for (int i = 0; i < 3; ++i) {
            uint8_t body[4];
            std::memset(body, uint8_t(0xA0 + i), 4);
            w.write_mold_packet(uint64_t(i + 1) * 1000, sess,
                                uint64_t(i + 1), 1, body, 4);
        }
    }

    GapBuffer gb;
    int delivered = 0;
    gb.set_on_message([&](const MoldMessage&) { ++delivered; });

    PcapReader reader;
    reader.replay(tmppath, gb);
    CHECK(delivered == 3, "3 messages from 3 packets");

    std::remove(tmppath);
}

// ── Test 5: filter port — non-matching UDP port is skipped ────────────────────

static void test_port_filter() {
    char tmppath[128] = "/tmp/test_port_XXXXXX";
    int fd = mkstemp(tmppath); close(fd);
    std::strcat(tmppath, ".pcap");

    {
        PcapWriter w(tmppath);
        const char* sess = "SESSIONA  ";
        auto body = make_itch_add(1, 'B', 100, 1'500'000);
        w.write_mold_packet(1000, sess, 1, 1, body.data(), uint16_t(body.size()));
    }

    GapBuffer gb;
    int delivered = 0;
    gb.set_on_message([&](const MoldMessage&) { ++delivered; });

    // Filter for port 9999 — our packets are on 15001, so none should match.
    PcapReader::Config cfg;
    cfg.filter_port = 9999;
    PcapReader reader(cfg);
    auto stats = reader.replay(tmppath, gb);

    CHECK(stats.non_udp_skipped == 1, "packet filtered by port");
    CHECK(delivered == 0,             "no messages delivered");

    std::remove(tmppath);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== PCAP Replay Tests ===\n\n");
    test_clean_stream();
    test_gap_in_pcap();
    test_heartbeat_in_pcap();
    test_multi_message_roundtrip();
    test_port_filter();

    std::printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
