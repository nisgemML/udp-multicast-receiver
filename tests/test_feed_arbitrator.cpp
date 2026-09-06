// test_feed_arbitrator.cpp — Tests for A/B feed line arbitration.
//
// Covers:
//   1. Both lines fully present, identical: no duplicate delivery, both
//      lines get "redundant" credit for the losing copy.
//   2. Line A drops a range that line B has: delivered with zero gaps,
//      zero retransmit requests — the whole point of A/B feeds.
//   3. Both lines drop the same range: a real gap fires and a retransmit
//      is requested after the timeout, same as the single-line case.
//   4. Per-line win-share and dark-line detection reflect an asymmetric
//      outage correctly.

#include "feed/feed_arbitrator.hpp"
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

// Same layout as tests/test_gap_buffer.cpp's make_packet — one dummy
// message per packet, content doesn't matter to GapBuffer/FeedArbitrator.
static std::vector<uint8_t> make_packet(const char* session, uint64_t seq,
                                         uint8_t msg_body_byte, uint16_t msg_len = 4) {
    const std::size_t total = kMoldHeaderSize + (2 + msg_len);
    std::vector<uint8_t> buf(total, 0);
    MoldHeader hdr{};
    std::memcpy(hdr.session, session, 10);
    hdr.seq_num   = seq;
    hdr.msg_count = 1;
    hdr.serialise(buf.data());
    put_be16(buf.data() + kMoldHeaderSize, msg_len);
    std::memset(buf.data() + kMoldHeaderSize + 2, msg_body_byte, msg_len);
    return buf;
}

static GapBuffer::Config make_cfg() {
    GapBuffer::Config cfg;
    cfg.gap_timeout_ns    = 1000;   // tight timeout for fast tests
    cfg.retry_interval_ns = 2000;
    std::memcpy(cfg.session, "ABFEEDSES ", 10);
    return cfg;
}

static void test_identical_streams_no_duplicate_delivery() {
    GapBuffer gb(make_cfg());
    FeedArbitrator arb(gb);
    int delivered = 0;
    gb.set_on_message([&](const MoldMessage&) { ++delivered; });

    uint64_t ts = 1000;
    for (uint64_t seq = 1; seq <= 10; ++seq) {
        auto pkt = make_packet("ABFEEDSES ", seq, uint8_t(seq));
        arb.ingest_a(pkt.data(), pkt.size(), ts++);
        arb.ingest_b(pkt.data(), pkt.size(), ts++); // same seq, arrives just after A
    }

    CHECK(delivered == 10, "10 unique messages delivered despite both lines sending everything");
    CHECK(arb.stat_seq_won(FeedLine::A) == 10, "line A always arrives first in this test");
    CHECK(arb.stat_redundant(FeedLine::B) == 10, "line B's copies are all redundant duplicates");
    CHECK(arb.a_win_share() == 1.0, "A supplied 100% of the winning copies");
}

static void test_line_a_gap_covered_by_line_b_no_stall() {
    GapBuffer gb(make_cfg());
    FeedArbitrator arb(gb);
    std::vector<uint64_t> delivered_seqs;
    int gaps_fired = 0;
    int retx_fired = 0;
    gb.set_on_message([&](const MoldMessage& m) { delivered_seqs.push_back(m.seq_num); });
    gb.set_on_gap([&](const GapEvent&) { ++gaps_fired; });
    gb.set_on_retransmit([&](const RetransmitRequest&) { ++retx_fired; });

    uint64_t ts = 1000;
    for (uint64_t seq = 1; seq <= 20; ++seq) {
        auto pkt = make_packet("ABFEEDSES ", seq, uint8_t(seq));
        // Line A drops sequences 5..9. Line B has everything.
        const bool a_has_it = !(seq >= 5 && seq <= 9);
        if (a_has_it) arb.ingest_a(pkt.data(), pkt.size(), ts);
        arb.ingest_b(pkt.data(), pkt.size(), ts);
        ts += 10;
        arb.tick(ts);
    }

    CHECK(delivered_seqs.size() == 20, "all 20 messages delivered via line B covering A's drops");
    CHECK(gaps_fired == 0, "no gap ever fires — line B always had the missing sequence in time");
    CHECK(retx_fired == 0, "no retransmit requested — this is the entire value of A/B feeds");
    CHECK(arb.stat_seq_won(FeedLine::B) >= 5, "line B is credited with winning the seqs A dropped");
}

