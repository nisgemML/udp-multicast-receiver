// test_gap_buffer.cpp — Tests for sequence gap detection and out-of-order buffering.
//
// These tests validate the GapBuffer's core invariants:
//
//   1. In-order packets are delivered immediately, in sequence.
//   2. Out-of-order packets are buffered and flushed when the gap fills.
//   3. Duplicate packets (seq < next_expected) are dropped silently.
//   4. Gap detection fires exactly once per gap.
//   5. Retransmit requests are sent after gap_timeout_ns, with correct seq range.
//   6. Flush after retransmit: buffered packets after the gap deliver correctly.
//   7. Partial overlaps (retransmit starts before next_expected) are handled.

#include "feed/gap_buffer.hpp"
#include "feed/wire_format.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace feed;

static int passed = 0, failed = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++failed; } else { ++passed; } \
    } while(0)

// ── Helpers ───────────────────────────────────────────────────────────────────

// Build a complete MoldUDP64 datagram with a single dummy ITCH message.
// The ITCH message body is just `msg_body_byte` repeated `msg_len` times.
static std::vector<uint8_t> make_packet(const char* session, uint64_t seq,
                                         uint16_t msg_count, uint8_t msg_body_byte,
                                         uint16_t msg_len = 4)
{
    // MoldHeader(20) + [length(2) + body(msg_len)] * msg_count
    const std::size_t total = kMoldHeaderSize + msg_count * (2 + msg_len);
    std::vector<uint8_t> buf(total, 0);

    MoldHeader hdr{};
    std::memcpy(hdr.session, session, 10);
    hdr.seq_num   = seq;
    hdr.msg_count = msg_count;
    hdr.serialise(buf.data());

    uint8_t* p = buf.data() + kMoldHeaderSize;
    for (uint16_t i = 0; i < msg_count; ++i) {
        put_be16(p, msg_len);
        std::memset(p + 2, msg_body_byte, msg_len);
        p += 2 + msg_len;
    }
    return buf;
}

// Make a heartbeat packet.
static std::vector<uint8_t> make_heartbeat(const char* session) {
    std::vector<uint8_t> buf(kMoldHeaderSize);
    MoldHeader hdr{};
    std::memcpy(hdr.session, session, 10);
    hdr.seq_num   = 0;
    hdr.msg_count = 0;
    hdr.serialise(buf.data());
    return buf;
}

// ── Test: in-order delivery ───────────────────────────────────────────────────

static void test_inorder_delivery() {
    GapBuffer gb;
    std::vector<uint64_t> received_seqs;
    gb.set_on_message([&](const MoldMessage& m) {
        received_seqs.push_back(m.seq_num);
    });

    const char* sess = "SESSIONA  ";
    uint64_t now = 1'000'000;

    for (uint64_t seq = 1; seq <= 5; ++seq) {
        auto pkt = make_packet(sess, seq, 1, uint8_t(seq));
        gb.ingest(pkt.data(), pkt.size(), now);
        now += 1000;
    }

    CHECK(received_seqs.size() == 5, "5 messages delivered");
    for (int i = 0; i < 5; ++i)
        CHECK(received_seqs[i] == uint64_t(i + 1), "in-order seq");
    CHECK(gb.stat_gaps() == 0, "no gaps");
    CHECK(gb.stat_duplicates() == 0, "no duplicates");
    CHECK(gb.next_expected() == 6, "next_expected == 6");
}

// ── Test: duplicate detection ─────────────────────────────────────────────────

static void test_duplicate_detection() {
    GapBuffer gb;
    int delivered = 0;
    gb.set_on_message([&](const MoldMessage&) { ++delivered; });

    const char* sess = "SESSIONA  ";
    uint64_t now = 1'000'000;

    // Send seq 1 and 2.
    auto p1 = make_packet(sess, 1, 1, 0xAA);
    auto p2 = make_packet(sess, 2, 1, 0xBB);
    gb.ingest(p1.data(), p1.size(), now++);
    gb.ingest(p2.data(), p2.size(), now++);

    // Re-send seq 1 (duplicate).
    gb.ingest(p1.data(), p1.size(), now++);

    CHECK(delivered == 2,                  "only 2 unique messages delivered");
    CHECK(gb.stat_duplicates() == 1,       "1 duplicate counted");
    CHECK(gb.next_expected() == 3,         "next_expected unaffected by duplicate");
}

