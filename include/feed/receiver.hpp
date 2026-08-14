#pragma once
// receiver.hpp — UDP multicast socket receiver with kernel-level timestamping.
//
// ── SO_TIMESTAMPING ───────────────────────────────────────────────────────────
//
// Application-level gettimeofday() timestamps measure when userspace *reads*
// the packet, not when it arrived at the NIC.  The difference is dominated by
// scheduler jitter and can be hundreds of microseconds.
//
// SO_TIMESTAMPING (Linux 2.6.30+) moves the timestamp into the kernel receive
// path.  With SOF_TIMESTAMPING_RX_SOFTWARE the timestamp is taken when the
// packet enters the socket receive queue — much closer to actual arrival.
// With hardware timestamping (SOF_TIMESTAMPING_RX_HARDWARE + a supported NIC)
// the timestamp is taken at the NIC DMA, giving ~10ns accuracy.
//
// The timestamp is retrieved via recvmsg() with MSG_ERRQUEUE, appearing in
// the ancillary data (cmsg) as a struct timespec in SO_TIMESTAMPING cmsg.
//
// ── Multicast join ────────────────────────────────────────────────────────────
//
// We use IP_ADD_MEMBERSHIP to join a multicast group on a specific interface.
// SO_REUSEADDR / SO_REUSEPORT allows multiple processes to receive from the
// same group:port — useful for running redundant receivers.
//
// ── Receive loop design ───────────────────────────────────────────────────────
//
// The receive loop busy-polls: recvmsg() with MSG_DONTWAIT returns EAGAIN if no
// packet is ready.  We spin rather than sleep to achieve the lowest possible
// latency.  On a dedicated isolated core this is appropriate; on a shared core
// you'd use epoll with a short timeout as a hybrid.
//
// The socket is bound to INADDR_ANY:port (not to the multicast group address)
// because Linux routes multicast packets based on the IP_ADD_MEMBERSHIP, not
// the bind address.

#include "feed/wire_format.hpp"
#include "feed/gap_buffer.hpp"

#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/net_tstamp.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <functional>
#include <atomic>
#include <string>

namespace feed {

// ── Receive timestamp ─────────────────────────────────────────────────────────

struct PacketTimestamp {
    uint64_t software_ns = 0;  // SOF_TIMESTAMPING_SOFTWARE (kernel recv queue)
    uint64_t hw_ns       = 0;  // SOF_TIMESTAMPING_RAW_HARDWARE (NIC, if available)
};

// ── MulticastReceiver ─────────────────────────────────────────────────────────

class MulticastReceiver {
public:
    struct Config {
        std::string multicast_group = "239.1.1.1";
        std::string source_ip       = "";          // if non-empty, use IP_ADD_SOURCE_MEMBERSHIP
        std::string interface_name  = "lo";        // network interface to join on
        uint16_t    port            = 15001;
        bool        enable_software_timestamps = true;
        bool        enable_hw_timestamps       = false;
        bool        reuse_port                 = true;
        int         recv_buffer_bytes          = 8 * 1024 * 1024;  // 8 MB
    };

    explicit MulticastReceiver(Config cfg, GapBuffer& gap_buf)
        : cfg_(std::move(cfg)), gap_buf_(gap_buf) {}

    ~MulticastReceiver() { close(); }

    // Non-copyable — owns a file descriptor.
    MulticastReceiver(const MulticastReceiver&)            = delete;
    MulticastReceiver& operator=(const MulticastReceiver&) = delete;

    // Open and configure the socket.  Returns false on error (errno set).
    [[nodiscard]] bool open() noexcept {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd_ < 0) return report_error("socket");

        if (!set_socket_options()) return false;
        if (!bind_socket())        return false;
        if (!join_multicast())     return false;

        return true;
    }

    void close() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

    // ── Receive one packet ────────────────────────────────────────────────────
    //
    // Returns number of messages delivered downstream, 0 if no packet ready,
    // -1 on socket error.  Non-blocking (MSG_DONTWAIT).

