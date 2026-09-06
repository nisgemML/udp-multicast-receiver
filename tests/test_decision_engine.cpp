// test_decision_engine.cpp — Correctness tests for DecisionEngine.
//
// This intentionally does NOT assert on latency numbers — a unit test
// process under a test harness is not a representative environment for
// timing, and hard-coding a "must be under Xns" assertion here would
// either be flaky or meaningless. Latency is measured for real by
// tools/tick_to_trade_bench.cpp instead. What's tested here is the
// decision *logic*: does it fire when it should, skip when it shouldn't,
// and record a latency sample whenever it does fire.

#include "feed/decision_engine.hpp"
#include "feed/order_book.hpp"

#include <cassert>
#include <cstdio>
#include <vector>

using namespace feed;

static int passed = 0, failed = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++failed; } else { ++passed; } \
    } while(0)

static void test_no_decision_without_both_sides() {
    DecisionEngine eng;
    int fired = 0;
    eng.set_on_decision([&](const Decision&) { ++fired; });

    TopOfBook t; // no bid, no ask
    eng.on_top_of_book("AAPL", t, 1000);
    CHECK(fired == 0, "no decision with an empty book");

    t.bid = PriceLevel{ 1'500'000, 100, 1 };
    eng.on_top_of_book("AAPL", t, 1000);
    CHECK(fired == 0, "no decision with only a bid");
}

static void test_no_decision_when_spread_too_tight() {
    DecisionEngine::Config cfg;
    cfg.min_spread_ticks = 2;
    cfg.tick_size        = 100;
    DecisionEngine eng(cfg);
    int fired = 0;
    eng.set_on_decision([&](const Decision&) { ++fired; });

    TopOfBook t;
    t.bid = PriceLevel{ 1'500'000, 100, 1 };
    t.ask = PriceLevel{ 1'500'100, 100, 1 }; // 1-tick spread, below the 2-tick filter
    eng.on_top_of_book("AAPL", t, 1000);
    CHECK(fired == 0, "spread at exactly the filter threshold does not fire");
}

static void test_decision_fires_on_wide_spread() {
    DecisionEngine::Config cfg;
    cfg.min_spread_ticks = 2;
    cfg.tick_size        = 100;
    DecisionEngine eng(cfg);
    std::vector<Decision> decisions;
    eng.set_on_decision([&](const Decision& d) { decisions.push_back(d); });

    TopOfBook t;
    t.bid = PriceLevel{ 1'500'000, 100, 1 };
    t.ask = PriceLevel{ 1'500'500, 100, 1 }; // 5-tick spread
    const uint64_t recv_ns = DecisionEngine::monotonic_ns();
    eng.on_top_of_book("AAPL", t, recv_ns);

    CHECK(decisions.size() == 1, "wide spread fires exactly one decision");
    CHECK(decisions[0].symbol == "AAPL", "decision carries the right symbol");
    CHECK(decisions[0].price == 1'500'100, "quotes one tick inside the current best bid");
    CHECK(decisions[0].side == 'B', "this toy rule always quotes the bid side");
    CHECK(decisions[0].recv_ns == recv_ns, "decision preserves the triggering recv_ns");
    CHECK(decisions[0].decided_ns >= recv_ns, "decision timestamp is not before arrival");
    CHECK(eng.decisions_emitted() == 1, "decisions_emitted counter matches");
}

static void test_latency_recorded_only_with_real_timestamps() {
    DecisionEngine eng;
    TopOfBook t;
    t.bid = PriceLevel{ 1'500'000, 100, 1 };
    t.ask = PriceLevel{ 1'500'500, 100, 1 };

    // recv_ns = 0 is what an un-timestamped test/replay path would pass;
    // decided_ns (a real monotonic "now") will always be far larger, so
    // this WOULD record a sample under a naive "decided - recv" — that's
    // fine and expected here since decided_ns > 0, but the point of the
    // guard is future callers passing recv_ns > decided_ns (clock skew,
    // misuse) don't corrupt the histogram with underflowed uint64 deltas.
    eng.on_top_of_book("AAPL", t, 0);
    CHECK(eng.latency().count() == 1, "valid recv_ns=0 still records one sample");

    DecisionEngine eng2;
    eng2.on_top_of_book("AAPL", t, UINT64_MAX); // recv_ns far in the "future"
    CHECK(eng2.latency().count() == 0,
          "recv_ns after decided_ns is rejected, not recorded as underflow garbage");
}

// ── End-to-end: OrderBook top-of-book changes drive DecisionEngine ───────────

static void test_wired_to_order_book() {
    OrderBook book;
    DecisionEngine eng;
    std::vector<Decision> decisions;
    eng.set_on_decision([&](const Decision& d) { decisions.push_back(d); });
    book.set_on_top_change([&](const TopOfBook& t, uint64_t recv_ns) {
        eng.on_top_of_book("AAPL", t, recv_ns);
    });

    ItchAddOrder bid{};
    bid.side = 'B'; bid.price = 1'500'000; bid.shares = 100; bid.order_ref = 1;
    book.apply_add(bid, DecisionEngine::monotonic_ns());
    CHECK(decisions.empty(), "one-sided book does not trigger a decision");

    ItchAddOrder ask{};
    ask.side = 'S'; ask.price = 1'500'500; ask.shares = 100; ask.order_ref = 2;
    book.apply_add(ask, DecisionEngine::monotonic_ns());
    CHECK(decisions.size() == 1, "adding the ask completes the book and fires a decision");
    CHECK(eng.top_changes_seen() == 2, "engine observed both top-of-book updates");
}

int main() {
    test_no_decision_without_both_sides();
    test_no_decision_when_spread_too_tight();
    test_decision_fires_on_wide_spread();
    test_latency_recorded_only_with_real_timestamps();
    test_wired_to_order_book();

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
