#include "ladder_order_book.hpp"
#include "order_book.hpp"
#include "pooled_order_book.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <limits>
#include <vector>

namespace {

lob::Event add(lob::OrderId id, lob::Side side, lob::Price price, lob::Quantity qty) {
  lob::Event event;
  event.type = lob::EventType::Add;
  event.order_id = id;
  event.side = side;
  event.price = price;
  event.qty = qty;
  return event;
}

lob::Event cancel(lob::OrderId id) {
  lob::Event event;
  event.type = lob::EventType::Cancel;
  event.order_id = id;
  return event;
}

lob::Event modify(lob::OrderId id, lob::Price price, lob::Quantity qty) {
  lob::Event event;
  event.type = lob::EventType::Modify;
  event.order_id = id;
  event.new_price = price;
  event.new_qty = qty;
  return event;
}

}  // namespace

TEST_CASE(maximum_valid_values_remain_observable) {
  lob::OrderBook book;
  std::vector<lob::Trade> trades;
  const lob::Quantity max_qty = std::numeric_limits<lob::Quantity>::max();
  const lob::Price max_price = std::numeric_limits<lob::Price>::max();

  CHECK_EQ(static_cast<int>(book.process(add(1, lob::Side::Bid, max_price, max_qty), trades)),
           static_cast<int>(lob::BookError::None));
  CHECK_EQ(book.top_of_book(lob::Side::Bid)->price, max_price);
  CHECK_EQ(book.top_of_book(lob::Side::Bid)->qty, max_qty);
}

TEST_CASE(quantity_overflow_rejection_preserves_the_existing_level) {
  lob::OrderBook book;
  std::vector<lob::Trade> trades;
  const lob::Quantity max_qty = std::numeric_limits<lob::Quantity>::max();

  CHECK_EQ(static_cast<int>(book.add_order(1, lob::Side::Bid, 100, max_qty, trades)),
           static_cast<int>(lob::BookError::None));
  CHECK_EQ(static_cast<int>(book.add_order(2, lob::Side::Bid, 100, 1, trades)),
           static_cast<int>(lob::BookError::QuantityOverflow));
  CHECK_EQ(book.live_order_count(), static_cast<std::size_t>(1));
  CHECK_EQ(book.top_of_book(lob::Side::Bid)->qty, max_qty);
  CHECK_TRUE(trades.empty());
}

template <typename Engine>
void check_quantity_overflow_uses_actual_crossing_quantity() {
  Engine book;
  book.reserve_orders(8);
  std::vector<lob::Trade> trades;
  const lob::Quantity max_qty = std::numeric_limits<lob::Quantity>::max();

  CHECK_EQ(static_cast<int>(book.add_order(1, lob::Side::Ask, 100, max_qty - 1, trades)),
           static_cast<int>(lob::BookError::None));
  CHECK_EQ(static_cast<int>(book.add_order(2, lob::Side::Bid, 100, max_qty - 1, trades)),
           static_cast<int>(lob::BookError::None));
  CHECK_EQ(book.stats().traded_qty, max_qty - 1);

  CHECK_EQ(static_cast<int>(book.add_order(3, lob::Side::Bid, 1, max_qty, trades)),
           static_cast<int>(lob::BookError::None));
  CHECK_EQ(book.top_of_book(lob::Side::Bid)->qty, max_qty);

  CHECK_EQ(static_cast<int>(book.add_order(4, lob::Side::Ask, 1, 2, trades)),
           static_cast<int>(lob::BookError::QuantityOverflow));
  CHECK_EQ(book.top_of_book(lob::Side::Bid)->qty, max_qty);
  CHECK_EQ(book.stats().traded_qty, max_qty - 1);
}

TEST_CASE(quantity_overflow_uses_actual_crossing_quantity_for_all_backends) {
  check_quantity_overflow_uses_actual_crossing_quantity<lob::OrderBook>();
  check_quantity_overflow_uses_actual_crossing_quantity<lob::PooledOrderBook>();
  check_quantity_overflow_uses_actual_crossing_quantity<lob::LadderOrderBook>();
}

TEST_CASE(thousands_of_orders_at_one_price_preserve_fifo_and_cancel) {
  lob::OrderBook book;
  book.reserve_orders(2000);
  std::vector<lob::Trade> trades;

  for (lob::OrderId id = 1; id <= 1000; ++id) {
    CHECK_EQ(static_cast<int>(book.add_order(id, lob::Side::Bid, 100, 1, trades)),
             static_cast<int>(lob::BookError::None));
  }
  CHECK_EQ(book.top_of_book(lob::Side::Bid)->order_count, static_cast<std::size_t>(1000));

  CHECK_EQ(static_cast<int>(book.cancel_order(500)), static_cast<int>(lob::BookError::None));
  CHECK_EQ(book.top_of_book(lob::Side::Bid)->order_count, static_cast<std::size_t>(999));
  CHECK_EQ(static_cast<int>(book.add_order(2000, lob::Side::Ask, 100, 999, trades)),
           static_cast<int>(lob::BookError::None));
  CHECK_EQ(trades.front().resting_order_id, static_cast<lob::OrderId>(1));
  CHECK_EQ(trades.back().resting_order_id, static_cast<lob::OrderId>(1000));
}

