# Loss Handling and Recovery

This documents what this receiver actually does when things go wrong, what
it deliberately leaves to the application layer, and what an operator
should do in each case. It's written as a runbook, not a feature list —
every section says what you'd see, what it means, and what to do about it.

---

## 1. The three UDP failure modes, and what handles each

| Failure | Detected by | Recovery |
|---|---|---|
| Packet loss | `GapBuffer`: `seq > next_expected` | Buffer + retransmit request (§2) |
| Packet duplication | `GapBuffer`: `seq < next_expected` | Dropped silently, counted (`stat_duplicates`) |
| Packet reordering | `GapBuffer`: out-of-order buffer | Buffered, flushed in order once the gap fills |
| **Correlated loss on one line** | N/A for a single receiver | `FeedArbitrator` — see §3 |

None of this is new information if you've read `gap_buffer.hpp`'s header
comment — this section is about what happens *around* that logic, which is
where the actual operational risk lives.

---

## 2. Single-line gap recovery, end to end

1. A gap is detected: `on_gap` fires once, with the missing range.
2. The gap sits in `GapBuffer`'s buffer for `gap_timeout_ns` (default
   200µs) — this is a debounce window, not a real recovery attempt: most
   gaps under ~100µs are reordering, not loss, and resolve themselves
   without a retransmit request ever being sent.
3. If still missing after the timeout, `on_retransmit` fires with a
   `RetransmitRequest` (session, first_seq, count). **This library does not
   send it.** Connecting to the retransmission server and sending the
   request over TCP is left to the application — deliberately, since it's
   the one piece of this pipeline that's genuinely venue-specific (host,
   port, auth, connection pooling all vary). Wire `on_retransmit` to your
   TCP client.
4. If the gap is still open after `retry_interval_ns` (default 1ms), the
   request is resent — capped at `max_retransmit_count` (default 100)
   sequences per request, so a gap larger than that is requested in
   multiple rounds.
5. **Buffer overflow.** The out-of-order buffer is a fixed 4096-slot ring
   (§ design note in `gap_buffer.hpp`). A gap larger than 4096 messages
   causes new packets to collide with not-yet-delivered ones in the
   buffer; `stat_buffer_overflows` increments and the colliding packet is
   dropped. **This is the one condition this receiver treats as
   irrecoverable in-place.** There is no in-process path back to a
   consistent state past this point — the buffer literally cannot hold
   enough context to reconstruct the missing range while continuing to
   accept new data.

   **What to do:** on `stat_buffer_overflows() > 0`, the application should
   stop trusting the current `GapBuffer`/`OrderBook` state, tear both down,
   and reconnect — see §4 for what "reconnect" actually needs to mean for
   the order book, not just the socket.

---

## 3. A/B feed lines change the shape of §2, not the logic

`FeedArbitrator` doesn't add new recovery logic — it shares one
`GapBuffer` across two lines, so whichever line delivers sequence N first
wins and the other's copy is a free duplicate-drop (see
`feed_arbitrator.hpp` for why that's correct with zero new state
machinery). The operational consequence:

- A loss on **one** line that the other line covers **never reaches §2 at
  all** — no gap event, no retransmit request, because from `GapBuffer`'s
  perspective the sequence just arrived a little late from the other
  socket. This is the entire value of running both lines instead of one.
- A gap only fires — and only then does §2 apply — when **both** lines are
  missing the same range. That's a genuine outage, not a switch hiccup,
  and should be treated with more urgency than a single-line gap would be.
- **Monitoring implication:** if you only alert on `stat_gaps()` /
  `stat_retransmit_requests()`, you will not see a line going dark as long
  as the other line covers for it — which is correct for *data
  correctness* but means you can lose real redundancy silently. Alert
  separately on `FeedArbitrator::line_looks_dark()` per line; by the time
  a real dual-line gap fires, you've already been running on single-line
  redundancy for however long the dark line has been down.

---

## 4. Order book state is a second thing that can be "gapped" —
and it's easy to miss

This is the least obvious failure mode in the whole pipeline, and it's not
a `GapBuffer` problem at all — it's inherent to how ITCH represents book
state, and it matters for anyone extending this repo's order book.

`GapBuffer` guarantees the *message stream* the application sees is
gap-free and ordered. It says nothing about whether that message stream
started at the *beginning of the session*. ITCH has no "give me the full
current book" message — the book is defined entirely as the accumulated
effect of every Add/Cancel/Delete/Executed/Replace since session open. If
a consumer's `GapBuffer` starts syncing at, say, sequence 4,000,000
because that's simply when the process started, `OrderBook` will build a
completely gap-free, internally-consistent — and **wrong** — book: every
order added before sequence 4,000,000 is invisible, `Cancel`/`Delete`
messages referencing those pre-existing orders show up as
`stat_unknown_order` (harmless to the process, but silently wrong for the
book), and the reconstructed top-of-book can be meaningfully different
from the real one.

**This will not show up as an error anywhere in this codebase.** No gap
fires, no stat trips, nothing crashes. The book is just quietly wrong from
depth on down, and possibly wrong at the top too if resting liquidity from
before startup was sitting inside the current best price.

**What to do:**
- Start the receiver before the session opens (before the exchange's
  first Add message), so `next_expected` syncs to the true first
  sequence — this is the only way this codebase gives you a genuinely
  correct book with zero extra work.
- If you must start mid-session (a restart after a crash, a redeploy),
  you need one of: (a) a venue-provided snapshot mechanism outside ITCH
  itself, or (b) a full retransmit from sequence 1 via the retransmission
  server described in §2 (works, but is exactly the "large gap" case in
  §2 — expect it to blow the 4096-slot buffer and need to be handled as a
  bulk backfill *before* switching the live `GapBuffer` over to real-time
  traffic, not as a live gap-fill).
- At minimum, log and monitor `next_expected()` at startup against a
  known session-open sequence number if your venue publishes one, so a
  mid-session cold start is a visible, alertable event rather than a
  silent one.

---

## 5. What's genuinely out of scope here (and why)

- **CPU pinning / `SCHED_FIFO`.** Belongs in the application's startup
  sequence, not the library — see `README.md`'s Production Considerations.
- **The retransmission TCP client itself.** Venue-specific; wire
  `on_retransmit` to yours.
- **Snapshot/refresh outside ITCH.** Not an ITCH concept at all — see §4.
- **DPDK / kernel bypass.** `GapBuffer::ingest()` takes a raw byte span
  and is transport-agnostic; swap the receive path, keep everything above
  it.

---

## 6. Testing this without a live feed

- `tools/gen_pcap` injects controlled gaps and duplicates into a synthetic
  PCAP file (`--gap-at`, `--gap-len`, `--dup-at`) — replay it through
  `receiver --replay` to exercise §2 end to end offline.
- `tests/test_gap_buffer.cpp` covers the gap/duplicate/reorder state
  machine directly.
- `tests/test_feed_arbitrator.cpp` covers §3: it specifically asserts that
  a one-line drop covered by the other line produces **zero** gap events
  and **zero** retransmit requests, and that a drop on both lines still
  produces exactly one real gap — i.e. it tests the *difference* single-vs
  dual-line makes, not just that arbitration runs without crashing.
- `tests/test_order_book.cpp`'s end-to-end test exercises §4's "book only
  sees what GapBuffer delivered" property directly, by checking that a
  cancel sitting behind an unfilled gap has visibly not been applied to
  the book until the gap closes.
