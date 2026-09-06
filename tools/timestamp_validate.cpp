// timestamp_validate.cpp — validates SO_TIMESTAMPING behavior with real,
// locally-measured numbers, and documents (without faking) what hardware
// timestamping validation requires.
//
// ── Methodology ────────────────────────────────────────────────────────────
//
// This sends UDP packets to itself over loopback and, for each one,
// records three points in the SAME clock domain (CLOCK_REALTIME, because
// that's the domain the kernel's SO_TIMESTAMPING software timestamp uses —
// see the note below):
//
//   t_send   — userspace clock_gettime() immediately before sendto()
//   t_kernel — the kernel's SOF_TIMESTAMPING_RX_SOFTWARE timestamp,
//              stamped when the packet entered this socket's receive
//              queue (retrieved via recvmsg()'s SCM_TIMESTAMPING cmsg)
//   t_read   — userspace clock_gettime() immediately after recvmsg()
//              returns with the packet
//
// From these, two intervals are genuinely measurable on any machine:
//
//   send_to_kernel_rx = t_kernel - t_send   (loopback stack transit —
//                        tiny; the closest thing to "wire time" available
//                        without real hardware)
//   kernel_rx_to_read  = t_read - t_kernel   (scheduler wake-up latency —
//                        exactly the gap receiver.hpp's design notes claim
//                        SO_TIMESTAMPING avoids for latency *measurement*
//                        purposes, since the recorded timestamp is
//                        t_kernel, not t_read)
//
// ── Why CLOCK_REALTIME, not CLOCK_MONOTONIC ───────────────────────────────────
//
// Linux's SO_TIMESTAMPING software timestamp (ts[0], SOF_TIMESTAMPING_
// SOFTWARE) is generated from the realtime clock, not the monotonic clock.
// Comparing it against a CLOCK_MONOTONIC userspace read (as the rest of
// this codebase does for its own internal latency math, correctly, since
// that's an all-monotonic comparison) would silently compare two different
// clock domains and produce a nonsense delta. This tool uses
// CLOCK_REALTIME throughout specifically to keep t_send/t_kernel/t_read
// comparable.
//
// ── Hardware timestamping ─────────────────────────────────────────────────────
//
// SOF_TIMESTAMPING_RX_HARDWARE requires a NIC that actually supports it
// (Intel X710/E810, Mellanox ConnectX-4/5/6, Solarflare SFN8000+) and,
// typically, root or CAP_NET_ADMIN to query via ethtool. This tool
// requests the hardware timestamp flag and reports plainly whether one
// came back — on loopback, in a container, it will not, and this tool
// says so explicitly rather than printing a fabricated number. The
// "~10ns" hardware accuracy figure quoted elsewhere in this repo's docs
// is the NIC vendors' documented DMA timestamp accuracy, not something
// reproducible without that hardware; validating it for real requires
// running this same methodology on a machine with a supported NIC and
// `ethtool -T <iface>` reporting hardware-raw-clock support.

#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>
#include <net/if.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <ctime>

#include "feed/stats.hpp"

using namespace feed;

namespace {

uint64_t ts_to_ns(const timespec& ts) {
    return uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
}

uint64_t realtime_ns() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts_to_ns(ts);
}

// Best-effort check of whether an interface's driver reports hardware
// timestamping support via ethtool. Returns false (with no error printed
// as fatal) if the ioctl isn't available — that's the expected, honest
// outcome on "lo" and in most containers/CI environments.
bool interface_reports_hw_timestamping(const char* ifname) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;

    ifreq ifr{};
    std::strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ethtool_ts_info info{};
    info.cmd = ETHTOOL_GET_TS_INFO;
    ifr.ifr_data = reinterpret_cast<char*>(&info);

    const bool ok = (::ioctl(fd, SIOCETHTOOL, &ifr) == 0);
    ::close(fd);
    if (!ok) return false;
    return (info.so_timestamping & SOF_TIMESTAMPING_TX_HARDWARE) &&
           (info.so_timestamping & SOF_TIMESTAMPING_RX_HARDWARE);
}

} // namespace