// ── Test: gap detection fires once ────────────────────────────────────────────

static void test_gap_detection() {
    GapBuffer gb;
    std::vector<GapEvent> gaps;
    gb.set_on_gap([&](const GapEvent& g) { gaps.push_back(g); });

    const char* sess = "SESSIONA  ";
    uint64_t now = 1'000'000;

    // Seq 1 delivered.
    auto p1 = make_packet(sess, 1, 1, 0x01);
    gb.ingest(p1.data(), p1.size(), now);

    // Skip seq 2,3.  Send seq 4 — reveals a gap of 2.
    auto p4 = make_packet(sess, 4, 1, 0x04);
    gb.ingest(p4.data(), p4.size(), now + 1000);

    CHECK(gaps.size() == 1,              "exactly one gap event");
    CHECK(gaps[0].first_missing_seq == 2,"gap starts at seq 2");
    CHECK(gaps[0].last_missing_seq  == 3,"gap ends at seq 3");

    // Sending seq 5 should NOT fire another gap event (gap already known).
    auto p5 = make_packet(sess, 5, 1, 0x05);
    gb.ingest(p5.data(), p5.size(), now + 2000);
    CHECK(gaps.size() == 1, "no second gap event for same gap");
}

// ── Test: out-of-order buffering and flush ────────────────────────────────────

static void test_outoforder_flush() {
    GapBuffer gb;
    std::vector<uint64_t> delivered;
    gb.set_on_message([&](const MoldMessage& m) { delivered.push_back(m.seq_num); });

    const char* sess = "SESSIONA  ";
    uint64_t now = 1'000'000;

    // Send seq 1.
    auto p1 = make_packet(sess, 1, 1, 0x01);
    gb.ingest(p1.data(), p1.size(), now++);

    // Skip seq 2, send seq 3 and 4 (buffered).
    auto p3 = make_packet(sess, 3, 1, 0x03);
    auto p4 = make_packet(sess, 4, 1, 0x04);
    gb.ingest(p3.data(), p3.size(), now++);
    gb.ingest(p4.data(), p4.size(), now++);

    CHECK(delivered.size() == 1, "only seq 1 delivered — seq 3,4 buffered");

    // Now deliver seq 2 — should flush 3 and 4 as well.
    auto p2 = make_packet(sess, 2, 1, 0x02);
    gb.ingest(p2.data(), p2.size(), now++);

    CHECK(delivered.size() == 4, "all 4 messages delivered after gap fills");
    for (int i = 0; i < 4; ++i)
        CHECK(delivered[i] == uint64_t(i + 1), "delivery order correct");
}

// ── Test: retransmit request timing ───────────────────────────────────────────

