// test_order_book.cpp — Tests for ITCH 5.0 order book reconstruction.
//
// Covers:
//   1. Add → book has the order at the right price/side.
//   2. Delete removes it; Cancel partially reduces it; Executed fills it.
//   3. Replace atomically swaps order_ref/price/shares.
//   4. Price-level aggregation across multiple orders at the same price.
//   5. Best bid/ask tracking as the book changes.
//   6. Multi-symbol routing — a Cancel/Delete/Executed with no symbol field
//      reaches the right per-symbol book via the order_ref index.
//   7. Defensive handling: unknown order_ref, duplicate Add.
//   8. End-to-end: GapBuffer → MultiSymbolBook over a real MoldUDP64 stream.

#include "feed/order_book.hpp"
#include "feed/gap_buffer.hpp"
#include "feed/wire_format.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace feed;

static int passed = 0, failed = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        ++failed; } else { ++passed; } \
    } while(0)

// ── Message body builders (match the byte layouts in wire_format.hpp) ────────

static std::vector<uint8_t> make_add(uint64_t order_ref, char side, uint32_t shares,
                                      const char* stock8, uint32_t price, bool mpid = false) {
    std::vector<uint8_t> b(36, 0);
    b[0] = uint8_t(mpid ? 'F' : 'A');
    put_be64(b.data() + 7, order_ref);
    b[15] = uint8_t(side);
    put_be32(b.data() + 16, shares);
    std::memcpy(b.data() + 20, stock8, 8);
    put_be32(b.data() + 28, price);
    return b;
}

static std::vector<uint8_t> make_delete(uint64_t order_ref) {
    std::vector<uint8_t> b(19, 0);
    b[0] = uint8_t('D');
    put_be64(b.data() + 11, order_ref);
    return b;
}

static std::vector<uint8_t> make_cancel(uint64_t order_ref, uint32_t cancelled_shares) {
    std::vector<uint8_t> b(23, 0);
    b[0] = uint8_t('X');
    put_be64(b.data() + 11, order_ref);
    put_be32(b.data() + 19, cancelled_shares);
    return b;
}

static std::vector<uint8_t> make_executed(uint64_t order_ref, uint32_t exec_shares) {
    std::vector<uint8_t> b(31, 0);
    b[0] = uint8_t('E');
    put_be64(b.data() + 11, order_ref);
    put_be32(b.data() + 19, exec_shares);
    put_be64(b.data() + 23, 999ULL); // match number, unused by the book
    return b;
}

static std::vector<uint8_t> make_replace(uint64_t orig_ref, uint64_t new_ref,
                                          uint32_t shares, uint32_t price) {
    std::vector<uint8_t> b(35, 0);
    b[0] = uint8_t('U');
    put_be64(b.data() + 11, orig_ref);
    put_be64(b.data() + 19, new_ref);
    put_be32(b.data() + 27, shares);
    put_be32(b.data() + 31, price);
    return b;
}

static MoldMessage as_msg(const std::vector<uint8_t>& body, uint64_t seq = 1,
                           uint64_t recv_ns = 1000) {
    return MoldMessage{ uint16_t(body.size()), body.data(), seq, recv_ns };
}

// ── Single-symbol OrderBook ───────────────────────────────────────────────────

static void test_add_and_top() {
    OrderBook book;
    ItchAddOrder m;
    auto bid_body = make_add(1, 'B', 100, "AAPL    ", 1'500'000);
    CHECK(ItchAddOrder::parse(bid_body.data(), bid_body.size(), m), "parse ok");
    book.apply_add(m, 1000);

    auto ask_body = make_add(2, 'S', 200, "AAPL    ", 1'500'500);
    CHECK(ItchAddOrder::parse(ask_body.data(), ask_body.size(), m), "parse ok");
    book.apply_add(m, 1001);

    auto top = book.top();
    CHECK(top.bid.has_value() && top.bid->price == 1'500'000 && top.bid->shares == 100,
          "best bid after two adds");
    CHECK(top.ask.has_value() && top.ask->price == 1'500'500 && top.ask->shares == 200,
          "best ask after two adds");
    CHECK(!top.crossed(), "book not crossed");
    CHECK(book.resting_order_count() == 2, "two resting orders");
}

