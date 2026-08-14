// gen_pcap.cpp — Synthetic MoldUDP64 PCAP generator.
//
// Generates a .pcap file for testing the receiver without a live feed.
//
// Usage:
//   ./gen_pcap [options] output.pcap
//
//   --count N         Total number of messages (default: 10000)
//   --gap-at S        Introduce a gap starting at sequence S (default: none)
//   --gap-len L       Length of the gap in messages (default: 5)
//   --msgs-per-pkt M  Messages per packet (default: 1)
//   --dup-at S        Insert a duplicate packet at sequence S
//   --rate-us R       Inter-packet delay in microseconds (default: 100)

#include "feed/wire_format.hpp"
#include "feed/pcap_replay.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace feed;

struct Options {
    std::string output       = "synthetic.pcap";
    uint64_t    count        = 10000;
    uint64_t    gap_at       = 0;
    uint64_t    gap_len      = 5;
    uint16_t    msgs_per_pkt = 1;
    uint64_t    dup_at       = 0;
    uint64_t    rate_us      = 100;
};

static Options parse_args(int argc, char* argv[]) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--count")        && i+1 < argc) opt.count        = std::stoull(argv[++i]);
        else if (!std::strcmp(argv[i], "--gap-at")  && i+1 < argc) opt.gap_at       = std::stoull(argv[++i]);
        else if (!std::strcmp(argv[i], "--gap-len") && i+1 < argc) opt.gap_len      = std::stoull(argv[++i]);
        else if (!std::strcmp(argv[i], "--msgs-per-pkt") && i+1 < argc) opt.msgs_per_pkt = uint16_t(std::stoul(argv[++i]));
        else if (!std::strcmp(argv[i], "--dup-at")  && i+1 < argc) opt.dup_at       = std::stoull(argv[++i]);
        else if (!std::strcmp(argv[i], "--rate-us") && i+1 < argc) opt.rate_us      = std::stoull(argv[++i]);
        else if (argv[i][0] != '-') opt.output = argv[i];
    }
    return opt;
}

// Append big-endian uint16 to a byte vector.
static void append_be16(std::vector<uint8_t>& v, uint16_t val) {
    uint8_t buf[2]; put_be16(buf, val);
    v.push_back(buf[0]); v.push_back(buf[1]);
}

// Build a synthetic ITCH 'A' message body (36 bytes).
static std::vector<uint8_t> make_add_order(uint64_t order_ref) {
    std::vector<uint8_t> body(36, 0);
    body[0] = uint8_t('A');
    put_be64(body.data() + 7, order_ref);
    body[15] = 'B';
    put_be32(body.data() + 16, 100u);
    std::memcpy(body.data() + 20, "AAPL    ", 8);
    put_be32(body.data() + 28, 1'500'000u);
    return body;
}

int main(int argc, char* argv[]) {
    const Options opt = parse_args(argc, argv);

    PcapWriter writer(opt.output);
    if (!writer.is_open()) {
        std::fprintf(stderr, "Cannot open: %s\n", opt.output.c_str());
        return 1;
    }

    const char* session = "SYNTHETIC ";
    uint64_t seq = 1;
    uint64_t ts_us = 1'700'000'000ULL * 1'000'000ULL;
    uint64_t msgs_written = 0;

    std::printf("Generating %s: %lu messages, %u msg/pkt",
                opt.output.c_str(), opt.count, opt.msgs_per_pkt);
    if (opt.gap_at) std::printf(", gap at seq %lu (len %lu)", opt.gap_at, opt.gap_len);
    if (opt.dup_at) std::printf(", dup at seq %lu", opt.dup_at);
    std::printf("\n");

    while (msgs_written < opt.count) {
        // Skip sequences in the gap.
        if (opt.gap_at && seq >= opt.gap_at && seq < opt.gap_at + opt.gap_len)
            seq += opt.gap_len;

        const uint16_t n = uint16_t(
            std::min<uint64_t>(opt.msgs_per_pkt, opt.count - msgs_written));

        // Build message payload: [length(2) + body] * n
        std::vector<uint8_t> msg_payload;
        for (uint16_t i = 0; i < n; ++i) {
            auto body = make_add_order(seq + i);
            append_be16(msg_payload, uint16_t(body.size()));
            msg_payload.insert(msg_payload.end(), body.begin(), body.end());
        }

        // Build MoldUDP64 packet: header(20) + messages
        std::vector<uint8_t> payload(kMoldHeaderSize + msg_payload.size());
        MoldHeader hdr{};
        std::memcpy(hdr.session, session, 10);
        hdr.seq_num   = seq;
        hdr.msg_count = n;
        hdr.serialise(payload.data());
        std::memcpy(payload.data() + kMoldHeaderSize,
                    msg_payload.data(), msg_payload.size());

        writer.write_packet(ts_us, payload.data(), uint16_t(payload.size()));
        ts_us += opt.rate_us;
        msgs_written += n;

        // Duplicate: write same packet again.
        if (opt.dup_at && seq == opt.dup_at) {
            writer.write_packet(ts_us, payload.data(), uint16_t(payload.size()));
            ts_us += opt.rate_us;
        }

        seq += n;
    }

    std::printf("Done: %lu packets → %s\n", writer.packets_written(), opt.output.c_str());
    return 0;
}
