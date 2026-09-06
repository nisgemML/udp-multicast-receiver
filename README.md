# UDP Multicast Market Data Receiver

[![CI](https://github.com/nisgemML/udp-multicast-receiver/actions/workflows/ci.yml/badge.svg)](https://github.com/nisgemML/udp-multicast-receiver/actions/workflows/ci.yml)

A production-grade MoldUDP64 multicast feed receiver in C++20. Handles sequence gaps, retransmit requests, out-of-order delivery, and redundant A/B feed lines — then reconstructs a full multi-symbol limit order book from the resulting ITCH 5.0 stream and wires it into a measurable tick-to-trade decision pipeline.

This is the full path from raw UDP datagrams to a decision: MoldUDP64 → gap-free ITCH stream → order book → decision, with a real latency measurement at the last step, not just the first.

---

## Architecture

```
                     ┌─────────────────────────────────────┐
UDP multicast feed   │         MulticastReceiver             │
 (A line, or PCAP)──►│  recvmsg() + SO_TIMESTAMPING          │
                     │  kernel-level rx timestamp            │
                     └──────────────┬────────────────────────┘
                                    │
UDP multicast feed   ┌──────────────▼────────────────────────┐
 (B line, redundant)►│           FeedArbitrator                │
                     │  shares one GapBuffer across A/B lines  │
                     │  first-valid-wins, per-line health      │
                     └──────────────┬────────────────────────┘
                                    │ raw datagram + timestamp
                                    ▼
                     ┌─────────────────────────────────────┐
                     │            GapBuffer                 │
                     │  sequence tracking + OOO buffering    │
                     │  retransmit request generation        │
                     └──────────────┬────────────────────────┘
                                    │ in-order MoldMessage stream (+ recv_ns)
                                    ▼
                     ┌─────────────────────────────────────┐
                     │      MultiSymbolBook (order_book.hpp) │
                     │  per-symbol limit order book          │
                     │  order_ref → symbol routing index     │
                     └──────────────┬────────────────────────┘
                                    │ top-of-book change (symbol, book, recv_ns)
                                    ▼
                     ┌─────────────────────────────────────┐
                     │         DecisionEngine                │
                     │  toy quoting rule + latency histogram │
                     │  recv_ns → decided_ns, measured        │
                     └─────────────────────────────────────┘
```

```
PcapReader ──► GapBuffer   (offline replay — identical downstream path)
```

A single-line receiver only needs `MulticastReceiver → GapBuffer`; the
`FeedArbitrator` stage is additive for venues that publish redundant A/B
lines (NASDAQ ITCH does). Both paths feed the same `GapBuffer::OnMessage`
contract, so everything downstream (order book, decision engine) is
identical either way.

---

## Components

### `include/feed/wire_format.hpp`

MoldUDP64 header parsing and ITCH 5.0 message layout: Add Order, Delete,
Cancel, Executed, and Replace — the five opcodes needed to reconstruct a
resting order book, not just count message types. Zero-copy design:
`MoldMessage.body` is a pointer directly into the receive buffer, and
`MoldMessage.recv_ns` carries the original packet arrival timestamp
through the whole pipeline (see `gap_buffer.hpp` below for why that has to
survive buffering). Big-endian helpers (`be16`, `be32`, `be48`, `be64`)
use `__builtin_bswap*` to avoid the glibc `ntohl` conditional and stay
free of POSIX socket headers.

### `include/feed/gap_buffer.hpp`

The core of the receiver. Maintains `next_expected_seq` and handles all three failure modes:

| Condition | Detection | Action |
|---|---|---|
| `seq > next_expected` | gap detected | buffer packet, fire `on_gap` callback, request retransmit after `gap_timeout_ns` |
| `seq == next_expected` | in-order | deliver immediately, flush any buffered out-of-order packets |
| `seq < next_expected` | duplicate | drop silently, increment `stat_duplicates` |

**Retransmit timing:** gap detected → wait `gap_timeout_ns` (default 100µs) before first request → retry every `retry_interval_ns` (default 1ms). Rate-limited to avoid flooding the retransmission server.

**Buffer capacity:** 4096 slots (power-of-two for O(1) index via bitmask). At NASDAQ peak (~10M msg/sec), a 1ms retransmit RTT creates ~10,000 buffered messages — the 4096-slot buffer handles typical gaps while consuming ~6MB of pre-allocated memory. See `docs/loss-handling.md` §2 for what happens when a gap exceeds it.

**Gap closure:** when the missing sequence(s) arrive (via retransmit or natural delivery), `flush_buffer()` scans forward from `next_expected` and delivers all consecutive buffered packets. Each buffered slot keeps the original `recv_ns` of its packet, so a message delivered late (because it sat behind a gap) still carries its true arrival time downstream — not the time it happened to get flushed.

### `include/feed/receiver.hpp`

Live UDP multicast socket with `SO_TIMESTAMPING`. Key design decisions:

**`SO_TIMESTAMPING` over `gettimeofday`:** application-level timestamps measure when userspace *reads* the packet (subject to scheduler jitter, ~100µs on a loaded system). `SOF_TIMESTAMPING_RX_SOFTWARE` moves the timestamp into the kernel receive path — taken when the packet enters the socket receive queue, ~1-5µs more accurate. With `SOF_TIMESTAMPING_RX_HARDWARE` (supported NIC required), the timestamp is taken at the NIC DMA for ~10ns accuracy. `tools/timestamp_validate` measures the userspace-read side of this tradeoff for real — see `BENCHMARK_RESULTS.md`.

**Busy-poll with `PAUSE`:** the receive loop calls `recvmsg(MSG_DONTWAIT)` in a tight loop with `__builtin_ia32_pause()` on empty returns. On a dedicated isolated core with `SCHED_FIFO`, this achieves the lowest possible receive latency. The `PAUSE` hint reduces memory bus traffic and power consumption during idle spins.

**Source-specific multicast (SSM):** when `source_ip` is set, uses `IP_ADD_SOURCE_MEMBERSHIP` instead of `IP_ADD_MEMBERSHIP`. SSM filters at the network layer — the kernel drops non-matching datagrams before they reach userspace. Production NASDAQ feeds use SSM to reduce spurious traffic.

### `include/feed/feed_arbitrator.hpp`

Arbitrates NASDAQ-style redundant A/B multicast lines. The insight (see
the file's header comment for the full reasoning) is that arbitration
doesn't need new state: feeding both lines into one shared `GapBuffer`
gets first-valid-wins delivery and automatic duplicate-drop for free from
existing dedup logic. `FeedArbitrator` is a thin instrumentation layer on
top — per-line packet/win/redundant counters and dark-line detection —
not a second correctness path. A drop on one line that the other line
covers never fires a gap or a retransmit request at all; only a drop on
**both** lines does. See `docs/loss-handling.md` §3 for the operational
implications.

### `include/feed/order_book.hpp`

Reconstructs a full multi-symbol limit order book from the decoded ITCH
stream. `OrderBook` applies Add/Cancel/Delete/Executed/Replace to
per-order state (`order_ref → {side, price, shares}`) and aggregated
price levels (best bid/ask in O(1)); `MultiSymbolBook` routes messages to
the right per-symbol book via an `order_ref → symbol` index, since ITCH
only carries the symbol on the Add message — everything after that
references only an order_ref. See the file's header comment for why a
naive per-order-count decrement on every partial cancel is a real, easy
bug (it was — caught by the test suite while building this, fixed, and
now regression-tested).

### `include/feed/decision_engine.hpp`

Closes the pipeline from order book state to an emitted decision, with a
latency measurement point at each end — not a matching engine or a
trading strategy, deliberately: see the file header for exactly what
"tick-to-trade" means here (a real, measured software-pipeline latency)
and, just as importantly, what it *isn't* (not a co-location wire-time
figure, and PCAP-file timestamps are explicitly not used for this
measurement — they're original capture time, not "now").

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

### `tools/tick_to_trade_bench`

Measures the software tick-to-trade latency described above, end to end,
with real numbers from an actual run — see `BENCHMARK_RESULTS.md`.

```bash
./tick_to_trade_bench --messages 200000 --warmup 20000 --symbols 8
```

### `tools/timestamp_validate`

Validates `SO_TIMESTAMPING` behavior with real, locally-measured numbers
(loopback round-trip, same clock domain throughout), and reports plainly
when hardware timestamping isn't available rather than fabricating a
number — see `BENCHMARK_RESULTS.md` and the file's header comment for the
full methodology, including exactly what it can't validate without real
NIC hardware.

```bash
./timestamp_validate --count 20000
```

---

## Tests

```
tests/test_wire_format.cpp     — byte-order helpers, MoldHeader parse/round-trip,
                                  ITCH message decode (Add/Delete/Cancel/Executed/Replace),
                                  RetransmitRequest serialise
tests/test_gap_buffer.cpp      — in-order delivery, duplicate drop, gap detection,
                                  out-of-order flush, retransmit timing, heartbeats,
                                  multi-message packets, large sequential stream
tests/test_pcap_replay.cpp     — write + read round-trip, gap in PCAP file,
                                  heartbeat filtering, port filter, multi-message
tests/test_order_book.cpp      — full order lifecycle, price-level aggregation,
                                  multi-symbol routing, top-of-book callbacks,
                                  end-to-end GapBuffer → book over a real stream
tests/test_feed_arbitrator.cpp — A/B line arbitration: one-line-drop-covered-by-
                                  the-other produces zero gaps; both-lines-drop
                                  still produces exactly one real gap; dark-line
                                  detection
tests/test_decision_engine.cpp — decision rule correctness, latency-recording
                                  guard rails, end-to-end wiring to OrderBook
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

# Benchmarks (work anywhere, no live feed needed)
./build/tick_to_trade_bench --messages 200000
./build/timestamp_validate --count 20000
```

---

## Design decisions

**Why not `epoll`?** For a co-located feed receiver, the goal is to minimise the time between a packet arriving at the NIC and being processed. `epoll` adds a syscall on each event; a busy-poll loop adds only the `recvmsg` cost. With a dedicated isolated core and `SCHED_FIFO` priority, busy-polling achieves ~1µs lower latency than event-driven I/O at the cost of 100% CPU usage on that core — a standard HFT trade-off.

**Why `#pragma pack` on PCAP headers?** The `PcapGlobalHeader` and `PcapRecordHeader` structs are read directly from disk with `fread`. Without `#pragma pack(1)`, the compiler may insert alignment padding that would misalign the `fread` into the struct fields. The PCAP format defines field offsets by byte position, not by natural alignment.

**Why a fixed-size ring buffer instead of a dynamic list?** Dynamic allocation on the receive path introduces unpredictable latency (malloc under contention can take microseconds). The 4096-slot array is allocated once at construction and never resized. Gap sizes exceeding the buffer (>4096 messages) are counted as `stat_buffer_overflows` and treated as irrecoverable — the application should reconnect. See `docs/loss-handling.md` §2 for exactly what "reconnect" needs to mean for the order book, not just the socket.

**Why separate `on_gap` and `on_retransmit` callbacks?** `on_gap` fires immediately when a gap is first detected — useful for latency monitoring ("how long do gaps take to fill?"). `on_retransmit` fires when a request is actually sent — useful for rate-limiting and logging. Keeping them separate avoids conflating detection with recovery.

**Why does `FeedArbitrator` share one `GapBuffer` instead of running two and merging?** Two independent `GapBuffer`s would each treat the other line's exclusive packets as permanent gaps, and merging two already-gapped streams after the fact is strictly harder than not gapping in the first place. Feeding both lines into one `GapBuffer` means the existing dedup logic *is* the arbitration logic — see `feed_arbitrator.hpp`.

**Why does the order book need its own `order_ref → symbol` index?** Because ITCH doesn't give you a choice: Cancel/Delete/Executed/Replace carry only an order_ref, never a symbol, so a multi-symbol reconstructor has to maintain that mapping itself or it cannot route those messages at all — see `order_book.hpp`.

**Why isn't `tools/tick_to_trade_bench`'s number comparable to a real HFT tick-to-trade figure?** It doesn't touch a NIC. It measures this process's own parse → book → decision cost using two `CLOCK_MONOTONIC` reads in the same process — real, reproducible, and honestly a different (smaller) quantity than a co-located wire-to-decision number. See `decision_engine.hpp`'s header comment.

---

## Production considerations

**Not included (intentionally):**

- **Retransmission TCP client:** `MulticastReceiver` fires the `on_retransmit` callback with a populated `RetransmitRequest`. Connecting to the retransmission server and sending it over TCP is left to the application layer — it depends on your network topology and whether you have a backup feed. See `docs/loss-handling.md` §2.

- **CPU pinning:** `pthread_setaffinity_np(CPU_N)` and `SCHED_FIFO` priority belong in the application's startup sequence, not in the library. See `docs/linux-tuning.md` for the full setup.

- **Hardware timestamping:** requires a supported NIC (Solarflare/Xilinx with OpenOnload, Intel X710 with PTP, Mellanox with PTP). The receiver already sets the `SOF_TIMESTAMPING_RX_HARDWARE` flag when `enable_hw_timestamps = true`; the driver must support it. `tools/timestamp_validate` documents exactly what it can and can't validate without that hardware.

- **DPDK / kernel bypass:** for sub-microsecond requirements, bypass the kernel entirely. The `GapBuffer` and wire format parsing are transport-agnostic — `GapBuffer::ingest()` takes a raw byte span.

- **Order book snapshot/refresh:** ITCH has no "give me the current book" message. A consumer that starts mid-session reconstructs a gap-free but *incomplete* book with no error or warning anywhere in this codebase. This is real enough to have its own section — see `docs/loss-handling.md` §4.

See `docs/loss-handling.md` for the full recovery runbook: what each failure mode looks like operationally, what to monitor, and what "reconnect" actually needs to mean once an order book is involved.

