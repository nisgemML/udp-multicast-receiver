// main.cpp — UDP multicast receiver demo.
//
// Demonstrates the full pipeline:
//   MulticastReceiver → GapBuffer → ITCH message handler
//
// In a real system the message handler would forward to an order book or
// risk engine over an SPSC queue.  Here we just count and print statistics.
//
// Usage:
//   ./receiver [--group 239.1.1.1] [--port 15001] [--iface eth0]
//              [--replay file.pcap]  # PCAP replay mode (no live socket)

#include "feed/wire_format.hpp"
#include "feed/gap_buffer.hpp"
#include "feed/receiver.hpp"
#include "feed/pcap_replay.hpp"
#include "feed/stats.hpp"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <csignal>
#include <string>
#include <chrono>

using namespace feed;

static std::atomic<bool> g_running{true};

extern "C" void handle_signal(int) {
    g_running.store(false, std::memory_order_release);
}

// ── ITCH message handler ──────────────────────────────────────────────────────

struct Handler {
    uint64_t          total_messages = 0;
    uint64_t          add_orders     = 0;
    uint64_t          delete_orders  = 0;
    uint64_t          executions     = 0;
    LatencyHistogram  latency;
    ThroughputTracker throughput;

    void on_message(const MoldMessage& msg) {
        ++total_messages;

        const auto type = itch_type(msg.body);
        switch (type) {
            case ItchMsgType::AddOrderNoMpid:
            case ItchMsgType::AddOrderMpid:
                ++add_orders;
                break;
            case ItchMsgType::OrderDelete:
                ++delete_orders;
                break;
            case ItchMsgType::OrderExecuted:
            case ItchMsgType::OrderExecutedPrice:
                ++executions;
                break;
            default:
                break;
        }

        // Latency = kernel_rx_ts - ITCH_timestamp.
        // (kernel timestamp is embedded in the MoldMessage by the receiver;
        //  here we use the seq_num as a proxy since ITCH ts is in message body.)
        // In the real pipeline, you'd pass the packet timestamp alongside the msg.
    }

    void print() const {
        std::printf("\n=== Message Handler Statistics ===\n");
        std::printf("  Total messages  : %lu\n", total_messages);
        std::printf("  Add orders      : %lu\n", add_orders);
        std::printf("  Delete orders   : %lu\n", delete_orders);
        std::printf("  Executions      : %lu\n", executions);
    }
};

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    // Parse arguments.
    std::string group      = "239.1.1.1";
    std::string iface      = "lo";
    std::string replay_file;
    uint16_t    port       = 15001;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--group")  && i+1 < argc) group      = argv[++i];
        else if (!std::strcmp(argv[i], "--port")   && i+1 < argc) port  = uint16_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--iface")  && i+1 < argc) iface = argv[++i];
        else if (!std::strcmp(argv[i], "--replay") && i+1 < argc) replay_file = argv[++i];
    }

    // ── Set up gap buffer ─────────────────────────────────────────────────────
    GapBuffer::Config gap_cfg;
    gap_cfg.gap_timeout_ns    = 200'000;     // 200 µs
    gap_cfg.retry_interval_ns = 2'000'000;   // 2 ms
    std::memcpy(gap_cfg.session, "          ", 10);

    GapBuffer gap_buf(gap_cfg);

    Handler handler;
    gap_buf.set_on_message([&](const MoldMessage& m) { handler.on_message(m); });
    gap_buf.set_on_gap([](const GapEvent& g) {
        std::fprintf(stderr,
            "[gap] seq %lu..%lu detected at %.3f ms\n",
            g.first_missing_seq, g.last_missing_seq,
            double(g.detected_at_ns) / 1e6);
    });
    gap_buf.set_on_retransmit([](const RetransmitRequest& r) {
        std::fprintf(stderr, "[retx] requesting seq %lu, count %u\n",
                     r.first_seq, r.count);
    });

    // ── PCAP replay mode ──────────────────────────────────────────────────────
    if (!replay_file.empty()) {
        std::printf("Replaying %s...\n", replay_file.c_str());

        PcapReader::Config pcfg;
        pcfg.realtime   = false;  // replay as fast as possible
        pcfg.filter_port = port;

        PcapReader reader(pcfg);
        auto stats = reader.replay(replay_file, gap_buf);

        std::printf("PCAP stats:\n");
        std::printf("  Packets read:    %lu\n", stats.packets_read);
        std::printf("  UDP packets:     %lu\n", stats.udp_packets);
        std::printf("  Non-UDP skipped: %lu\n", stats.non_udp_skipped);
        std::printf("  Messages:        %lu\n", stats.messages_ingested);

        handler.print();

        std::printf("\nGap buffer stats:\n");
        std::printf("  Gaps detected:   %lu\n", gap_buf.stat_gaps());
        std::printf("  Duplicates:      %lu\n", gap_buf.stat_duplicates());
        std::printf("  Retransmit reqs: %lu\n", gap_buf.stat_retransmit_requests());
        std::printf("  Heartbeats:      %lu\n", gap_buf.stat_heartbeats());
        return 0;
    }

    // ── Live multicast mode ───────────────────────────────────────────────────
    MulticastReceiver::Config rcfg;
    rcfg.multicast_group = group;
    rcfg.interface_name  = iface;
    rcfg.port            = port;

    MulticastReceiver receiver(rcfg, gap_buf);
    if (!receiver.open()) {
        std::fprintf(stderr, "Failed to open multicast socket\n");
        return 1;
    }

    std::printf("Listening on %s:%u (interface %s)\n",
                group.c_str(), port, iface.c_str());
    std::printf("Press Ctrl+C to stop.\n\n");

    auto t0 = std::chrono::steady_clock::now();

    receiver.run(g_running);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    std::printf("\nRan for %ld ms\n", elapsed);
    handler.print();

    std::printf("\nGap buffer stats:\n");
    std::printf("  Gaps detected:   %lu\n", gap_buf.stat_gaps());
    std::printf("  Duplicates:      %lu\n", gap_buf.stat_duplicates());
    std::printf("  Retransmit reqs: %lu\n", gap_buf.stat_retransmit_requests());
    std::printf("  Heartbeats:      %lu\n", gap_buf.stat_heartbeats());

    return 0;
}