static void test_price_level_aggregation() {
    OrderBook book;
    ItchAddOrder m;
    for (uint64_t ref = 1; ref <= 3; ++ref) {
        auto body = make_add(ref, 'B', 100, "MSFT    ", 3'000'000);
        CHECK(ItchAddOrder::parse(body.data(), body.size(), m), "parse ok");
        book.apply_add(m, 1000);
    }
    auto top = book.top();
    CHECK(top.bid && top.bid->shares == 300, "three orders aggregate to 300 shares");
    CHECK(top.bid->orders == 3, "level order count is 3");
    CHECK(book.bid_level_count() == 1, "still one price level");
}

static void test_delete_removes_order_and_shrinks_level() {
    OrderBook book;
    ItchAddOrder am;
    auto b1 = make_add(1, 'B', 100, "MSFT    ", 3'000'000);
    CHECK(ItchAddOrder::parse(b1.data(), b1.size(), am), "parse ok");
    book.apply_add(am, 1000);
    auto b2 = make_add(2, 'B', 50, "MSFT    ", 3'000'000);
    CHECK(ItchAddOrder::parse(b2.data(), b2.size(), am), "parse ok");
    book.apply_add(am, 1001);

    ItchDeleteOrder dm;
    auto db = make_delete(1);
    CHECK(ItchDeleteOrder::parse(db.data(), db.size(), dm), "parse ok");
    book.apply_delete(dm, 1002);

    auto top = book.top();
    CHECK(top.bid && top.bid->shares == 50, "level shrinks to remaining order's shares");
    CHECK(book.resting_order_count() == 1, "one order left");
}

static void test_cancel_partial_reduction() {
    OrderBook book;
    ItchAddOrder am;
    auto ab = make_add(1, 'B', 100, "MSFT    ", 3'000'000);
    CHECK(ItchAddOrder::parse(ab.data(), ab.size(), am), "parse ok");
    book.apply_add(am, 1000);

    ItchOrderCancel cm;
    auto cb = make_cancel(1, 40);
    CHECK(ItchOrderCancel::parse(cb.data(), cb.size(), cm), "parse ok");
    book.apply_cancel(cm, 1001);

    auto top = book.top();
    CHECK(top.bid && top.bid->shares == 60, "cancel reduces shares by cancelled amount");
    CHECK(book.resting_order_count() == 1, "order still resting after partial cancel");

    // Cancel the rest — order should disappear.
    auto cb2 = make_cancel(1, 60);
    CHECK(ItchOrderCancel::parse(cb2.data(), cb2.size(), cm), "parse ok");
    book.apply_cancel(cm, 1002);
    CHECK(book.resting_order_count() == 0, "order removed once shares hit zero");
    CHECK(!book.top().bid.has_value(), "no bid left");
}

static void test_executed_partial_and_full_fill() {
    OrderBook book;
    ItchAddOrder am;
    auto ab = make_add(1, 'S', 100, "MSFT    ", 3'000'000);
    CHECK(ItchAddOrder::parse(ab.data(), ab.size(), am), "parse ok");
    book.apply_add(am, 1000);

    ItchOrderExecuted em;
    auto eb = make_executed(1, 30);
    CHECK(ItchOrderExecuted::parse(eb.data(), eb.size(), em), "parse ok");
    book.apply_executed(em, 1001);
    CHECK(book.top().ask && book.top().ask->shares == 70, "partial fill reduces resting shares");

    auto eb2 = make_executed(1, 70);
    CHECK(ItchOrderExecuted::parse(eb2.data(), eb2.size(), em), "parse ok");
    book.apply_executed(em, 1002);
    CHECK(!book.top().ask.has_value(), "fully filled order leaves the book");
    CHECK(book.stat_executes() == 2, "two executions recorded");
}

