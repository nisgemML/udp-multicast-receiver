# Benchmark Results — UDP Multicast Market Data Receiver

Every measured number in this document was produced by running the code in
this repo. Figures that are design estimates or vendor specifications are
labelled as such inline, so a measured number and an asserted one never
look alike. Where a number can't honestly be produced in
this environment (real NIC hardware timestamping, live multicast over a
real network), that's stated explicitly rather than filled in with a
plausible-looking figure. Reproducible:

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
ctest --output-on-failure --timeout 30   # 6/6 tests, 226 assertions

# Benchmarks (no live feed needed):
./tick_to_trade_bench --messages 200000 --warmup 20000 --symbols 8
./timestamp_validate --count 20000
```

**Environment:** Ubuntu 24.04, GCC 13.3, x86-64 container with **one vCPU**
(no core isolation, no `SCHED_FIFO`) — relevant to every tail number below. All test
binaries were also run clean under AddressSanitizer
(`-DASAN=ON -DCMAKE_BUILD_TYPE=Debug`) and under a separate Debug build —
zero failures, zero sanitizer reports, zero compiler warnings under
`-Wall -Wextra -Wpedantic` across all three configurations. This is not
a one-off, manually-run claim: `.github/workflows/ci.yml` runs all three
configurations (Release, Debug, Debug+ASan) on every push and pull
request, so a future regression in any of them fails CI, not just this
document.

---

## Test results

```
6/6 tests passed (226 assertions total)

  test_wire_format      : 62 passed, 0 failed
                           (byte-order helpers, MoldHeader parse/round-trip,
                            hand-written ITCH 5.0 golden vectors for all 5
                            book-affecting opcodes: A, D, X, E, U)
  test_gap_buffer        : 45 passed, 0 failed
                           (in-order delivery, duplicate drop, gap detection,
                            out-of-order flush, retransmit timing at a 500µs
                            configured timeout, heartbeats, multi-message packets)
  test_pcap_replay       : 26 passed, 0 failed
                           (PCAP write/read round-trip, gap in a PCAP file,
                            heartbeat filtering, port filter, multi-message)
  test_order_book        : 62 passed, 0 failed
                           (order lifecycle, price-level aggregation, multi-symbol
                            routing, top-of-book callbacks, end-to-end GapBuffer
                            integration)
  test_feed_arbitrator   : 16 passed, 0 failed
                           (A/B line arbitration, single-line-outage tolerance,
                            genuine dual-line gap, dark-line detection)
  test_decision_engine    : 15 passed, 0 failed
                           (decision rule correctness, latency guard rails,
                            end-to-end OrderBook wiring)
