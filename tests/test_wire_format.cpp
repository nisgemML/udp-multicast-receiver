// test_wire_format.cpp — Unit tests for MoldUDP64 and ITCH wire format parsing.

#include "feed/wire_format.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace feed;

static int passed = 0, failed = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++failed; } else { ++passed; } \
    } while(0)

// ── Byte-order helpers ────────────────────────────────────────────────────────

static void test_be_helpers() {
    uint8_t buf[8];

    // be16
    buf[0] = 0x12; buf[1] = 0x34;
    CHECK(be16(buf) == 0x1234, "be16");

    // be32
    buf[0]=0x12; buf[1]=0x34; buf[2]=0x56; buf[3]=0x78;
    CHECK(be32(buf) == 0x12345678u, "be32");

    // be64
    buf[0]=0x01; buf[1]=0x02; buf[2]=0x03; buf[3]=0x04;
    buf[4]=0x05; buf[5]=0x06; buf[6]=0x07; buf[7]=0x08;
    CHECK(be64(buf) == 0x0102030405060708ULL, "be64");

    // be48
    uint8_t ts[6] = {0x00, 0x00, 0x01, 0x83, 0x5A, 0x12};
    uint64_t v = be48(ts);
    CHECK(v == 0x0000'0183'5A12ULL, "be48");

    // put_be16
    uint8_t out[2];
    put_be16(out, 0xABCD);
    CHECK(out[0] == 0xAB && out[1] == 0xCD, "put_be16");

    // put_be64
    uint8_t out8[8];
    put_be64(out8, 0x0102030405060708ULL);
    CHECK(out8[0]==0x01 && out8[7]==0x08, "put_be64");
}

// ── MoldHeader parse ──────────────────────────────────────────────────────────

static void test_mold_header_parse() {
    // Build a valid MoldUDP64 header manually.
    uint8_t buf[20];
    std::memcpy(buf, "SESSIONA  ", 10);  // session
    put_be64(buf + 10, 42ULL);           // seq_num = 42
    put_be16(buf + 18, 3);              // msg_count = 3

    MoldHeader hdr;
    CHECK(hdr.parse(buf, 20), "parse succeeds");
    CHECK(hdr.seq_num == 42, "seq_num == 42");
    CHECK(hdr.msg_count == 3, "msg_count == 3");
    CHECK(!hdr.is_heartbeat(), "not heartbeat");
    CHECK(hdr.session_view() == "SESSIONA  ", "session");

    // Too short — should fail.
    CHECK(!hdr.parse(buf, 19), "parse fails on short buffer");
}

static void test_mold_header_heartbeat() {
    uint8_t buf[20];
    std::memcpy(buf, "SESSIONA  ", 10);
    put_be64(buf + 10, 0ULL);
    put_be16(buf + 18, 0);    // heartbeat

    MoldHeader hdr;
    (void)hdr.parse(buf, 20);
    CHECK(hdr.is_heartbeat(), "heartbeat detected");
    CHECK(hdr.seq_num == 0, "heartbeat seq_num == 0");
}

// ── MoldHeader serialise round-trip ──────────────────────────────────────────

static void test_mold_header_round_trip() {
    MoldHeader orig;
    std::memcpy(orig.session, "TESTTEST  ", 10);
    orig.seq_num   = 999999ULL;
    orig.msg_count = 7;

    uint8_t buf[20];
    orig.serialise(buf);

    MoldHeader parsed;
    (void)parsed.parse(buf, 20);

    CHECK(parsed.seq_num   == orig.seq_num,   "seq_num round-trip");
    CHECK(parsed.msg_count == orig.msg_count, "msg_count round-trip");
    CHECK(std::memcmp(parsed.session, orig.session, 10) == 0, "session round-trip");
}

// ── ItchAddOrder parse ────────────────────────────────────────────────────────