static void test_replace_swaps_ref_price_shares() {
    OrderBook book;
    ItchAddOrder am;
    auto ab = make_add(1, 'B', 100, "MSFT    ", 3'000'000);
    CHECK(ItchAddOrder::parse(ab.data(), ab.size(), am), "parse ok");
    book.apply_add(am, 1000);

    ItchOrderReplace rm;
    auto rb = make_replace(1, 2, 150, 3'000'500);
    CHECK(ItchOrderReplace::parse(rb.data(), rb.size(), rm), "parse ok");
    book.apply_replace(rm, 1001);

    CHECK(book.resting_order_count() == 1, "replace keeps exactly one resting order");
    auto top = book.top();
    CHECK(top.bid && top.bid->price == 3'000'500 && top.bid->shares == 150,
          "replace moved price and updated shares");

    // The old order_ref should no longer be cancellable.
    ItchOrderCancel cm;
    auto cb = make_cancel(1, 10);
    CHECK(ItchOrderCancel::parse(cb.data(), cb.size(), cm), "parse ok");
    book.apply_cancel(cm, 1002);
    CHECK(book.stat_unknown_order() == 1, "cancelling the replaced-away ref is rejected");
}

static void test_defensive_unknown_and_duplicate() {
    OrderBook book;
    ItchDeleteOrder dm;
    auto db = make_delete(999);
    CHECK(ItchDeleteOrder::parse(db.data(), db.size(), dm), "parse ok");
    book.apply_delete(dm, 1000);
    CHECK(book.stat_unknown_order() == 1, "delete of unknown order_ref is counted, not fatal");

    ItchAddOrder am;
    auto ab = make_add(1, 'B', 100, "MSFT    ", 3'000'000);
    CHECK(ItchAddOrder::parse(ab.data(), ab.size(), am), "parse ok");
    book.apply_add(am, 1000);
    book.apply_add(am, 1001); // duplicate order_ref
    CHECK(book.stat_duplicate_add() == 1, "duplicate Add is rejected, not silently overwritten");
    CHECK(book.resting_order_count() == 1, "book state unaffected by the duplicate");
}

static void test_top_of_book_callback_fires_on_change_only() {
    OrderBook book;
    int fires = 0;
    book.set_on_top_change([&](const TopOfBook&, uint64_t) { ++fires; });

    ItchAddOrder am;
    auto ab = make_add(1, 'B', 100, "MSFT    ", 3'000'000);
    CHECK(ItchAddOrder::parse(ab.data(), ab.size(), am), "parse ok");
    book.apply_add(am, 1000);
    CHECK(fires == 1, "first add fires top-of-book change");

    // A second order behind the best price does not change top-of-book price,
    // but does change aggregate size at that price — still a real top change.
    auto ab2 = make_add(2, 'B', 50, "MSFT    ", 3'000'000);
    CHECK(ItchAddOrder::parse(ab2.data(), ab2.size(), am), "parse ok");
    book.apply_add(am, 1001);
    CHECK(fires == 2, "size change at best price still fires");

    // Adding behind the market at a worse price should NOT fire.
    auto ab3 = make_add(3, 'B', 50, "MSFT    ", 2'999'000);
    CHECK(ItchAddOrder::parse(ab3.data(), ab3.size(), am), "parse ok");
    book.apply_add(am, 1002);
    CHECK(fires == 2, "add at a non-top price does not fire top-of-book callback");
}

// ── Multi-symbol routing ──────────────────────────────────────────────────────

static void test_multi_symbol_routing() {
    MultiSymbolBook mb;

    auto aapl = make_add(1, 'B', 100, "AAPL    ", 1'500'000);
    CHECK(mb.ingest(as_msg(aapl, 1)), "AAPL add ingested");

    auto msft = make_add(2, 'B', 200, "MSFT    ", 3'000'000);
    CHECK(mb.ingest(as_msg(msft, 2)), "MSFT add ingested");

    CHECK(mb.symbol_count() == 2, "two symbols tracked");

    // Cancel order 1 (AAPL) — carries no symbol, must route via index.
    auto cancel = make_cancel(1, 40);
    CHECK(mb.ingest(as_msg(cancel, 3)), "cancel ingested");

    const OrderBook* aapl_book = mb.book_for("AAPL");
    const OrderBook* msft_book = mb.book_for("MSFT");
    CHECK(aapl_book != nullptr && msft_book != nullptr, "both books exist");
    CHECK(aapl_book->top().bid && aapl_book->top().bid->shares == 60,
          "cancel routed to AAPL book, not MSFT");
    CHECK(msft_book->top().bid && msft_book->top().bid->shares == 200,
          "MSFT book untouched by AAPL cancel");
    CHECK(mb.stat_unrouted() == 0, "no unrouted messages");

    // Delete order 2 (MSFT), then a further cancel on it should be unrouted.
    auto del = make_delete(2);
    mb.ingest(as_msg(del, 4));
    auto stray_cancel = make_cancel(2, 10);
    mb.ingest(as_msg(stray_cancel, 5));
    CHECK(mb.stat_unrouted() == 1, "cancel after delete is correctly unrouted, not misapplied");
}