static void test_both_lines_drop_same_range_real_gap() {
    GapBuffer gb(make_cfg());
    FeedArbitrator arb(gb);
    int gaps_fired = 0;
    int retx_fired = 0;
    uint64_t retx_first_seq = 0, retx_count = 0;
    gb.set_on_gap([&](const GapEvent&) { ++gaps_fired; });
    gb.set_on_retransmit([&](const RetransmitRequest& r) {
        ++retx_fired; retx_first_seq = r.first_seq; retx_count = r.count;
    });

    uint64_t ts = 1000;
    for (uint64_t seq = 1; seq <= 20; ++seq) {
        auto pkt = make_packet("ABFEEDSES ", seq, uint8_t(seq));
        // Both lines miss 8..10 — a genuine outage no arbitration can hide.
        const bool have_it = !(seq >= 8 && seq <= 10);
        if (have_it) {
            arb.ingest_a(pkt.data(), pkt.size(), ts);
            arb.ingest_b(pkt.data(), pkt.size(), ts);
        }
        ts += 200; // exceed gap_timeout_ns (1000ns) after a few iterations
        arb.tick(ts);
    }

    CHECK(gaps_fired == 1, "exactly one real gap fires when both lines are missing the same range");
    CHECK(retx_fired >= 1, "retransmit requested when neither line covers the gap in time");
    CHECK(retx_first_seq == 8, "retransmit request starts at the first truly-missing sequence");
    CHECK(retx_count == 3, "retransmit request covers exactly the 3 missing sequences");
}

static void test_dark_line_detection() {
    GapBuffer gb(make_cfg());
    FeedArbitrator arb(gb);

    uint64_t ts = 1000;
    // Line A active for a while...
    for (uint64_t seq = 1; seq <= 5; ++seq) {
        auto pkt = make_packet("ABFEEDSES ", seq, uint8_t(seq));
        arb.ingest_a(pkt.data(), pkt.size(), ts);
        ts += 100;
    }
    CHECK(!arb.line_looks_dark(FeedLine::A, ts, 5'000'000), "line A recently won, not dark yet");

    // ...then goes silent while line B keeps the stream moving.
    uint64_t last_b_ts = ts;
    for (uint64_t seq = 6; seq <= 10; ++seq) {
        auto pkt = make_packet("ABFEEDSES ", seq, uint8_t(seq));
        arb.ingest_b(pkt.data(), pkt.size(), ts);
        last_b_ts = ts;
        ts += 10'000'000; // 10ms between B packets, well past the 5ms staleness bar
    }

    CHECK(arb.line_looks_dark(FeedLine::A, ts, 5'000'000),
          "line A hasn't won a sequence in >5ms while B is active — flagged dark");
    CHECK(!arb.line_looks_dark(FeedLine::B, last_b_ts, 5'000'000), "line B just won, not dark");
}

// Kept as a separate test function (rather than adding a second GapBuffer
// inside test_dark_line_detection above): GapBuffer embeds its 4096-slot
// out-of-order buffer directly as a stack member (~6.3MB, by design — no
// heap allocation on the hot path), and every test in this repo constructs
// exactly one GapBuffer per function to stay clear of the 8MB default stack
// limit. Two in one frame reliably stack-overflows.
static void test_never_connected_line_is_not_reported_dark() {
    GapBuffer gb(make_cfg());
    FeedArbitrator arb(gb);
    CHECK(!arb.line_looks_dark(FeedLine::A, 999'999'999, 5'000'000),
          "a line that never sent anything is not reported as 'dark'");
}

int main() {
    test_identical_streams_no_duplicate_delivery();
    test_line_a_gap_covered_by_line_b_no_stall();
    test_both_lines_drop_same_range_real_gap();
    test_dark_line_detection();
    test_never_connected_line_is_not_reported_dark();

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
