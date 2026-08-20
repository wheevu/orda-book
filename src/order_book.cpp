#include "order_book.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>

namespace lob {

void OrderBook::reserve_orders(std::size_t capacity) {
  order_index_.reserve(capacity);
  order_index_.max_load_factor(0.7F);
}

OrderBook::PriceLevel& OrderBook::ensure_level(Side side, Price price) {
  if (side == Side::Bid) {
    auto [it, inserted] = bids_.try_emplace(price);
    if (inserted) {
      it->second.price = price;
    }
    return it->second;
  }

  auto [it, inserted] = asks_.try_emplace(price);
  if (inserted) {
    it->second.price = price;
  }
  return it->second;
}

void OrderBook::add_resting_order(OrderId order_id, Side side, Price price, Quantity qty) {
  PriceLevel& level = ensure_level(side, price);
  level.orders.push_back(Order{order_id, side, price, qty});
  auto order_it = std::prev(level.orders.end());
  level.total_qty += qty;
  order_index_.emplace(order_id, OrderLocation{side, &level, order_it});
}

void OrderBook::remove_order(OrderId order_id, const OrderLocation& location) {
  PriceLevel& level = *location.level;
  level.total_qty -= location.order_it->qty;
  level.orders.erase(location.order_it);
  if (level.orders.empty()) {
    assert(level.total_qty == 0);
    if (location.side == Side::Bid) {
      bids_.erase(level.price);
    } else {
      asks_.erase(level.price);
    }
  }
  order_index_.erase(order_id);
}

bool OrderBook::crosses(Side incoming_side, Price incoming_price, Price resting_price) const {
  if (incoming_side == Side::Bid) {
    return incoming_price >= resting_price;
  }
  return incoming_price <= resting_price;
}

Quantity OrderBook::match_incoming(OrderId incoming_order_id, Side incoming_side, Price incoming_price,
                                   Quantity incoming_qty, std::vector<Trade>& trades) {
  if (incoming_side == Side::Bid) {
    auto level_it = asks_.begin();
    while (incoming_qty > 0 && level_it != asks_.end() &&
           crosses(incoming_side, incoming_price, level_it->first)) {
      PriceLevel& level = level_it->second;
      while (incoming_qty > 0 && !level.orders.empty()) {
        Order& resting = level.orders.front();
        const Quantity fill_qty = std::min(incoming_qty, resting.qty);
        trades.push_back(Trade{resting.order_id, incoming_order_id, resting.price, fill_qty,
                               incoming_side});
        stats_.trades += 1;
        stats_.traded_qty += fill_qty;

        incoming_qty -= fill_qty;
        resting.qty -= fill_qty;
        level.total_qty -= fill_qty;

        if (resting.qty == 0) {
          order_index_.erase(resting.order_id);
          level.orders.pop_front();
        }
      }

      if (level.orders.empty()) {
        assert(level.total_qty == 0);
        level_it = asks_.erase(level_it);
      } else {
        ++level_it;
      }
    }
    return incoming_qty;
  }

  auto level_it = bids_.begin();
  while (incoming_qty > 0 && level_it != bids_.end() &&
         crosses(incoming_side, incoming_price, level_it->first)) {
    PriceLevel& level = level_it->second;
    while (incoming_qty > 0 && !level.orders.empty()) {
      Order& resting = level.orders.front();
      const Quantity fill_qty = std::min(incoming_qty, resting.qty);
      trades.push_back(Trade{resting.order_id, incoming_order_id, resting.price, fill_qty,
                             incoming_side});
      stats_.trades += 1;
      stats_.traded_qty += fill_qty;

      incoming_qty -= fill_qty;
      resting.qty -= fill_qty;
      level.total_qty -= fill_qty;

      if (resting.qty == 0) {
        order_index_.erase(resting.order_id);
        level.orders.pop_front();
      }
    }

    if (level.orders.empty()) {
      assert(level.total_qty == 0);
      level_it = bids_.erase(level_it);
    } else {
      ++level_it;
    }
  }
  return incoming_qty;
}

BookError OrderBook::add_order(OrderId order_id, Side side, Price price, Quantity qty,
                               std::vector<Trade>& trades) {
  stats_.add_requests += 1;

  if (!is_valid_price(price)) {
    stats_.rejected_requests += 1;
    return BookError::InvalidPrice;
  }
  if (!is_valid_qty(qty)) {
    stats_.rejected_requests += 1;
    return BookError::InvalidQuantity;
  }
  if (order_index_.find(order_id) != order_index_.end()) {
    stats_.rejected_requests += 1;
    return BookError::DuplicateOrderId;
  }

  const Quantity remaining = match_incoming(order_id, side, price, qty, trades);
  if (remaining > 0) {
    add_resting_order(order_id, side, price, remaining);
  }
  return BookError::None;
}

BookError OrderBook::cancel_order(OrderId order_id) {
  stats_.cancel_requests += 1;

  auto it = order_index_.find(order_id);
  if (it == order_index_.end()) {
    stats_.rejected_requests += 1;
    return BookError::UnknownOrderId;
  }

  remove_order(order_id, it->second);
  return BookError::None;
}

BookError OrderBook::modify_order(OrderId order_id, Price new_price, Quantity new_qty,
                                  std::vector<Trade>& trades) {
  stats_.modify_requests += 1;

  auto it = order_index_.find(order_id);
  if (it == order_index_.end()) {
    stats_.rejected_requests += 1;
    return BookError::UnknownOrderId;
  }

  if (!is_valid_price(new_price)) {
    stats_.rejected_requests += 1;
    return BookError::InvalidPrice;
  }
  if (!is_valid_qty(new_qty)) {
    stats_.rejected_requests += 1;
    return BookError::InvalidQuantity;
  }

  const Side side = it->second.order_it->side;
  remove_order(order_id, it->second);

  const Quantity remaining = match_incoming(order_id, side, new_price, new_qty, trades);
  if (remaining > 0) {
    add_resting_order(order_id, side, new_price, remaining);
  }
  return BookError::None;
}

BookError OrderBook::process(const Event& event, std::vector<Trade>& trades) {
  switch (event.type) {
    case EventType::Add:
      return add_order(event.order_id, event.side, event.price, event.qty, trades);
    case EventType::Cancel:
      return cancel_order(event.order_id);
    case EventType::Modify:
      return modify_order(event.order_id, event.new_price, event.new_qty, trades);
  }
  stats_.rejected_requests += 1;
  return BookError::UnknownOrderId;
}

std::optional<LevelSnapshot> OrderBook::top_of_book(Side side) const {
  if (side == Side::Bid) {
    if (bids_.empty()) {
      return std::nullopt;
    }
    const PriceLevel& level = bids_.begin()->second;
    return LevelSnapshot{level.price, level.total_qty, level.orders.size()};
  }

  if (asks_.empty()) {
    return std::nullopt;
  }
  const PriceLevel& level = asks_.begin()->second;
  return LevelSnapshot{level.price, level.total_qty, level.orders.size()};
}

std::vector<LevelSnapshot> OrderBook::levels(Side side) const {
  std::vector<LevelSnapshot> snapshots;
  if (side == Side::Bid) {
    snapshots.reserve(bids_.size());
    for (const auto& [price, level] : bids_) {
      snapshots.push_back(LevelSnapshot{price, level.total_qty, level.orders.size()});
    }
    return snapshots;
  }

  snapshots.reserve(asks_.size());
  for (const auto& [price, level] : asks_) {
    snapshots.push_back(LevelSnapshot{price, level.total_qty, level.orders.size()});
  }
  return snapshots;
}

std::string OrderBook::format_book(bool full_depth) const {
  std::ostringstream out;
  out << "ASKS\n";
  if (asks_.empty()) {
    out << "  <empty>\n";
  } else if (full_depth) {
    for (const auto& [price, level] : asks_) {
      out << "  " << price << ' ' << level.total_qty << ' ' << level.orders.size() << "\n";
    }
  } else {
    const auto& [price, level] = *asks_.begin();
    out << "  " << price << ' ' << level.total_qty << ' ' << level.orders.size() << "\n";
  }

  out << "BIDS\n";
  if (bids_.empty()) {
    out << "  <empty>\n";
  } else if (full_depth) {
    for (const auto& [price, level] : bids_) {
      out << "  " << price << ' ' << level.total_qty << ' ' << level.orders.size() << "\n";
    }
  } else {
    const auto& [price, level] = *bids_.begin();
    out << "  " << price << ' ' << level.total_qty << ' ' << level.orders.size() << "\n";
  }

  return out.str();
}

}  // namespace lob