static void test_multi_symbol_top_change_callback_includes_symbol() {
    MultiSymbolBook mb;
    std::vector<std::string> fired_symbols;
    mb.set_on_top_change([&](std::string_view sym, const TopOfBook&, uint64_t) {
        fired_symbols.emplace_back(sym);
    });

    auto aapl = make_add(1, 'B', 100, "AAPL    ", 1'500'000);
    mb.ingest(as_msg(aapl, 1));
    auto msft = make_add(2, 'B', 200, "MSFT    ", 3'000'000);
    mb.ingest(as_msg(msft, 2));

    CHECK(fired_symbols.size() == 2, "one top-change callback per symbol's first add");
    CHECK(fired_symbols[0] == "AAPL" && fired_symbols[1] == "MSFT",
          "callback reports the correct symbol per book");
}

// ── End-to-end: GapBuffer → MultiSymbolBook over a real MoldUDP64 stream ─────

static std::vector<uint8_t> mold_packet(const char* session, uint64_t seq,
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

static void test_end_to_end_gap_buffer_to_book() {
    GapBuffer::Config cfg;
    std::memcpy(cfg.session, "TESTSESS  ", 10);
    GapBuffer gb(cfg);
    MultiSymbolBook mb;
    gb.set_on_message([&](const MoldMessage& m) { mb.ingest(m); });

    auto add1 = make_add(1, 'B', 100, "AAPL    ", 1'500'000);
    auto add2 = make_add(2, 'S', 100, "AAPL    ", 1'500'500);
    auto cancel = make_cancel(1, 50);

    uint64_t ts = 1000;
    gb.ingest(mold_packet("TESTSESS  ", 1, add1).data(),
              mold_packet("TESTSESS  ", 1, add1).size(), ts++);
    gb.ingest(mold_packet("TESTSESS  ", 2, add2).data(),
              mold_packet("TESTSESS  ", 2, add2).size(), ts++);
    // seq 4 arrives before seq 3 — out-of-order, buffered by GapBuffer.
    gb.ingest(mold_packet("TESTSESS  ", 4, cancel).data(),
              mold_packet("TESTSESS  ", 4, cancel).size(), ts++);

    const OrderBook* book = mb.book_for("AAPL");
    CHECK(book != nullptr, "book created from live stream");
    CHECK(book->top().bid && book->top().bid->shares == 100,
          "cancel (seq 4) not yet applied — still buffered behind missing seq 3");

    // A real seq-3 message (heartbeat-equivalent no-op via a second Add at a
    // worse price) fills the gap; GapBuffer should now flush seq 4 too.
    auto filler = make_add(3, 'B', 10, "AAPL    ", 1'000'000);
    gb.ingest(mold_packet("TESTSESS  ", 3, filler).data(),
              mold_packet("TESTSESS  ", 3, filler).size(), ts++);

    CHECK(book->top().bid && book->top().bid->shares == 50,
          "cancel applied once GapBuffer delivers it in order (seq 3 filled the gap)");
    CHECK(gb.stat_gaps() == 1, "exactly one gap was detected during the reorder");
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    test_add_and_top();
    test_price_level_aggregation();
    test_delete_removes_order_and_shrinks_level();
    test_cancel_partial_reduction();
    test_executed_partial_and_full_fill();
    test_replace_swaps_ref_price_shares();
    test_defensive_unknown_and_duplicate();
    test_top_of_book_callback_fires_on_change_only();
    test_multi_symbol_routing();
    test_multi_symbol_top_change_callback_includes_symbol();
    test_end_to_end_gap_buffer_to_book();

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