```

---

## A spec-conformance bug that every test passed

The ITCH 5.0 decoder originally had the wrong byte layout. Every ITCH 5.0
message starts `type | stock locate (2) | tracking number (2) | timestamp
(6)`, with message fields from offset 11. The old `ItchAddOrder` omitted
Locate and Tracking entirely (order ref at offset 7 instead of 11, price
at 28 instead of 32), and every message type read its timestamp from
offset 1 instead of 5. Against a real NASDAQ capture it would have
mis-decoded every Add Order.

All 202 assertions passed anyway, because every test fixture and tool
built its messages with the same wrong layout — encoder and decoder
agreed with each other, and nothing compared either one to the spec.

The fix is two parts: correct offsets in `wire_format.hpp` (with the
common prefix as named constants), and golden byte vectors in
`test_wire_format.cpp` typed by hand from the spec tables rather than
produced by any helper in this repo. Checked that they bite: compiling
the new golden tests against the **original** decoder gives
`50 passed, 11 failed` — exactly the timestamp/order-ref/side/shares/
stock/price fields that were mis-positioned.


---

## Order book reconstruction

`tests/test_order_book.cpp` exercises the full ITCH order-book state
machine directly (not just via the gap buffer): Add, Cancel, Delete,
Executed, and Replace against per-order state and aggregated price
levels, plus multi-symbol routing via the `order_ref → symbol` index
described in `order_book.hpp`.

**A real bug this test suite caught:** the first implementation
decremented a price level's resting-order count on every partial
cancel/fill, not just when an order fully left the book. On a
single-order price level, one partial cancel would hit zero and erase
the *entire level* — including the shares still resting on the same
order. 4 of the 62 assertions failed on the first run; the fix separates
"reduce shares" from "an order left the level" into two operations that
only jointly erase a level once both counters are actually zero.
`include/feed/order_book.hpp` documents the corrected design; the regression is
now covered by `test_cancel_partial_reduction` and
`test_executed_partial_and_full_fill`.

---

## A/B feed arbitration

`tests/test_feed_arbitrator.cpp` validates the actual value proposition
of redundant feed lines, not just that arbitration runs without
crashing:

- A drop on line A that line B covers produces **zero** gap events and
  **zero** retransmit requests — confirmed by
  `test_line_a_gap_covered_by_line_b_no_stall`, which specifically
  asserts `gaps_fired == 0` and `retx_fired == 0`, not just that all
  messages were eventually delivered.
- A drop on **both** lines still produces exactly one real gap and a
  correctly-scoped retransmit request (`first_seq`/`count` matching the
  true missing range) — `test_both_lines_drop_same_range_real_gap`.

**Two real bugs this test suite caught** while building it:
1. Win attribution used a `next_expected()` before/after diff, which
   double-counted the very first packet ever ingested (`GapBuffer`
   session-syncs its `next_expected_` sentinel from 0 inside the first
   `ingest()` call, so the diff included both the sync jump and the real
   delivery). Fixed by using `GapBuffer::ingest()`'s own delivered-count
   return value instead of re-deriving it from state.
2. A test that constructed two `GapBuffer` instances in one stack frame
   segfaulted — `GapBuffer` embeds its 4096-slot out-of-order buffer
   directly as a ~6.3MB stack member (by design, to avoid heap allocation
   on the hot path), and two of them exceeded the 8MB default stack
   limit. Split into two test functions, matching the one-`GapBuffer`-
   per-function convention every other test file already follows.

---

## Tick-to-trade software latency

`tools/tick_to_trade_bench` drives synthetic MoldUDP64 packets through
`GapBuffer → MultiSymbolBook → DecisionEngine`. Two quantities, both from
`CLOCK_MONOTONIC` reads in the same process, both with **exact**
percentiles (every sample stored and sorted, nearest-rank):

- **Per message:** wall time of one `GapBuffer::ingest()` call. The book
  update and decision rule run synchronously inside it via callbacks, so
  this is the whole parse → book → decision path for that message — not
  parse alone. (An earlier version labelled this "GapBuffer.ingest()
  only", which was wrong.)
- **recv → decision:** `decided_ns − recv_ns` for the subset of messages
  that changed top of book and passed the decision rule.

Three consecutive runs, 200,000 messages (+20,000 warmup, discarded),
8 symbols:

```
Per message (n=200,000)            p50     p90     p99     p99.9    mean
  run 1                            377     764    1402     5151      587
  run 2                            368     782    1476     5176      584
  run 3                            370     772    1408     5596      581

recv -> decision (n=3,942)         p50     p90     p99     p99.9    mean
  run 1                            397     783    1364    13198     1078
  run 2                            392     800    1623    20161     1006
  run 3                            394     786    1327     4160     1052
                                                              (all ns)
