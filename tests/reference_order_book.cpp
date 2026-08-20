#include "reference_order_book.hpp"

#include <algorithm>

namespace reference_lob {

bool OrderBook::crosses(lob::Side incoming_side, lob::Price incoming_price,
                        lob::Price resting_price) {
  return incoming_side == lob::Side::Bid ? incoming_price >= resting_price
                                         : incoming_price <= resting_price;
}

bool OrderBook::contains(lob::OrderId order_id) const {
  const auto contains_in = [order_id](const auto& levels) {
    for (const auto& [price, orders] : levels) {
      static_cast<void>(price);
      for (const Order& order : orders) {
        if (order.order_id == order_id) {
          return true;
        }
      }
    }
    return false;
  };
  return contains_in(bids_) || contains_in(asks_);
}

void OrderBook::add_resting_order(lob::OrderId order_id, lob::Side side, lob::Price price,
                                  lob::Quantity qty) {
  Order order{order_id, side, price, qty};
  if (side == lob::Side::Bid) {
    bids_[price].push_back(order);
  } else {
    asks_[price].push_back(order);
  }
}

bool OrderBook::remove_order(lob::OrderId order_id) {
  const auto remove_from = [order_id](auto& levels) {
    for (auto level_it = levels.begin(); level_it != levels.end(); ++level_it) {
      auto& orders = level_it->second;
      const auto order_it = std::find_if(orders.begin(), orders.end(),
                                         [order_id](const Order& order) {
                                           return order.order_id == order_id;
                                         });
      if (order_it == orders.end()) {
        continue;
      }
      orders.erase(order_it);
      if (orders.empty()) {
        levels.erase(level_it);
      }
      return true;
    }
    return false;
  };
  return remove_from(bids_) || remove_from(asks_);
}

lob::Quantity OrderBook::match_incoming(lob::OrderId incoming_order_id,
                                        lob::Side incoming_side, lob::Price incoming_price,
                                        lob::Quantity incoming_qty,
                                        std::vector<lob::Trade>& trades) {
  if (incoming_side == lob::Side::Bid) {
    auto level_it = asks_.begin();
    while (incoming_qty > 0 && level_it != asks_.end() &&
           crosses(incoming_side, incoming_price, level_it->first)) {
      auto& orders = level_it->second;
      while (incoming_qty > 0 && !orders.empty()) {
        Order& resting = orders.front();
        const lob::Quantity fill_qty = std::min(incoming_qty, resting.qty);
        trades.push_back(lob::Trade{resting.order_id, incoming_order_id, resting.price, fill_qty,
                                    incoming_side});
        ++stats_.trades;
        stats_.traded_qty += fill_qty;
        incoming_qty -= fill_qty;
        resting.qty -= fill_qty;
        if (resting.qty == 0) {
          orders.erase(orders.begin());
        }
      }
      if (orders.empty()) {
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
    auto& orders = level_it->second;
    while (incoming_qty > 0 && !orders.empty()) {
      Order& resting = orders.front();
      const lob::Quantity fill_qty = std::min(incoming_qty, resting.qty);
      trades.push_back(lob::Trade{resting.order_id, incoming_order_id, resting.price, fill_qty,
                                  incoming_side});
      ++stats_.trades;
      stats_.traded_qty += fill_qty;
      incoming_qty -= fill_qty;
      resting.qty -= fill_qty;
      if (resting.qty == 0) {
        orders.erase(orders.begin());
      }
    }
    if (orders.empty()) {
      level_it = bids_.erase(level_it);
    } else {
      ++level_it;
    }
  }
  return incoming_qty;
}

lob::BookError OrderBook::add_order(lob::OrderId order_id, lob::Side side, lob::Price price,
                                    lob::Quantity qty, std::vector<lob::Trade>& trades) {
  ++stats_.add_requests;
  if (!valid_price(price)) {
    ++stats_.rejected_requests;
    return lob::BookError::InvalidPrice;
  }
  if (!valid_qty(qty)) {
    ++stats_.rejected_requests;
    return lob::BookError::InvalidQuantity;
  }
  if (contains(order_id)) {
    ++stats_.rejected_requests;
    return lob::BookError::DuplicateOrderId;
  }

  const lob::Quantity remaining = match_incoming(order_id, side, price, qty, trades);
  if (remaining > 0) {
    add_resting_order(order_id, side, price, remaining);
  }
  return lob::BookError::None;
}

lob::BookError OrderBook::cancel_order(lob::OrderId order_id) {
  ++stats_.cancel_requests;
  if (!remove_order(order_id)) {
    ++stats_.rejected_requests;
    return lob::BookError::UnknownOrderId;
  }
  return lob::BookError::None;
}

lob::BookError OrderBook::modify_order(lob::OrderId order_id, lob::Price new_price,
                                       lob::Quantity new_qty,
                                       std::vector<lob::Trade>& trades) {
  ++stats_.modify_requests;
  if (!contains(order_id)) {
    ++stats_.rejected_requests;
    return lob::BookError::UnknownOrderId;
  }
  if (!valid_price(new_price)) {
    ++stats_.rejected_requests;
    return lob::BookError::InvalidPrice;
  }
  if (!valid_qty(new_qty)) {
    ++stats_.rejected_requests;
    return lob::BookError::InvalidQuantity;
  }

  lob::Side side{};
  const auto find_side = [order_id, &side](const auto& levels) {
    for (const auto& [price, orders] : levels) {
      static_cast<void>(price);
      for (const Order& order : orders) {
        if (order.order_id == order_id) {
          side = order.side;
          return true;
        }
      }
    }
    return false;
  };
  if (!find_side(bids_) && !find_side(asks_)) {
    ++stats_.rejected_requests;
    return lob::BookError::UnknownOrderId;
  }

  remove_order(order_id);
  const lob::Quantity remaining = match_incoming(order_id, side, new_price, new_qty, trades);
  if (remaining > 0) {
    add_resting_order(order_id, side, new_price, remaining);
  }
  return lob::BookError::None;
}

lob::BookError OrderBook::process(const lob::Event& event, std::vector<lob::Trade>& trades) {
  switch (event.type) {
    case lob::EventType::Add:
      return add_order(event.order_id, event.side, event.price, event.qty, trades);
    case lob::EventType::Cancel:
      return cancel_order(event.order_id);
    case lob::EventType::Modify:
      return modify_order(event.order_id, event.new_price, event.new_qty, trades);
  }
  ++stats_.rejected_requests;
  return lob::BookError::UnknownOrderId;
}

std::size_t OrderBook::live_order_count() const {
  const auto count_in = [](const auto& levels) {
    std::size_t count = 0;
    for (const auto& [price, orders] : levels) {
      static_cast<void>(price);
      count += orders.size();
    }
    return count;
  };
  return count_in(bids_) + count_in(asks_);
}

std::vector<lob::OrderSnapshot> OrderBook::orders(lob::Side side) const {
  std::vector<lob::OrderSnapshot> snapshots;
  const auto append = [&snapshots](const auto& levels) {
    for (const auto& [price, orders] : levels) {
      for (const Order& order : orders) {
        snapshots.push_back(lob::OrderSnapshot{order.order_id, order.side, price, order.qty});
      }
    }
  };
  if (side == lob::Side::Bid) {
    append(bids_);
  } else {
    append(asks_);
  }
  return snapshots;
}

}  // namespace reference_lob
