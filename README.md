# UDP Multicast Market Data Receiver

[![CI](https://github.com/nisgemML/udp-multicast-receiver/actions/workflows/ci.yml/badge.svg)](https://github.com/nisgemML/udp-multicast-receiver/actions/workflows/ci.yml)

A production-grade MoldUDP64 multicast feed receiver in C++20. Handles sequence gaps, retransmit requests, and out-of-order delivery. PCAP replay makes it testable without a live feed.

This is the component that sits before the order book: it turns raw UDP datagrams into a sequenced, gap-free stream of ITCH 5.0 messages.

---

## Architecture

```
                     ┌─────────────────────────────────────┐
UDP multicast feed   │         MulticastReceiver            │
 (or PCAP file)  ──► │  recvmsg() + SO_TIMESTAMPING        │
                     │  kernel-level rx timestamp           │
                     └──────────────┬──────────────────────┘
                                    │ raw datagram + timestamp
                                    ▼
                     ┌─────────────────────────────────────┐
                     │            GapBuffer                │
                     │  sequence tracking + OOO buffering  │
                     │  retransmit request generation      │
                     └──────────────┬──────────────────────┘
                                    │ in-order MoldMessage stream
                                    ▼
                     ┌─────────────────────────────────────┐
                     │         Application handler          │
                     │  ITCH decode → order book / risk    │
                     └─────────────────────────────────────┘
```

```
PcapReader ──► GapBuffer   (offline replay — identical downstream path)
```

---

## Components

### `include/feed/wire_format.hpp`

MoldUDP64 header parsing and ITCH 5.0 message layout. Zero-copy design: `MoldMessage.body` is a pointer directly into the receive buffer. Big-endian helpers (`be16`, `be32`, `be48`, `be64`) use `__builtin_bswap*` to avoid the glibc `ntohl` conditional and stay free of POSIX socket headers.

### `include/feed/gap_buffer.hpp`

The core of the receiver. Maintains `next_expected_seq` and handles all three failure modes:

| Condition | Detection | Action |
|---|---|---|
| `seq > next_expected` | gap detected | buffer packet, fire `on_gap` callback, request retransmit after `gap_timeout_ns` |
| `seq == next_expected` | in-order | deliver immediately, flush any buffered out-of-order packets |
| `seq < next_expected` | duplicate | drop silently, increment `stat_duplicates` |

**Retransmit timing:** gap detected → wait `gap_timeout_ns` (default 100µs) before first request → retry every `retry_interval_ns` (default 1ms). Rate-limited to avoid flooding the retransmission server.

**Buffer capacity:** 4096 slots (power-of-two for O(1) index via bitmask). At NASDAQ peak (~10M msg/sec), a 1ms retransmit RTT creates ~10,000 buffered messages — the 4096-slot buffer handles typical gaps while consuming ~6MB of pre-allocated memory.

**Gap closure:** when the missing sequence(s) arrive (via retransmit or natural delivery), `flush_buffer()` scans forward from `next_expected` and delivers all consecutive buffered packets.

### `include/feed/receiver.hpp`

Live UDP multicast socket with `SO_TIMESTAMPING`. Key design decisions:

**`SO_TIMESTAMPING` over `gettimeofday`:** application-level timestamps measure when userspace *reads* the packet (subject to scheduler jitter, ~100µs on a loaded system). `SOF_TIMESTAMPING_RX_SOFTWARE` moves the timestamp into the kernel receive path — taken when the packet enters the socket receive queue, ~1-5µs more accurate. With `SOF_TIMESTAMPING_RX_HARDWARE` (supported NIC required), the timestamp is taken at the NIC DMA for ~10ns accuracy.

**Busy-poll with `PAUSE`:** the receive loop calls `recvmsg(MSG_DONTWAIT)` in a tight loop with `__builtin_ia32_pause()` on empty returns. On a dedicated isolated core with `SCHED_FIFO`, this achieves the lowest possible receive latency. The `PAUSE` hint reduces memory bus traffic and power consumption during idle spins.

**Source-specific multicast (SSM):** when `source_ip` is set, uses `IP_ADD_SOURCE_MEMBERSHIP` instead of `IP_ADD_MEMBERSHIP`. SSM filters at the network layer — the kernel drops non-matching datagrams before they reach userspace. Production NASDAQ feeds use SSM to reduce spurious traffic.

### `include/feed/pcap_replay.hpp`

**`PcapWriter`:** generates synthetic MoldUDP64 PCAP files with full Ethernet+IP+UDP framing. Used by the test suite and by `gen_pcap` to create test fixtures.

**`PcapReader`:** reads standard libpcap files (both byte-order variants), extracts UDP payloads, and feeds them through the same `GapBuffer::ingest()` path as the live receiver. Handles 802.1Q VLAN tags. Supports real-time pacing (replay at original packet timing) or as-fast-as-possible for CI.

### `tools/gen_pcap`

Generates synthetic PCAP files with controlled test patterns:

```bash
# 10000 messages, gap at seq 500 (len 5), duplicate at seq 200
./gen_pcap --count 10000 --gap-at 500 --gap-len 5 --dup-at 200 output.pcap

# Replay through the receiver
./receiver --replay output.pcap --port 15001
```

---

## Tests

```
tests/test_wire_format.cpp  — byte-order helpers, MoldHeader parse/round-trip,
                              ITCH message decode, RetransmitRequest serialise
tests/test_gap_buffer.cpp   — in-order delivery, duplicate drop, gap detection,
                              out-of-order flush, retransmit timing, heartbeats,
                              multi-message packets, large sequential stream
tests/test_pcap_replay.cpp  — write + read round-trip, gap in PCAP file,
                              heartbeat filtering, port filter, multi-message
```

All tests generate their own data — no external files or live network required.

---

## Building

```bash
# Dependencies: libpcap-dev, cmake >= 3.22, ninja, g++ >= 12 or clang >= 15

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# Live multicast receiver (requires network access)
./build/receiver --group 239.1.1.1 --port 15001 --iface eth0

# PCAP replay (works anywhere)
./build/gen_pcap --count 50000 --gap-at 1000 --gap-len 3 test.pcap
./build/receiver --replay test.pcap
```

---

## Design decisions

**Why not `epoll`?** For a co-located feed receiver, the goal is to minimise the time between a packet arriving at the NIC and being processed. `epoll` adds a syscall on each event; a busy-poll loop adds only the `recvmsg` cost. With a dedicated isolated core and `SCHED_FIFO` priority, busy-polling achieves ~1µs lower latency than event-driven I/O at the cost of 100% CPU usage on that core — a standard HFT trade-off.

**Why `#pragma pack` on PCAP headers?** The `PcapGlobalHeader` and `PcapRecordHeader` structs are read directly from disk with `fread`. Without `#pragma pack(1)`, the compiler may insert alignment padding that would misalign the `fread` into the struct fields. The PCAP format defines field offsets by byte position, not by natural alignment.

**Why a fixed-size ring buffer instead of a dynamic list?** Dynamic allocation on the receive path introduces unpredictable latency (malloc under contention can take microseconds). The 4096-slot array is allocated once at construction and never resized. Gap sizes exceeding the buffer (>4096 messages) are counted as `stat_buffer_overflows` and treated as irrecoverable — the application should reconnect.

**Why separate `on_gap` and `on_retransmit` callbacks?** `on_gap` fires immediately when a gap is first detected — useful for latency monitoring ("how long do gaps take to fill?"). `on_retransmit` fires when a request is actually sent — useful for rate-limiting and logging. Keeping them separate avoids conflating detection with recovery.

---

## Production considerations

**Not included (intentionally):**

- **Retransmission TCP client:** `MulticastReceiver` fires the `on_retransmit` callback with a populated `RetransmitRequest`. Connecting to the retransmission server and sending it over TCP is left to the application layer — it depends on your network topology and whether you have a backup feed.

- **CPU pinning:** `pthread_setaffinity_np(CPU_N)` and `SCHED_FIFO` priority belong in the application's startup sequence, not in the library. See `docs/linux-tuning.md` for the full setup.

- **Hardware timestamping:** requires a supported NIC (Solarflare/Xilinx with OpenOnload, Intel X710 with PTP, Mellanox with PTP). The receiver already sets the `SOF_TIMESTAMPING_RX_HARDWARE` flag when `enable_hw_timestamps = true`; the driver must support it.

- **DPDK / kernel bypass:** for sub-microsecond requirements, bypass the kernel entirely. The `GapBuffer` and wire format parsing are transport-agnostic — `GapBuffer::ingest()` takes a raw byte span.