```

How to read these: p50–p99 are stable run to run; p99.9 and the mean
are not. Each run's max is ~2–8 ms, which is scheduler preemption on a
single shared vCPU, and a handful of those samples is enough to pull
the decision-path mean above 1 µs and move its p99.9 by 5x. On an
isolated core (`docs/linux-tuning.md`) the tail is the thing that should
change; on this box it isn't a property of the code.

(4,472 decisions are emitted in total; 3,942 is the post-warmup count
actually recorded.)

**Correction to earlier numbers.** A previous version of this file
reported `p50=256 p99=256 p99.9=256 mean=293` for the decision path.
Those came from `LatencyHistogram`, which bucketed by power of two and
reported each bucket's **lower** bound — so "256" meant "somewhere in
256–511", and a p99.9 below the mean was a symptom of that, not a
result. The live-path histogram now reports the bucket's exclusive upper
bound (a true "< X" statement), and the benchmarks use exact
percentiles.

**What this number is not:** it does not touch a NIC, does not include
wire transit, and is not a co-location tick-to-trade figure comparable
to what a trading firm quotes. It is this process's own parse → book →
decision cost on this CPU, and the tool's output banner says so too. See
`include/feed/decision_engine.hpp` for why PCAP-embedded timestamps
can't be used for this measurement (they're capture time, not "now").


---

## SO_TIMESTAMPING validation

`tools/timestamp_validate` measures real `SO_TIMESTAMPING` behavior via
loopback UDP round-trips, all three timestamps (`t_send`, kernel
`SOF_TIMESTAMPING_RX_SOFTWARE`, `t_read`) taken in the same
`CLOCK_REALTIME` domain — see the tool's header comment for why domain
consistency matters here (the kernel's software timestamp is
realtime-based, not monotonic, so comparing it against a monotonic
userspace read would silently mix clock domains).

Actual run, this machine, 20,000 round-trips (exact percentiles, ns):

```
Missing SW timestamp   : 0
HW timestamp populated : 0 (expected — see below)

                           p50     p90     p99     p99.9    mean