// ── ITCH 5.0 golden vectors ──────────────────────────────────────────────────
//
// Every byte below is typed by hand from the NASDAQ TotalView-ITCH 5.0
// message-format tables — deliberately NOT built with put_be*/any encoder in
// this repo. A test whose fixture comes from the same code as the parser can
// only prove the two agree with each other; this repo once shipped an Add
// Order layout missing Stock Locate/Tracking Number that passed every test
// for exactly that reason. These vectors pin the layout to the spec.
//
// Common prefix: type | locate(2) | tracking(2) | timestamp(6), fields at 11.
// Values used throughout:
//   locate    = 0x002A (42)
//   tracking  = 0x0007
//   timestamp = 0x1F1ACED9F000 = 34,200,000,000,000 ns = 09:30:00.000

static constexpr uint64_t kGoldenTs = 34'200'000'000'000ULL;

static void test_itch_add_order_golden() {
    const uint8_t a[36] = {
        'A',
        0x00, 0x2A,                                     // 1  stock locate = 42
        0x00, 0x07,                                     // 3  tracking
        0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00,             // 5  timestamp
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x39, // 11 order ref = 12345
        'B',                                            // 19 side
        0x00, 0x00, 0x00, 0x64,                         // 20 shares = 100
        'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',         // 24 stock
        0x00, 0x16, 0xE3, 0x60,                         // 32 price = 1,500,000 ($150.0000)
    };
    ItchAddOrder ao;
    CHECK(ItchAddOrder::parse(a, sizeof(a), ao), "A: parse succeeds");
    CHECK(ao.stock_locate == 42,           "A: stock locate @1");
    CHECK(ao.timestamp_ns == kGoldenTs,    "A: timestamp @5");
    CHECK(itch_timestamp_ns(a) == kGoldenTs, "A: itch_timestamp_ns helper");
    CHECK(ao.order_ref    == 12345ULL,     "A: order ref @11");
    CHECK(ao.side         == 'B',          "A: side @19");
    CHECK(ao.shares       == 100u,         "A: shares @20");
    CHECK(std::memcmp(ao.stock, "AAPL    ", 8) == 0, "A: stock @24");
    CHECK(ao.price        == 1'500'000u,   "A: price @32");
    CHECK(!ao.has_mpid,                    "A: no MPID for 'A'");
    CHECK(!ItchAddOrder::parse(a, 35, ao), "A: rejects short buffer");
}

static void test_itch_delete_golden() {
    const uint8_t d[19] = {
        'D', 0x00, 0x2A, 0x00, 0x07,
        0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x39, // 11 order ref
    };
    ItchDeleteOrder m;
    CHECK(ItchDeleteOrder::parse(d, sizeof(d), m), "D: parse succeeds");
    CHECK(m.timestamp_ns == kGoldenTs,  "D: timestamp @5");
    CHECK(m.order_ref    == 12345ULL,   "D: order ref @11");
    CHECK(!ItchDeleteOrder::parse(d, 18, m), "D: rejects short buffer");
}

static void test_itch_cancel_golden() {
    const uint8_t x[23] = {
        'X', 0x00, 0x2A, 0x00, 0x07,
        0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x39, // 11 order ref
        0x00, 0x00, 0x00, 0x28,                         // 19 cancelled = 40
    };
    ItchOrderCancel m;
    CHECK(ItchOrderCancel::parse(x, sizeof(x), m), "X: parse succeeds");
    CHECK(m.timestamp_ns     == kGoldenTs, "X: timestamp @5");
    CHECK(m.order_ref        == 12345ULL,  "X: order ref @11");
    CHECK(m.cancelled_shares == 40u,       "X: cancelled shares @19");
    CHECK(!ItchOrderCancel::parse(x, 22, m), "X: rejects short buffer");
}

