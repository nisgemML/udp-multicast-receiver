// tick_to_trade_bench.cpp — measures software tick-to-trade latency:
// GapBuffer::ingest() → MultiSymbolBook → DecisionEngine, end to end.
//
// ── What this measures, and what it deliberately does not ────────────────────
//
// Every synthetic packet is stamped with the CURRENT CLOCK_MONOTONIC value
// immediately before being handed to GapBuffer::ingest(). That timestamp is
// threaded, unchanged, through GapBuffer → MoldMessage::recv_ns →
// MultiSymbolBook → DecisionEngine, which records (decided_ns - recv_ns)
// for every decision it emits. Because both ends of that delta come from
// the same clock read moments apart in the same process, the resulting
// histogram is a real, reproducible measurement of this machine's software
// path: byte-parse → book update → decision rule.
//
// It is NOT a co-location tick-to-trade number. It does not touch a NIC,
// does not include network wire transit, and is not comparable to a
// published exchange-to-strategy latency figure. See
// include/feed/decision_engine.hpp for why PCAP-file timestamps are
// deliberately not used for this measurement (they're original capture
// time, not "now" — subtracting them from a live clock read would produce
// a meaningless number that happens to look like a latency figure).
//
// Usage:
//   ./tick_to_trade_bench [--messages N] [--symbols K] [--warmup N]

#include "feed/wire_format.hpp"
#include "feed/gap_buffer.hpp"
#include "feed/order_book.hpp"
#include "feed/decision_engine.hpp"
#include "feed/stats.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <random>

using namespace feed;

namespace {

struct Options {
    uint64_t messages = 200'000;
    uint32_t symbols  = 8;
    uint64_t warmup   = 20'000;
};

Options parse_args(int argc, char* argv[]) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--messages") && i + 1 < argc) o.messages = std::stoull(argv[++i]);
        else if (!std::strcmp(argv[i], "--symbols") && i + 1 < argc) o.symbols = uint32_t(std::stoul(argv[++i]));
        else if (!std::strcmp(argv[i], "--warmup") && i + 1 < argc) o.warmup = std::stoull(argv[++i]);
    }
    return o;
}

// Build a MoldUDP64 packet carrying exactly one ITCH message body.
std::vector<uint8_t> mold_packet(const char* session, uint64_t seq,
                                  const std::vector<uint8_t>& body) {
    std::vector<uint8_t> buf(kMoldHeaderSize + 2 + body.size());
    MoldHeader hdr{};
    std::memcpy(hdr.session, session, 10);
    hdr.seq_num   = seq;
    hdr.msg_count = 1;
    hdr.serialise(buf.data());
    put_be16(buf.data() + kMoldHeaderSize, uint16_t(body.size()));
    std::memcpy(buf.data() + kMoldHeaderSize + 2, body.data(), body.size());
    return buf;
}

std::vector<uint8_t> make_add(uint64_t order_ref, char side, uint32_t shares,
                               const std::string& stock8, uint32_t price) {
    std::vector<uint8_t> b(36, 0);
    b[0] = 'A';
    put_be64(b.data() + 7, order_ref);
    b[15] = uint8_t(side);
    put_be32(b.data() + 16, shares);
    std::memcpy(b.data() + 20, stock8.data(), 8);
    put_be32(b.data() + 28, price);
    return b;
}

std::vector<uint8_t> make_cancel(uint64_t order_ref, uint32_t cancelled_shares) {
    std::vector<uint8_t> b(23, 0);
    b[0] = 'X';
    put_be64(b.data() + 11, order_ref);
    put_be32(b.data() + 19, cancelled_shares);
    return b;
}

std::vector<uint8_t> make_delete(uint64_t order_ref) {
    std::vector<uint8_t> b(19, 0);
    b[0] = 'D';
    put_be64(b.data() + 11, order_ref);
    return b;
}

std::string pad_symbol(const std::string& s) {
    std::string padded = s;
    while (padded.size() < 8) padded += ' ';
    return padded;
}

} // namespace

