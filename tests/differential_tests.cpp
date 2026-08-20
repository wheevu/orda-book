#include "order_book.hpp"
#include "reference_order_book.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <vector>

namespace {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9e3779b97f4a7c15ULL : seed) {}

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

std::vector<lob::Event> generate_history(std::uint64_t seed, std::size_t count) {
  Rng rng(seed);
  std::vector<lob::Event> events;
  std::vector<lob::OrderId> known_ids;
  lob::OrderId next_id = 1;
  events.reserve(count);

  for (std::size_t index = 0; index < count; ++index) {
    const std::size_t operation = rng.uniform(12);
    const lob::Side side = (rng.next() & 1U) == 0U ? lob::Side::Bid : lob::Side::Ask;
    const lob::Price price = 95 + static_cast<lob::Price>(rng.uniform(11));
    const lob::Quantity qty = 1 + static_cast<lob::Quantity>(rng.uniform(32));

    if (operation < 5 || known_ids.empty()) {
      const lob::OrderId id = next_id++;
      known_ids.push_back(id);
      events.push_back(add(id, side, price, qty));
    } else {
      const lob::OrderId id = known_ids[rng.uniform(known_ids.size())];
      if (operation < 7) {
        events.push_back(cancel(id));
      } else if (operation < 9) {
        events.push_back(modify(id, price, qty));
      } else if (operation == 9) {
        events.push_back(modify(id, 0, qty));
      } else if (operation == 10) {
        events.push_back(add(id, side, price, qty));
      } else {
        events.push_back(cancel(next_id + 1000000));
      }
    }
  }
  return events;
}

void compare_trades(const std::vector<lob::Trade>& actual,
                    const std::vector<lob::Trade>& expected) {
  CHECK_EQ(actual.size(), expected.size());
  for (std::size_t index = 0; index < actual.size(); ++index) {
    CHECK_EQ(actual[index].resting_order_id, expected[index].resting_order_id);
    CHECK_EQ(actual[index].incoming_order_id, expected[index].incoming_order_id);
    CHECK_EQ(actual[index].price, expected[index].price);
    CHECK_EQ(actual[index].qty, expected[index].qty);
    CHECK_EQ(static_cast<int>(actual[index].aggressor_side),
             static_cast<int>(expected[index].aggressor_side));
  }
}

void compare_orders(const std::vector<lob::OrderSnapshot>& actual,
                    const std::vector<lob::OrderSnapshot>& expected) {
  CHECK_EQ(actual.size(), expected.size());
  for (std::size_t index = 0; index < actual.size(); ++index) {
    CHECK_EQ(actual[index].order_id, expected[index].order_id);
    CHECK_EQ(static_cast<int>(actual[index].side), static_cast<int>(expected[index].side));
    CHECK_EQ(actual[index].price, expected[index].price);
    CHECK_EQ(actual[index].qty, expected[index].qty);
  }
}

void compare_stats(const lob::EngineStats& actual, const lob::EngineStats& expected) {
  CHECK_EQ(actual.add_requests, expected.add_requests);
  CHECK_EQ(actual.cancel_requests, expected.cancel_requests);
  CHECK_EQ(actual.modify_requests, expected.modify_requests);
  CHECK_EQ(actual.rejected_requests, expected.rejected_requests);
  CHECK_EQ(actual.trades, expected.trades);
  CHECK_EQ(actual.traded_qty, expected.traded_qty);
}

void compare_after_event(const lob::OrderBook& actual,
                         const reference_lob::OrderBook& expected) {
  compare_orders(actual.orders(lob::Side::Bid), expected.orders(lob::Side::Bid));
  compare_orders(actual.orders(lob::Side::Ask), expected.orders(lob::Side::Ask));
  CHECK_EQ(actual.live_order_count(), expected.live_order_count());
  compare_stats(actual.stats(), expected.stats());
}

void run_differential_history(const std::vector<lob::Event>& events) {
  lob::OrderBook actual;
  actual.reserve_orders(events.size());
  reference_lob::OrderBook expected;

  for (const lob::Event& event : events) {
    std::vector<lob::Trade> actual_trades;
    std::vector<lob::Trade> expected_trades;
    const lob::BookError actual_error = actual.process(event, actual_trades);
    const lob::BookError expected_error = expected.process(event, expected_trades);
    CHECK_EQ(static_cast<int>(actual_error), static_cast<int>(expected_error));
    compare_trades(actual_trades, expected_trades);
    compare_after_event(actual, expected);
  }
}

}  // namespace

TEST_CASE(differential_engine_matches_reference_across_fixed_histories) {
  for (std::uint64_t seed = 1; seed <= 256; ++seed) {
    run_differential_history(generate_history(seed, 750));
  }
}