static void test_itch_executed_golden() {
    const uint8_t e[31] = {
        'E', 0x00, 0x2A, 0x00, 0x07,
        0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x39, // 11 order ref
        0x00, 0x00, 0x00, 0x1E,                         // 19 executed = 30
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x86, 0xA0, // 23 match = 100000
    };
    ItchOrderExecuted m;
    CHECK(ItchOrderExecuted::parse(e, sizeof(e), m), "E: parse succeeds");
    CHECK(m.timestamp_ns    == kGoldenTs, "E: timestamp @5");
    CHECK(m.order_ref       == 12345ULL,  "E: order ref @11");
    CHECK(m.executed_shares == 30u,       "E: executed shares @19");
    CHECK(m.match_number    == 100000ULL, "E: match number @23");
    CHECK(!ItchOrderExecuted::parse(e, 30, m), "E: rejects short buffer");
}

static void test_itch_replace_golden() {
    const uint8_t u[35] = {
        'U', 0x00, 0x2A, 0x00, 0x07,
        0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x39, // 11 orig ref = 12345
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x3A, // 19 new ref  = 12346
        0x00, 0x00, 0x00, 0x96,                         // 27 shares = 150
        0x00, 0x2D, 0xC8, 0x34,                         // 31 price  = 3,000,372
    };
    ItchOrderReplace m;
    CHECK(ItchOrderReplace::parse(u, sizeof(u), m), "U: parse succeeds");
    CHECK(m.timestamp_ns   == kGoldenTs,   "U: timestamp @5");
    CHECK(m.orig_order_ref == 12345ULL,    "U: orig ref @11");
    CHECK(m.new_order_ref  == 12346ULL,    "U: new ref @19");
    CHECK(m.shares         == 150u,        "U: shares @27");
    CHECK(m.price          == 3'000'372u,  "U: price @31");
    CHECK(!ItchOrderReplace::parse(u, 34, m), "U: rejects short buffer");
}

// ── RetransmitRequest serialise ───────────────────────────────────────────────

static void test_retransmit_request() {
    RetransmitRequest req;
    std::memcpy(req.session, "SESSION1  ", 10);
    req.first_seq = 1001ULL;
    req.count     = 50;

    uint8_t buf[20];
    req.serialise(buf);

    CHECK(std::memcmp(buf, "SESSION1  ", 10) == 0, "session serialised");
    CHECK(be64(buf + 10) == 1001ULL, "first_seq serialised");
    CHECK(be16(buf + 18) == 50,      "count serialised");
    CHECK(RetransmitRequest::kSize == 20, "size constant");
}

// ── PCAP header layout ────────────────────────────────────────────────────────

static void test_pcap_headers() {
    PcapGlobalHeader gh{};
    CHECK(gh.magic_number  == kPcapMagicLE, "pcap magic");
    CHECK(gh.version_major == 2,            "pcap version major");
    CHECK(gh.version_minor == 4,            "pcap version minor");
    CHECK(gh.network       == 1,            "LINKTYPE_ETHERNET");
    CHECK(sizeof(PcapGlobalHeader) == 24,   "global header size");
    CHECK(sizeof(PcapRecordHeader) == 16,   "record header size");
}

// ── itch_type helper ──────────────────────────────────────────────────────────

static void test_itch_type_helper() {
    uint8_t body_a = uint8_t('A');
    CHECK(itch_type(&body_a) == ItchMsgType::AddOrderNoMpid, "type helper 'A'");

    uint8_t body_d = uint8_t('D');
    CHECK(itch_type(&body_d) == ItchMsgType::OrderDelete, "type helper 'D'");
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    std::printf("=== Wire Format Tests ===\n\n");
    test_be_helpers();
    test_mold_header_parse();
    test_mold_header_heartbeat();
    test_mold_header_round_trip();
    test_itch_add_order_golden();
    test_itch_delete_golden();
    test_itch_cancel_golden();
    test_itch_executed_golden();
    test_itch_replace_golden();
    test_retransmit_request();
    test_pcap_headers();
    test_itch_type_helper();

    std::printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