int main(int argc, char* argv[]) {
    const Options opt = parse_args(argc, argv);

    std::vector<std::string> symbols;
    for (uint32_t i = 0; i < opt.symbols; ++i)
        symbols.push_back(pad_symbol("SYM" + std::to_string(i)));

    GapBuffer::Config gcfg;
    std::memcpy(gcfg.session, "T2TBENCH  ", 10);
    GapBuffer       gap_buf(gcfg);
    MultiSymbolBook book;
    DecisionEngine  engine;

    gap_buf.set_on_message([&](const MoldMessage& m) { book.ingest(m); });
    book.set_on_top_change([&](std::string_view sym, const TopOfBook& t, uint64_t recv_ns) {
        engine.on_top_of_book(sym, t, recv_ns);
    });

    // Deterministic PRNG so the run is reproducible; not used for timing.
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint32_t> sym_dist(0, opt.symbols - 1);
    std::uniform_int_distribution<int>      op_dist(0, 9); // 0-5 add, 6-7 cancel, 8-9 delete
    std::uniform_int_distribution<uint32_t> price_jitter(0, 50);

    std::vector<uint64_t> live_orders; // order_refs currently resting, for cancel/delete targets
    live_orders.reserve(opt.messages);

    LatencyHistogram parse_latency; // pure GapBuffer::ingest() cost, for comparison

    uint64_t seq = 1;
    uint64_t next_order_ref = 1;
    const uint64_t total = opt.warmup + opt.messages;

    for (uint64_t i = 0; i < total; ++i) {
        const bool warming = i < opt.warmup;
        const uint32_t sym_idx = sym_dist(rng);
        const std::string& sym = symbols[sym_idx];
        const int op = op_dist(rng);
        // Each symbol has its own well-separated mid price. Bids sit
        // 150-200 ticks below the mid, asks 150-200 above — a consistent
        // ~300-400 tick spread so the decision rule's 2-tick filter fires
        // regularly instead of only on rare random crosses, giving the
        // latency histogram a meaningful sample count.
        const uint32_t mid = 1'000'000 + sym_idx * 50'000;

        std::vector<uint8_t> body;
        if (op <= 5 || live_orders.empty()) {
            const char side = (op % 2 == 0) ? 'B' : 'S';
            const uint32_t half_spread = (150 + price_jitter(rng)) * 100;
            const uint32_t price = (side == 'B') ? (mid - half_spread) : (mid + half_spread);
            body = make_add(next_order_ref, side, 100, sym, price);
            live_orders.push_back(next_order_ref);
            ++next_order_ref;
        } else if (op <= 7) {
            const uint64_t ref = live_orders[rng() % live_orders.size()];
            body = make_cancel(ref, 30);
        } else {
            const std::size_t idx = rng() % live_orders.size();
            body = make_delete(live_orders[idx]);
            live_orders.erase(live_orders.begin() + idx);
        }

        auto pkt = mold_packet("T2TBENCH  ", seq++, body);

        const uint64_t t0 = DecisionEngine::monotonic_ns();
        gap_buf.ingest(pkt.data(), pkt.size(), t0);
        const uint64_t t1 = DecisionEngine::monotonic_ns();

        if (!warming) parse_latency.record(t1 - t0);
    }

    std::printf("=== Tick-to-Trade Software Latency Benchmark ===\n");
    std::printf("Messages          : %lu (+ %lu warmup)\n", opt.messages, opt.warmup);
    std::printf("Symbols            : %u\n", opt.symbols);
    std::printf("Symbols in book    : %zu\n", book.symbol_count());
    std::printf("Top-of-book events : %lu\n", engine.top_changes_seen());
    std::printf("Decisions emitted  : %lu\n", engine.decisions_emitted());
    std::printf("\n");
    parse_latency.print("GapBuffer.ingest() only");
    std::printf("\n");
    engine.latency().print("recv -> decision (full pipeline)");
    std::printf("\n");
    std::printf("NOTE: 'recv -> decision' is a software-only pipeline latency\n");
    std::printf("(parse + book update + decision rule) measured in-process on\n");
    std::printf("this machine. It excludes NIC/network wire transit and is not\n");
    std::printf("a co-location tick-to-trade figure.\n");

    return 0;
}