    int recv_one() noexcept {
        // recvmsg control buffer for ancillary data (timestamps).
        alignas(16) uint8_t ctrl_buf[256];
        iovec  iov  { recv_buf_, sizeof(recv_buf_) };
        msghdr msg  {};
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = ctrl_buf;
        msg.msg_controllen = sizeof(ctrl_buf);

        const ssize_t n = ::recvmsg(fd_, &msg, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return report_error("recvmsg");
        }
        ++stat_recv_calls_;

        const uint64_t sw_ns = extract_timestamp(msg, ctrl_buf);
        const uint64_t ts_ns = sw_ns ? sw_ns : monotonic_ns();

        const int delivered = gap_buf_.ingest(recv_buf_, static_cast<std::size_t>(n), ts_ns);

        if (delivered > 0)  stat_messages_delivered_ += delivered;
        if (delivered < 0)  ++stat_parse_errors_;

        return delivered;
    }

    // ── Busy-poll receive loop ─────────────────────────────────────────────────
    //
    // Spins calling recv_one() until `running` is set to false.
    // Calls gap_buf_.tick() every `tick_interval_ns` nanoseconds.

    void run(std::atomic<bool>& running,
             uint64_t tick_interval_ns = 100'000) noexcept
    {
        uint64_t last_tick = monotonic_ns();
        while (running.load(std::memory_order_relaxed)) {
            recv_one();
            const uint64_t now = monotonic_ns();
            if (now - last_tick >= tick_interval_ns) {
                gap_buf_.tick(now);
                last_tick = now;
            }
            __builtin_ia32_pause();  // reduce power + memory traffic while spinning
        }
    }

    // ── Statistics ────────────────────────────────────────────────────────────

    [[nodiscard]] uint64_t stat_recv_calls()         const noexcept { return stat_recv_calls_; }
    [[nodiscard]] uint64_t stat_messages_delivered() const noexcept { return stat_messages_delivered_; }
    [[nodiscard]] uint64_t stat_parse_errors()       const noexcept { return stat_parse_errors_; }

private:
    // ── Socket setup ──────────────────────────────────────────────────────────

    bool set_socket_options() noexcept {
        int one = 1;

        if (cfg_.reuse_port) {
            if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
                return report_error("SO_REUSEADDR");
            if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0)
                return report_error("SO_REUSEPORT");
        }

        // Receive buffer — bump to reduce kernel-side drops during bursts.
        if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF,
                         &cfg_.recv_buffer_bytes,
                         sizeof(cfg_.recv_buffer_bytes)) < 0)
            return report_error("SO_RCVBUF");

        // Software timestamps: taken when the packet enters the socket receive queue.
        if (cfg_.enable_software_timestamps) {
            int ts_flags = SOF_TIMESTAMPING_RX_SOFTWARE |
                           SOF_TIMESTAMPING_SOFTWARE;
            if (cfg_.enable_hw_timestamps)
                ts_flags |= SOF_TIMESTAMPING_RX_HARDWARE |
                            SOF_TIMESTAMPING_RAW_HARDWARE;
            if (::setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPING,
                             &ts_flags, sizeof(ts_flags)) < 0)
                return report_error("SO_TIMESTAMPING");
        }

        // Disable IP_MULTICAST_LOOP: we don't want to receive our own sends
        // (relevant if this process also sends on the multicast group).
        int loop = 0;
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

        // Bind to the specific interface for multicast sends (affects TTL scope).
        const in_addr iface_addr = interface_address(cfg_.interface_name.c_str());
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF,
                     &iface_addr, sizeof(iface_addr));

        return true;
    }

    bool bind_socket() noexcept {
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(cfg_.port);
        addr.sin_addr.s_addr = INADDR_ANY;  // bind to all interfaces
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            return report_error("bind");
        return true;
    }

    bool join_multicast() noexcept {
        const in_addr group  = to_addr(cfg_.multicast_group.c_str());
        const in_addr iface  = interface_address(cfg_.interface_name.c_str());

        if (cfg_.source_ip.empty()) {
            // Any-source multicast.
            ip_mreq mreq{};
            mreq.imr_multiaddr = group;
            mreq.imr_interface = iface;
            if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                             &mreq, sizeof(mreq)) < 0)
                return report_error("IP_ADD_MEMBERSHIP");
        } else {
            // Source-specific multicast (SSM) — only receive from one source.
            // More common in production because it reduces unwanted traffic.
            ip_mreq_source mreq{};
            mreq.imr_multiaddr  = group;
            mreq.imr_interface  = iface;
            mreq.imr_sourceaddr = to_addr(cfg_.source_ip.c_str());
            if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP,
                             &mreq, sizeof(mreq)) < 0)
                return report_error("IP_ADD_SOURCE_MEMBERSHIP");
        }
        return true;
    }

    // ── Timestamp extraction ──────────────────────────────────────────────────
    //
    // SO_TIMESTAMPING delivers a cmsg of type SCM_TIMESTAMPING containing
    // three struct timespec values:
    //   [0] = software timestamp  (SOF_TIMESTAMPING_SOFTWARE)
    //   [1] = (deprecated)
    //   [2] = hardware timestamp  (SOF_TIMESTAMPING_RAW_HARDWARE)

    static uint64_t extract_timestamp(const msghdr& msg,
                                       const uint8_t* ctrl_buf) noexcept
    {
        for (const cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm != nullptr;
             cm = CMSG_NXTHDR(const_cast<msghdr*>(&msg), const_cast<cmsghdr*>(cm)))
        {
            if (cm->cmsg_level == SOL_SOCKET &&
                cm->cmsg_type  == SO_TIMESTAMPING)
            {
                // Three struct timespec values.
                struct timespec ts[3];
                std::memcpy(ts, CMSG_DATA(cm), sizeof(ts));
                // Use software timestamp (ts[0]) if non-zero.
                if (ts[0].tv_sec != 0 || ts[0].tv_nsec != 0)
                    return uint64_t(ts[0].tv_sec) * 1'000'000'000ULL +
                           uint64_t(ts[0].tv_nsec);
            }
        }
        return 0;
    }

    // ── Utility ───────────────────────────────────────────────────────────────

    static uint64_t monotonic_ns() noexcept {
        struct timespec ts;
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
    }

    static in_addr to_addr(const char* s) noexcept {
        in_addr a{}; ::inet_pton(AF_INET, s, &a); return a;
    }

    static in_addr interface_address(const char* ifname) noexcept {
        // Try to resolve the interface IP from its name.
        // Falls back to INADDR_ANY (0.0.0.0) which lets the kernel pick.
        int s = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) { in_addr a{}; a.s_addr = INADDR_ANY; return a; }
        ifreq ifr{};
        std::strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
        in_addr a{};
        if (::ioctl(s, SIOCGIFADDR, &ifr) == 0)
            a = reinterpret_cast<sockaddr_in*>(&ifr.ifr_addr)->sin_addr;
        else
            a.s_addr = INADDR_ANY;
        ::close(s);
        return a;
    }

    bool report_error(const char* call) noexcept {
        std::fprintf(stderr, "[MulticastReceiver] %s: %s\n", call, std::strerror(errno));
        return false;
    }

    // ── Members ───────────────────────────────────────────────────────────────

    Config     cfg_;
    GapBuffer& gap_buf_;
    int        fd_ = -1;

    // Single large receive buffer — reused every call to avoid allocations.
    uint8_t recv_buf_[65535];

    uint64_t stat_recv_calls_          = 0;
    uint64_t stat_messages_delivered_  = 0;
    uint64_t stat_parse_errors_        = 0;
};

} // namespace feed
