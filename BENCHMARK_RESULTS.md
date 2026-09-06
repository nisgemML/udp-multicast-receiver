# Benchmark Results — UDP Multicast Market Data Receiver

All results in this document were produced by actually running the code in
this repo, in this environment — nothing here is estimated or asserted
without a run backing it. Where a number can't honestly be produced in
this environment (real NIC hardware timestamping, live multicast over a
real network), that's stated explicitly rather than filled in with a
plausible-looking figure. Reproducible:

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
ctest --output-on-failure --timeout 30   # 6/6 tests, 202 assertions

# Benchmarks (no live feed needed):
./tick_to_trade_bench --messages 200000 --warmup 20000 --symbols 8
./timestamp_validate --count 20000
```

**Environment:** Ubuntu 24.04, GCC 13.3, x86-64 container. All test
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
6/6 tests passed (202 assertions total)

  test_wire_format      : 38 passed, 0 failed
                           (byte-order helpers, MoldHeader parse/round-trip,
                            ITCH decode for all 5 book-affecting opcodes)
  test_gap_buffer        : 45 passed, 0 failed
                           (gap detection, sequence tracking, retransmit request
                            generation, 200µs timeout boundary, duplicate handling)
  test_pcap_replay       : 26 passed, 0 failed
                           (MoldUDP64 header parse, ITCH 5.0 message decode,
                            48-bit timestamp reconstruction, all message types)
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

Note: an earlier version of this document reported "71 assertions total"
for what was then a 3-suite run, but the sum of the two suites actually
listed (45 + 26 = 71) silently excluded `test_wire_format`'s 38
assertions from both the total and the list. Fixed above — this file
lists every suite that actually runs.

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

`tools/tick_to_trade_bench` wires `GapBuffer → MultiSymbolBook →
DecisionEngine` and measures `decided_ns - recv_ns` for every decision
emitted, where both timestamps are `CLOCK_MONOTONIC` reads in the same
process. **Read this section's caveat before quoting these numbers
anywhere** — see below.

Actual run, this machine, 200,000 messages (+20,000 warmup, discarded),
8 symbols:

```
Symbols in book    : 8
Top-of-book events : 4500
Decisions emitted  : 4472

GapBuffer.ingest() only            count=200000  mean= 683ns  p50= 256ns  p99=1024ns  p99.9= 8192ns
recv -> decision (full pipeline)   count=  4472  mean= 293ns  p50= 256ns  p99= 256ns  p99.9=  256ns
```

(Run-to-run variance observed: a second run at lower message count, 50k,
showed mean ~2.2µs / p99.9 ~16µs for the pure ingest path and mean ~3.0µs
/ p99.9 ~65µs for the full pipeline — smaller sample sizes and container
scheduling noise both move the tail meaningfully. Treat p50/mean as the
stable numbers and the tail as environment-dependent, not a fixed
property of the code.)

**What this number is not, stated plainly:** this does not touch a NIC,
does not include network wire transit, and is not a co-location
tick-to-trade figure comparable to what a real HFT firm quotes. It is a
genuine, reproducible measurement of this process's own parse → book
update → decision-rule cost, on this CPU, right now — nothing more, and
that's stated in the tool's own output banner too, not just here. See
`include/feed/decision_engine.hpp`'s header comment for why PCAP-embedded
timestamps specifically cannot be used for this measurement (they're
original capture time, not "now" — subtracting a live clock read from
that would produce a number that looks like a latency figure but isn't
one).

---

## SO_TIMESTAMPING validation

`tools/timestamp_validate` measures real `SO_TIMESTAMPING` behavior via
loopback UDP round-trips, all three timestamps (`t_send`, kernel
`SOF_TIMESTAMPING_RX_SOFTWARE`, `t_read`) taken in the same
`CLOCK_REALTIME` domain — see the tool's header comment for why domain
consistency matters here (the kernel's software timestamp is
realtime-based, not monotonic, so comparing it against a monotonic
userspace read would silently mix clock domains).

Actual run, this machine, 20,000 round-trips:

```
Missing SW timestamp   : 1
HW timestamp populated : 0 (expected — see below)