send -> kernel_rx_sw_ts    625     737    1160    11629      686
kernel_rx_sw_ts -> read   1243    1528    1945    18430     1343
```

`kernel_rx_sw_ts -> read` — the scheduler wake-up gap between the
kernel timestamping the packet and userspace actually reading it — runs
about 2x the kernel-receive-queue transit time (p50 1.24 µs vs
0.63 µs) in this
environment. That's a real, measured demonstration of exactly the
problem `receiver.hpp`'s design notes describe: an application-level
`gettimeofday()`-at-read timestamp would silently include this gap in
every latency measurement, where `SO_TIMESTAMPING`'s kernel-side
timestamp doesn't.

**Hardware timestamping was not validated — honestly, not
approximately.** This container's `lo` interface reports no hardware
timestamping support (`ethtool` `SIOCETHTOOL`/`ETHTOOL_GET_TS_INFO`
query returns no `SOF_TIMESTAMPING_RX_HARDWARE` capability, as expected
— loopback has no NIC DMA path), and `SOF_TIMESTAMPING_RAW_HARDWARE`
never populates in the captured cmsg data across all 20,000 round-trips.
The **"~10ns" hardware timestamp accuracy figure quoted below in
"Timestamping accuracy" is a NIC vendor-documented specification, not a
number measured in this repository** — that distinction matters and is
worth being explicit about. Validating it for real requires running
`timestamp_validate`'s exact methodology on a machine with a supported
NIC (Intel X710/E810, Mellanox ConnectX-4/5/6, Solarflare SFN8000+)
after confirming `ethtool -T <iface>` reports `hardware-raw-clock`
support.

---

## Parse cost (design estimate, not measured)

The header + Add Order decode is a handful of big-endian loads
(`__builtin_bswap*` compiles to `BSWAP`/`MOVBE`), so the decode itself
should cost on the order of tens of cycles. **That is an estimate from
instruction count, not a measurement** — this repo does not have a
parse-only microbenchmark, and the per-message figure above (p50 ~370 ns)
covers much more than decode: sequence tracking, dedup, two
`std::function` callback hops, the order-book update (hash-map lookups
on order ref and symbol), the decision rule, and two `clock_gettime`
calls. A parse-only benchmark is the right next step before quoting any
decode number.


---

## Timestamping accuracy

**SO_TIMESTAMPING** is the key latency measurement mechanism:

| Timestamp source | Accuracy | Source |
|---|---|---|
| `SOF_TIMESTAMPING_RX_SOFTWARE` | ~1–5µs (design estimate) | Kernel receive queue timestamp |
| `SOF_TIMESTAMPING_RX_HARDWARE` | **~10ns** (vendor spec, not measured here) | NIC DMA timestamp (Intel X710, Mellanox ConnectX) |

See "SO_TIMESTAMPING validation" above for what was actually measured in
this environment (the software-timestamp path, on loopback) versus what
these accuracy figures represent (design targets / vendor specs).

Hardware timestamps are retrieved via `recvmsg()` SCM_TIMESTAMPING ancillary
data — the NIC records the timestamp at DMA time, before the packet reaches
the kernel networking stack, eliminating scheduler jitter entirely.

**Why this matters for latency measurement:**

Without hardware timestamping, a software receive timestamp can be delayed by
scheduler jitter — this repo's own measured `kernel_rx_sw_ts -> read` figure
above (p50 ~1.2µs, p99.9 ~18µs on an idle container) is a real instance of
exactly that jitter, on the low end of what a loaded production machine would
show. This is the same goal as kernel bypass (DPDK/RDMA) but achieved from
the kernel side: the packet still goes through the kernel stack, but the
timestamp is captured at the NIC before any kernel processing occurs.

**Production note:** Hardware timestamp support requires:
- Intel X710/X550/E810, Mellanox ConnectX-4/5/6, or Solarflare SFN8000+
- `ethtool -T <iface>` to verify `hardware-raw-clock` capability
- `SOF_TIMESTAMPING_RAW_HARDWARE` flag (not `SOF_TIMESTAMPING_SYS_HARDWARE`)

---

## Gap detection timeout and receive buffer sizing

**Gap timeout.** `GapBuffer::Config` defaults to `gap_timeout_ns` = 100 µs
and `retry_interval_ns` = 1 ms; the `receiver` binary overrides these to
200 µs / 2 ms. The timeout is a debounce: it trades a little recovery
latency for not firing retransmit requests on packets that are merely
reordered. The right value depends on the venue's observed reordering
distribution, which can't be measured here — **both defaults are
judgment calls, not tuned values.** What *is* tested is the mechanism:
`test_retransmit_request_timing` (configured at 500 µs) checks that no
request fires before the timeout and one fires after it.

**Receive buffer.** `MulticastReceiver` requests an 8 MB `SO_RCVBUF`.
How long that absorbs a stall is `buffer / (packet rate × per-packet
kernel cost)`, where per-packet cost is the datagram plus `skb`
overhead, typically well above the ITCH payload. Rather than quote a
duration from guessed inputs, the useful, checkable fact is this one:
**Linux silently clamps `SO_RCVBUF` to `net.core.rmem_max`.** Measured
in this container (`rmem_max` = 4 MB): an 8 MB `SO_RCVBUF` request was
granted 4 MB; `SO_RCVBUFFORCE` got the full 8 MB. On a stock kernel the
cap is ~208 KB. `open()` now tries `SO_RCVBUFFORCE`, falls back, reads
the granted size back, and exposes `recv_buffer_shortfall()`; the
`receiver` binary warns on it. See `docs/linux-tuning.md` §5a.


---

## Source-specific multicast (SSM)

`IP_ADD_SOURCE_MEMBERSHIP` has the kernel drop multicast from other
sources before it reaches the socket, so with SSM the application only
pays for the traffic it subscribed to. The size of that saving depends
entirely on what else is on the group, so **this repo makes no CPU
figure for it.** It also can't exercise live multicast at all: this
sandbox has no multicast routing on loopback (verified: a loopback
multicast send/receive delivered nothing). The socket-option code is
compiled and linked, but the live multicast data path, ASM and SSM
alike, still needs validation on a host with multicast routing or
against a real feed.
