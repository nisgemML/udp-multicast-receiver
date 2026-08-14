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
    hdr.parse(buf, 20);
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
    parsed.parse(buf, 20);

    CHECK(parsed.seq_num   == orig.seq_num,   "seq_num round-trip");
    CHECK(parsed.msg_count == orig.msg_count, "msg_count round-trip");
    CHECK(std::memcmp(parsed.session, orig.session, 10) == 0, "session round-trip");
}

// ── ItchAddOrder parse ────────────────────────────────────────────────────────

static void test_itch_add_order_parse() {
    // Construct a synthetic 'A' message body (36 bytes minimum).
    uint8_t body[36] = {};
    body[0] = uint8_t('A');          // type

    // Timestamp: 6 bytes BE, value = 34200000000000 (9:30:00 in ns)
    const uint64_t ts = 34200000000000ULL;
    body[1] = uint8_t(ts >> 40);
    body[2] = uint8_t(ts >> 32);
    body[3] = uint8_t(ts >> 24);
    body[4] = uint8_t(ts >> 16);
    body[5] = uint8_t(ts >>  8);
    body[6] = uint8_t(ts);

    // order_ref = 12345 (8 bytes BE at offset 7)
    put_be64(body + 7, 12345ULL);

    // side = 'B'
    body[15] = 'B';

    // shares = 100 (4 bytes BE at offset 16)
    put_be32(body + 16, 100u);

    // stock = "AAPL    " (8 bytes at offset 20)
    std::memcpy(body + 20, "AAPL    ", 8);

    // price = 1500000 = $150.0000 (4 bytes BE at offset 28)
    put_be32(body + 28, 1'500'000u);

    ItchAddOrder ao;
    CHECK(ItchAddOrder::parse(body, 36, ao), "parse succeeds");
    CHECK(ao.timestamp_ns == ts,          "timestamp_ns");
    CHECK(ao.order_ref    == 12345ULL,    "order_ref");
    CHECK(ao.side         == 'B',         "side");
    CHECK(ao.shares       == 100u,        "shares");
    CHECK(ao.price        == 1'500'000u,  "price");
    CHECK(std::memcmp(ao.stock, "AAPL    ", 8) == 0, "stock");
    CHECK(!ao.has_mpid,                   "no MPID for 'A' type");

    // Too short — should fail.
    CHECK(!ItchAddOrder::parse(body, 35, ao), "parse fails on short buffer");
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
    test_itch_add_order_parse();
    test_retransmit_request();
    test_pcap_headers();
    test_itch_type_helper();

    std::printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