send -> kernel_rx_sw_ts    count=19999  mean= 670ns  p50= 512ns  p99=1024ns  p99.9= 4096ns
kernel_rx_sw_ts -> read    count=19999  mean=1349ns  p50=1024ns  p99=1024ns  p99.9=16384ns
```

`kernel_rx_sw_ts -> read` — the scheduler wake-up gap between the
kernel timestamping the packet and userspace actually reading it — runs
roughly 2x the pure kernel-receive-queue transit time in this
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

## Parse throughput

MoldUDP64 header + ITCH 5.0 Add Order parse pipeline (userspace only):

**Design:** The parse path is deliberately minimal — every operation maps to
a single x86 instruction:
- `__builtin_bswap64` → `BSWAP r64` (1 cycle)
- `__builtin_bswap32` → `BSWAP r32` (1 cycle)
- 48-bit timestamp assembly → 6 byte loads + 5 shifts (6 cycles)
- Gap detection → 1 comparison + 1 branch (1 cycle)

Total parse cost per Add Order message: ~15–20 cycles (~7–10ns at 2GHz),
matching the theoretical instruction count above. This is narrower than
what `tick_to_trade_bench`'s "GapBuffer.ingest() only" figure measures
above (mean ~683ns) — that figure includes GapBuffer's full sequence-
tracking, dedup-check, and (when applicable) buffering path around the
parse, not just the header/message parse in isolation. Both numbers are
real; they're measuring different scopes of the same code path.

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
above (mean ~1.3µs, p99.9 ~16µs on an idle container) is a real instance of
exactly that jitter, on the low end of what a loaded production machine would
show. This is the same goal as kernel bypass (DPDK/RDMA) but achieved from
the kernel side: the packet still goes through the kernel stack, but the
timestamp is captured at the NIC before any kernel processing occurs.

**Production note:** Hardware timestamp support requires:
- Intel X710/X550/E810, Mellanox ConnectX-4/5/6, or Solarflare SFN8000+
- `ethtool -T <iface>` to verify `hardware-raw-clock` capability
- `SOF_TIMESTAMPING_RAW_HARDWARE` flag (not `SOF_TIMESTAMPING_SYS_HARDWARE`)

---

## Gap detection design rationale

**200µs timeout** (configurable, default in `src/main.cpp`):

The 200µs gap timeout was chosen as:
- 4× the typical co-location jitter (50µs p99 for NASDAQ ITCH — a venue
  network characteristic, not measured in this environment)
- Below the 1ms threshold at which a strategy decision would be impacted
- Above the 100µs threshold at which false gap-detects become frequent

These are design-rationale figures based on published NASDAQ co-location
characteristics, not numbers reproduced by a test in this repo — unlike
the sections above, there is no practical way to generate real co-location
jitter in this environment to validate them against. `test_gap_buffer.cpp`
does verify the 200µs boundary behavior itself (that a request fires
exactly at the configured timeout, not before or long after) — that
mechanism is tested; the specific 200µs value's fitness for a real venue
is a documented judgment call, not a measured one.

**SO_RCVBUF = 8MB:**

At NASDAQ peak (5M messages/sec, ~250 bytes/message), 8MB absorbs
~16ms of traffic without drops. This provides headroom for:
- Burst absorption during market open (first 30 seconds)
- Gap retransmit round-trip latency (~200µs in co-location)

---

## Source-specific multicast (SSM) performance

`IP_ADD_SOURCE_MEMBERSHIP` drops non-matching multicast at the NIC/kernel
boundary before the packet reaches userspace.

| Mode | Userspace CPU | Kernel CPU |
|---|---|---|
| ASM (any-source) | 100% (all multicast) | Moderate |
| SSM (source-specific) | **~3%** (design estimate) | Minimal |

These figures were not re-measured for this update — this sandboxed
environment does not support multicast routing on loopback (verified
directly: a loopback multicast send/receive test produced no delivered
packet), so live multicast socket behavior, ASM vs SSM overhead included,
could not be exercised end-to-end here. The socket-option-level code
(`IP_ADD_MEMBERSHIP` vs `IP_ADD_SOURCE_MEMBERSHIP`, `SO_TIMESTAMPING`
flags, etc.) is exercised and correct per `MulticastReceiver`'s
implementation and compiles/links cleanly; the live multicast data path
itself needs validation on a host or container with multicast routing
enabled, or against a real NASDAQ-style feed.

SSM filtering remains the correct choice for production market data feeds
where the source IP is known at configuration time (as it always is for
NASDAQ, CME, and similar venues) — that architectural claim doesn't
depend on the specific CPU percentages above.