TEST_CASE(partial_fill_then_cancel_and_modify_use_remaining_quantity) {
  lob::OrderBook book;
  std::vector<lob::Trade> trades;

  CHECK_EQ(static_cast<int>(book.add_order(1, lob::Side::Bid, 100, 10, trades)),
           static_cast<int>(lob::BookError::None));
  CHECK_EQ(static_cast<int>(book.add_order(2, lob::Side::Ask, 100, 4, trades)),
           static_cast<int>(lob::BookError::None));
  CHECK_EQ(static_cast<int>(book.modify_order(1, 99, 3, trades)),
           static_cast<int>(lob::BookError::None));
  CHECK_EQ(book.top_of_book(lob::Side::Bid)->price, 99);
  CHECK_EQ(book.top_of_book(lob::Side::Bid)->qty, 3);
  CHECK_EQ(static_cast<int>(book.cancel_order(1)), static_cast<int>(lob::BookError::None));
  CHECK_TRUE(!book.top_of_book(lob::Side::Bid).has_value());
}

TEST_CASE(filled_order_id_can_be_reused) {
  lob::OrderBook book;
  std::vector<lob::Trade> trades;

  CHECK_EQ(static_cast<int>(book.add_order(1, lob::Side::Bid, 100, 1, trades)),
           static_cast<int>(lob::BookError::None));
  CHECK_EQ(static_cast<int>(book.add_order(2, lob::Side::Ask, 100, 1, trades)),
           static_cast<int>(lob::BookError::None));
  CHECK_EQ(static_cast<int>(book.add_order(1, lob::Side::Ask, 101, 2, trades)),
           static_cast<int>(lob::BookError::None));
  CHECK_EQ(book.live_order_count(), static_cast<std::size_t>(1));
  CHECK_EQ(book.top_of_book(lob::Side::Ask)->price, 101);
}

TEST_CASE(repeated_cancel_replace_keeps_one_live_order) {
  lob::OrderBook book;
  std::vector<lob::Trade> trades;
  CHECK_EQ(static_cast<int>(book.add_order(1, lob::Side::Bid, 100, 1, trades)),
           static_cast<int>(lob::BookError::None));

  for (lob::Price price = 101; price < 151; ++price) {
    CHECK_EQ(static_cast<int>(book.modify_order(1, price, 1, trades)),
             static_cast<int>(lob::BookError::None));
  }
  CHECK_EQ(book.live_order_count(), static_cast<std::size_t>(1));
  CHECK_EQ(book.top_of_book(lob::Side::Bid)->price, 150);
}

TEST_CASE(empty_book_transitions_survive_repeated_sweeps) {
  lob::OrderBook book;
  std::vector<lob::Trade> trades;
  for (lob::OrderId round = 0; round < 100; ++round) {
    const lob::OrderId bid_id = round * 2 + 1;
    const lob::OrderId ask_id = round * 2 + 2;
    CHECK_EQ(static_cast<int>(book.process(add(bid_id, lob::Side::Bid, 100, 2), trades)),
             static_cast<int>(lob::BookError::None));
    CHECK_EQ(static_cast<int>(book.process(add(ask_id, lob::Side::Ask, 99, 2), trades)),
             static_cast<int>(lob::BookError::None));
    CHECK_TRUE(!book.top_of_book(lob::Side::Bid).has_value());
    CHECK_TRUE(!book.top_of_book(lob::Side::Ask).has_value());
  }
}

TEST_CASE(cancelling_filled_and_unknown_ids_is_rejected_without_state_change) {
  lob::OrderBook book;
  std::vector<lob::Trade> trades;
  CHECK_EQ(static_cast<int>(book.process(add(1, lob::Side::Bid, 100, 1), trades)),
           static_cast<int>(lob::BookError::None));
  CHECK_EQ(static_cast<int>(book.process(add(2, lob::Side::Ask, 100, 1), trades)),
           static_cast<int>(lob::BookError::None));
  const std::size_t live_orders = book.live_order_count();
  CHECK_EQ(static_cast<int>(book.process(cancel(1), trades)),
           static_cast<int>(lob::BookError::UnknownOrderId));
  CHECK_EQ(static_cast<int>(book.process(cancel(999), trades)),
           static_cast<int>(lob::BookError::UnknownOrderId));
  CHECK_EQ(book.live_order_count(), live_orders);
}