int main(int argc, char* argv[]) {
    int count = 20000;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--count") && i + 1 < argc) count = std::atoi(argv[++i]);

    std::printf("=== SO_TIMESTAMPING Validation ===\n");
    std::printf("Method: %d loopback UDP round-trips, CLOCK_REALTIME throughout.\n\n", count);

    // ── Hardware timestamping capability check (informational, not fatal) ──
    const bool lo_hw = interface_reports_hw_timestamping("lo");
    std::printf("Hardware timestamping on 'lo': %s\n",
                lo_hw ? "reported available (unexpected on loopback)"
                      : "not available (expected — loopback has no NIC DMA path)");
    std::printf("To validate SOF_TIMESTAMPING_RX_HARDWARE for real, run this same\n");
    std::printf("binary's methodology against a supported NIC (Intel X710/E810,\n");
    std::printf("Mellanox ConnectX-4/5/6, Solarflare SFN8000+) after confirming\n");
    std::printf("`ethtool -T <iface>` reports hardware-raw-clock support.\n\n");

    // ── Software timestamp round-trip measurement ───────────────────────────
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { std::perror("socket"); return 1; }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(0); // let the kernel choose a free port
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind"); return 1;
    }
    socklen_t alen = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &alen);

    int ts_flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE |
                   SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &ts_flags, sizeof(ts_flags)) < 0) {
        std::perror("SO_TIMESTAMPING"); return 1;
    }

    LatencyHistogram send_to_kernel;
    LatencyHistogram kernel_to_read;
    int missing_ts = 0;
    int hw_ts_seen = 0;

    for (int i = 0; i < count; ++i) {
        char msg[8] = "ping";
        const uint64_t t_send = realtime_ns();
        if (sendto(fd, msg, 4, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::perror("sendto"); continue;
        }

        alignas(16) uint8_t ctrl[256];
        uint8_t buf[64];
        iovec iov{ buf, sizeof(buf) };
        msghdr mh{};
        mh.msg_iov = &iov; mh.msg_iovlen = 1;
        mh.msg_control = ctrl; mh.msg_controllen = sizeof(ctrl);

        const ssize_t n = recvmsg(fd, &mh, 0);
        const uint64_t t_read = realtime_ns();
        if (n < 0) { std::perror("recvmsg"); continue; }

        uint64_t t_kernel = 0;
        for (cmsghdr* cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SO_TIMESTAMPING) {
                timespec tss[3];
                std::memcpy(tss, CMSG_DATA(cm), sizeof(tss));
                if (tss[0].tv_sec != 0 || tss[0].tv_nsec != 0) t_kernel = ts_to_ns(tss[0]);
                if (tss[2].tv_sec != 0 || tss[2].tv_nsec != 0) ++hw_ts_seen;
            }
        }

        if (t_kernel == 0) { ++missing_ts; continue; }
        if (t_kernel >= t_send)  send_to_kernel.record(t_kernel - t_send);
        if (t_read   >= t_kernel) kernel_to_read.record(t_read - t_kernel);
    }
    close(fd);

    std::printf("Round-trips completed  : %d\n", count);
    std::printf("Missing SW timestamp   : %d\n", missing_ts);
    std::printf("HW timestamp populated : %d (expected 0 on loopback)\n\n", hw_ts_seen);

    send_to_kernel.print("send -> kernel_rx_sw_ts");
    kernel_to_read.print("kernel_rx_sw_ts -> read");

    std::printf("\nInterpretation:\n");
    std::printf("  send->kernel_rx  : loopback stack transit — the closest local\n");
    std::printf("                     analogue to NIC wire time available here.\n");
    std::printf("  kernel_rx->read  : scheduler wake-up latency. This is exactly\n");
    std::printf("                     the gap SO_TIMESTAMPING avoids for latency\n");
    std::printf("                     *measurement* by recording kernel_rx_sw_ts,\n");
    std::printf("                     not the time userspace happens to wake up.\n");

    return 0;
}