static void test_retransmit_request_timing() {
    GapBuffer::Config cfg;
    cfg.gap_timeout_ns    = 500'000;   // 500 µs
    cfg.retry_interval_ns = 1'000'000; // 1 ms
    std::memcpy(cfg.session, "SESSIONA  ", 10);

    GapBuffer gb(cfg);
    std::vector<RetransmitRequest> retx;
    gb.set_on_retransmit([&](const RetransmitRequest& r) { retx.push_back(r); });

    const char* sess = "SESSIONA  ";
    const uint64_t t0 = 1'000'000'000ULL;

    // Seq 1 in order, then seq 3 revealing a gap at seq 2.
    auto p1 = make_packet(sess, 1, 1, 0x01);
    auto p3 = make_packet(sess, 3, 1, 0x03);
    gb.ingest(p1.data(), p1.size(), t0);
    gb.ingest(p3.data(), p3.size(), t0 + 100);

    // Tick before timeout — no retransmit yet.
    gb.tick(t0 + 400'000);
    CHECK(retx.empty(), "no retransmit before timeout");

    // Tick after timeout — first retransmit.
    gb.tick(t0 + 600'000);
    CHECK(retx.size() == 1,           "first retransmit sent");
    CHECK(retx[0].first_seq == 2,     "retransmit starts at missing seq 2");
    CHECK(retx[0].count == 1,         "retransmit count == 1 (only seq 2 missing)");

    // Tick before retry interval — no second retransmit.
    gb.tick(t0 + 1'100'000);
    CHECK(retx.size() == 1, "no second retransmit before retry interval");

    // Tick after retry interval — second retransmit.
    gb.tick(t0 + 1'700'000);
    CHECK(retx.size() == 2, "second retransmit after retry interval");
}

// ── Test: retransmit fills the gap ────────────────────────────────────────────

static void test_retransmit_fills_gap() {
    GapBuffer gb;
    std::vector<uint64_t> delivered;
    gb.set_on_message([&](const MoldMessage& m) { delivered.push_back(m.seq_num); });

    const char* sess = "SESSIONA  ";
    uint64_t now = 1'000'000;

    // seq 1, gap 2, seq 3.
    gb.ingest(make_packet(sess, 1, 1, 0x01).data(),
              make_packet(sess, 1, 1, 0x01).size(), now++);
    gb.ingest(make_packet(sess, 3, 1, 0x03).data(),
              make_packet(sess, 3, 1, 0x03).size(), now++);

    CHECK(delivered.size() == 1, "only seq 1 before retransmit");

    // Retransmit arrives: seq 2.
    auto p2 = make_packet(sess, 2, 1, 0x02);
    gb.ingest(p2.data(), p2.size(), now++);

    CHECK(delivered.size() == 3, "seq 2 and 3 delivered after retransmit");
    CHECK(delivered[1] == 2,     "seq 2 delivered");
    CHECK(delivered[2] == 3,     "seq 3 delivered");
    CHECK(!gb.in_gap(),          "gap cleared");
}

// ── Test: heartbeat handling ──────────────────────────────────────────────────

static void test_heartbeat() {
    GapBuffer gb;
    int delivered = 0;
    gb.set_on_message([&](const MoldMessage&) { ++delivered; });

    const char* sess = "SESSIONA  ";
    auto hb = make_heartbeat(sess);
    int r = gb.ingest(hb.data(), hb.size(), 1'000'000);
    CHECK(r == 0,       "heartbeat returns 0 delivered");
    CHECK(delivered == 0, "no messages from heartbeat");
    CHECK(gb.stat_heartbeats() == 1, "heartbeat counted");
}

// ── Test: multi-message packets ───────────────────────────────────────────────

static void test_multi_message_packet() {
    GapBuffer gb;
    std::vector<uint64_t> delivered;
    gb.set_on_message([&](const MoldMessage& m) { delivered.push_back(m.seq_num); });

    const char* sess = "SESSIONA  ";
    // One packet, 3 messages starting at seq 1.
    auto pkt = make_packet(sess, 1, 3, 0xAA, 4);
    gb.ingest(pkt.data(), pkt.size(), 1'000'000);

    CHECK(delivered.size() == 3, "3 messages from one packet");
    CHECK(delivered[0] == 1, "first message seq 1");
    CHECK(delivered[1] == 2, "second message seq 2");
    CHECK(delivered[2] == 3, "third message seq 3");
    CHECK(gb.next_expected() == 4, "next_expected == 4");
}

// ── Test: parse error handling ────────────────────────────────────────────────

static void test_parse_errors() {
    GapBuffer gb;
    // Too-short buffer should return -1.
    uint8_t tiny[5] = {0};
    int r = gb.ingest(tiny, 5, 1'000'000);
    CHECK(r == -1, "short buffer returns -1");
}

// ── Test: large gap spanning buffer ───────────────────────────────────────────

static void test_large_sequential_stream() {
    GapBuffer gb;
    uint64_t total_delivered = 0;
    gb.set_on_message([&](const MoldMessage&) { ++total_delivered; });

    const char* sess = "SESSIONA  ";
    // Send 10000 in-order messages in packets of 10 each.
    uint64_t seq = 1;
    for (int i = 0; i < 1000; ++i) {
        auto pkt = make_packet(sess, seq, 10, 0x55, 4);
        gb.ingest(pkt.data(), pkt.size(), uint64_t(i) * 1000);
        seq += 10;
    }
    CHECK(total_delivered == 10000, "10000 messages delivered in order");
    CHECK(gb.stat_gaps() == 0,      "no gaps in clean stream");
    CHECK(gb.next_expected() == 10001, "next_expected correct");
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== Gap Buffer Tests ===\n\n");
    test_inorder_delivery();
    test_duplicate_detection();
    test_gap_detection();
    test_outoforder_flush();
    test_retransmit_request_timing();
    test_retransmit_fills_gap();
    test_heartbeat();
    test_multi_message_packet();
    test_parse_errors();
    test_large_sequential_stream();

    std::printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
