#include "order_book.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace {

class TestRng {
 public:
  explicit TestRng(std::uint64_t seed) : state_(seed == 0 ? 0x9e3779b97f4a7c15ULL : seed) {}

  std::uint64_t next() {
    std::uint64_t value = state_;
    value ^= value << 13U;
    value ^= value >> 7U;
    value ^= value << 17U;
    state_ = value;
    return value;
  }

  std::size_t uniform(std::size_t bound) {
    return bound == 0 ? 0 : static_cast<std::size_t>(next() % bound);
  }

 private:
  std::uint64_t state_;
};

struct SequenceResult {
  std::vector<lob::BookError> errors;
  std::vector<lob::Trade> trades;
  std::string book;
  lob::EngineStats stats;
};

lob::Event make_add(lob::OrderId order_id, lob::Side side, lob::Price price, lob::Quantity qty) {
  lob::Event event;
  event.type = lob::EventType::Add;
  event.order_id = order_id;
  event.side = side;
  event.price = price;
  event.qty = qty;
  return event;
}

lob::Event make_cancel(lob::OrderId order_id) {
  lob::Event event;
  event.type = lob::EventType::Cancel;
  event.order_id = order_id;
  return event;
}

lob::Event make_modify(lob::OrderId order_id, lob::Price price, lob::Quantity qty) {
  lob::Event event;
  event.type = lob::EventType::Modify;
  event.order_id = order_id;
  event.new_price = price;
  event.new_qty = qty;
  return event;
}

std::vector<lob::Event> generate_events(std::uint64_t seed, std::size_t count) {
  TestRng rng(seed);
  std::vector<lob::Event> events;
  events.reserve(count);
  std::vector<lob::OrderId> candidates;
  lob::OrderId next_order_id = 1;

  for (std::size_t index = 0; index < count; ++index) {
    const std::size_t operation = rng.uniform(10);
    const lob::Side side = (rng.next() & 1U) == 0U ? lob::Side::Bid : lob::Side::Ask;
    const lob::Price price = 98 + static_cast<lob::Price>(rng.uniform(5));
    const lob::Quantity qty = 1 + static_cast<lob::Quantity>(rng.uniform(20));

    if (operation < 5 || candidates.empty()) {
      const lob::OrderId order_id = next_order_id++;
      events.push_back(make_add(order_id, side, price, qty));
      candidates.push_back(order_id);
    } else {
      const lob::OrderId order_id = candidates[rng.uniform(candidates.size())];
      if (operation < 7) {
        events.push_back(make_cancel(order_id));
      } else if (operation == 7) {
        events.push_back(make_modify(order_id, price, qty));
      } else if (operation == 8) {
        events.push_back(make_modify(order_id, 0, qty));
      } else {
        events.push_back(make_add(order_id, side, price, qty));
      }
    }
  }
  return events;
}

void check_invariants(const lob::OrderBook& book) {
  const std::vector<lob::LevelSnapshot> bids = book.levels(lob::Side::Bid);
  const std::vector<lob::LevelSnapshot> asks = book.levels(lob::Side::Ask);
  std::size_t visible_orders = 0;

  for (std::size_t index = 0; index < bids.size(); ++index) {
    CHECK_TRUE(bids[index].price > 0);
    CHECK_TRUE(bids[index].qty > 0);
    CHECK_TRUE(bids[index].order_count > 0);
    visible_orders += bids[index].order_count;
    if (index > 0) {
      CHECK_TRUE(bids[index - 1].price > bids[index].price);
    }
  }

  for (std::size_t index = 0; index < asks.size(); ++index) {
    CHECK_TRUE(asks[index].price > 0);
    CHECK_TRUE(asks[index].qty > 0);
    CHECK_TRUE(asks[index].order_count > 0);
    visible_orders += asks[index].order_count;
    if (index > 0) {
      CHECK_TRUE(asks[index - 1].price < asks[index].price);
    }
  }

  if (!bids.empty() && !asks.empty()) {
    CHECK_TRUE(bids.front().price < asks.front().price);
  }
  CHECK_EQ(book.live_order_count(), visible_orders);
}

SequenceResult run_sequence(const std::vector<lob::Event>& events) {
  lob::OrderBook book;
  book.reserve_orders(events.size());
  SequenceResult result;
  result.errors.reserve(events.size());
  result.trades.reserve(events.size());

  for (const lob::Event& event : events) {
    result.errors.push_back(book.process(event, result.trades));
    check_invariants(book);
    for (const lob::Trade& trade : result.trades) {
      CHECK_TRUE(trade.qty > 0);
      CHECK_TRUE(trade.price > 0);
    }
  }

  result.book = book.format_book();
  result.stats = book.stats();
  return result;
}

void check_same_result(const SequenceResult& left, const SequenceResult& right) {
  CHECK_EQ(left.errors.size(), right.errors.size());
  for (std::size_t index = 0; index < left.errors.size(); ++index) {
    CHECK_EQ(static_cast<int>(left.errors[index]), static_cast<int>(right.errors[index]));
  }

  CHECK_EQ(left.trades.size(), right.trades.size());
  for (std::size_t index = 0; index < left.trades.size(); ++index) {
    CHECK_EQ(left.trades[index].resting_order_id, right.trades[index].resting_order_id);
    CHECK_EQ(left.trades[index].incoming_order_id, right.trades[index].incoming_order_id);
    CHECK_EQ(left.trades[index].price, right.trades[index].price);
    CHECK_EQ(left.trades[index].qty, right.trades[index].qty);
    CHECK_EQ(static_cast<int>(left.trades[index].aggressor_side),
             static_cast<int>(right.trades[index].aggressor_side));
  }

  CHECK_EQ(left.book, right.book);
  CHECK_EQ(left.stats.add_requests, right.stats.add_requests);
  CHECK_EQ(left.stats.cancel_requests, right.stats.cancel_requests);
  CHECK_EQ(left.stats.modify_requests, right.stats.modify_requests);
  CHECK_EQ(left.stats.rejected_requests, right.stats.rejected_requests);
  CHECK_EQ(left.stats.trades, right.stats.trades);
  CHECK_EQ(left.stats.traded_qty, right.stats.traded_qty);
}

}  // namespace

TEST_CASE(random_event_sequences_preserve_book_invariants) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    const std::vector<lob::Event> events = generate_events(seed, 500);
    run_sequence(events);
  }
}

TEST_CASE(random_event_sequences_are_deterministic) {
  const std::vector<lob::Event> events = generate_events(0x12345678ULL, 1000);
  const SequenceResult first = run_sequence(events);
  const SequenceResult second = run_sequence(events);
  check_same_result(first, second);
}
