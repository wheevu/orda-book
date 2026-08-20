#include "pooled_order_book.hpp"
#include "quantity_arithmetic.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace lob {

void PooledOrderBook::reserve_orders(std::size_t capacity) {
  if (capacity > static_cast<std::size_t>(std::numeric_limits<SlotIndex>::max() - 1U)) {
    throw std::length_error("pooled order capacity exceeds slot index range");
  }

  slots_.clear();
  slots_.resize(capacity + 1U);
  free_head_ = capacity == 0 ? kInvalidSlot : 1U;
  for (std::size_t index = 1; index <= capacity; ++index) {
    slots_[index].free_next = index == capacity
                                  ? kInvalidSlot
                                  : static_cast<SlotIndex>(index + 1U);
  }
  order_index_.clear();
  order_index_.reserve(capacity);
  order_index_.max_load_factor(0.7F);
  bids_.clear();
  asks_.clear();
  stats_ = EngineStats{};
}

PooledOrderBook::PriceLevel& PooledOrderBook::ensure_level(Side side, Price price) {
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

PooledOrderBook::SlotIndex PooledOrderBook::allocate_slot() {
  if (free_head_ == kInvalidSlot) {
    return kInvalidSlot;
  }
  const SlotIndex slot = free_head_;
  free_head_ = slots_[slot].free_next;
  slots_[slot] = OrderSlot{};
  slots_[slot].live = true;
  return slot;
}

void PooledOrderBook::release_slot(SlotIndex slot) {
  slots_[slot] = OrderSlot{};
  slots_[slot].free_next = free_head_;
  free_head_ = slot;
}

void PooledOrderBook::append_slot(PriceLevel& level, SlotIndex slot) {
  OrderSlot& order = slots_[slot];
  order.prev = level.tail;
  order.next = kInvalidSlot;
  if (level.tail != kInvalidSlot) {
    slots_[level.tail].next = slot;
  } else {
    level.head = slot;
  }
  level.tail = slot;
  ++level.order_count;
  level.total_qty += order.qty;
}

void PooledOrderBook::unlink_slot(PriceLevel& level, SlotIndex slot) {
  const OrderSlot& order = slots_[slot];
  if (order.prev != kInvalidSlot) {
    slots_[order.prev].next = order.next;
  } else {
    level.head = order.next;
  }
  if (order.next != kInvalidSlot) {
    slots_[order.next].prev = order.prev;
  } else {
    level.tail = order.prev;
  }
  --level.order_count;
  level.total_qty -= order.qty;
}

void PooledOrderBook::add_resting_order(OrderId order_id, Side side, Price price, Quantity qty) {
  const SlotIndex slot = allocate_slot();
  if (slot == kInvalidSlot) {
    throw std::logic_error("pooled order capacity was not checked before insertion");
  }
  OrderSlot& order = slots_[slot];
  order.order_id = order_id;
  order.side = side;
  order.price = price;
  order.qty = qty;
  PriceLevel& level = ensure_level(side, price);
  append_slot(level, slot);
  order_index_.emplace(order_id, slot);
}

void PooledOrderBook::remove_order(OrderId order_id, SlotIndex slot) {
  OrderSlot& order = slots_[slot];
  PriceLevel* level = nullptr;
  if (order.side == Side::Bid) {
    level = &bids_.at(order.price);
  } else {
    level = &asks_.at(order.price);
  }
  unlink_slot(*level, slot);
  if (level->order_count == 0) {
    if (order.side == Side::Bid) {
      bids_.erase(order.price);
    } else {
      asks_.erase(order.price);
    }
  }
  order_index_.erase(order_id);
  release_slot(slot);
}

bool PooledOrderBook::crosses(Side incoming_side, Price incoming_price,
                              Price resting_price) const {
  return incoming_side == Side::Bid ? incoming_price >= resting_price
                                    : incoming_price <= resting_price;
}

Quantity PooledOrderBook::max_crossing_qty(Side incoming_side, Price incoming_price,
                                           Quantity incoming_qty) const {
  Quantity crossing_qty = 0;
  const auto add_level_qty = [&crossing_qty, incoming_qty](Quantity level_qty) {
    const Quantity remaining = incoming_qty - crossing_qty;
    crossing_qty += std::min(remaining, level_qty);
  };
  if (incoming_side == Side::Bid) {
    for (const auto& [price, level] : asks_) {
      if (!crosses(incoming_side, incoming_price, price)) break;
      add_level_qty(level.total_qty);
      if (crossing_qty == incoming_qty) break;
    }
  } else {
    for (const auto& [price, level] : bids_) {
      if (!crosses(incoming_side, incoming_price, price)) break;
      add_level_qty(level.total_qty);
      if (crossing_qty == incoming_qty) break;
    }
  }
  return crossing_qty;
}

Quantity PooledOrderBook::match_incoming(OrderId incoming_order_id, Side incoming_side,
                                         Price incoming_price, Quantity incoming_qty,
                                         std::vector<Trade>& trades) {
  if (incoming_side == Side::Bid) {
    auto level_it = asks_.begin();
    while (incoming_qty > 0 && level_it != asks_.end() &&
           crosses(incoming_side, incoming_price, level_it->first)) {
      PriceLevel& level = level_it->second;
      while (incoming_qty > 0 && level.head != kInvalidSlot) {
        const SlotIndex slot = level.head;
        OrderSlot& resting = slots_[slot];
        const Quantity fill_qty = std::min(incoming_qty, resting.qty);
        trades.push_back(Trade{resting.order_id, incoming_order_id, resting.price, fill_qty,
                               incoming_side});
        ++stats_.trades;
        stats_.traded_qty += fill_qty;
        incoming_qty -= fill_qty;
        resting.qty -= fill_qty;
        level.total_qty -= fill_qty;
        if (resting.qty == 0) {
          order_index_.erase(resting.order_id);
          const SlotIndex next = resting.next;
          if (next != kInvalidSlot) {
            slots_[next].prev = kInvalidSlot;
          }
          level.head = next;
          if (level.tail == slot) {
            level.tail = kInvalidSlot;
          }
          --level.order_count;
          release_slot(slot);
        }
      }
      if (level.order_count == 0) {
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
    while (incoming_qty > 0 && level.head != kInvalidSlot) {
      const SlotIndex slot = level.head;
      OrderSlot& resting = slots_[slot];
      const Quantity fill_qty = std::min(incoming_qty, resting.qty);
      trades.push_back(Trade{resting.order_id, incoming_order_id, resting.price, fill_qty,
                             incoming_side});
      ++stats_.trades;
      stats_.traded_qty += fill_qty;
      incoming_qty -= fill_qty;
      resting.qty -= fill_qty;
      level.total_qty -= fill_qty;
      if (resting.qty == 0) {
        order_index_.erase(resting.order_id);
        const SlotIndex next = resting.next;
        if (next != kInvalidSlot) {
          slots_[next].prev = kInvalidSlot;
        }
        level.head = next;
        if (level.tail == slot) {
          level.tail = kInvalidSlot;
        }
        --level.order_count;
        release_slot(slot);
      }
    }
    if (level.order_count == 0) {
      level_it = bids_.erase(level_it);
    } else {
      ++level_it;
    }
  }
  return incoming_qty;
}

BookError PooledOrderBook::add_order(OrderId order_id, Side side, Price price, Quantity qty,
                                     std::vector<Trade>& trades) {
  ++stats_.add_requests;
  if (!is_valid_price(price)) {
    ++stats_.rejected_requests;
    return BookError::InvalidPrice;
  }
  if (!is_valid_qty(qty)) {
    ++stats_.rejected_requests;
    return BookError::InvalidQuantity;
  }
  if (order_index_.find(order_id) != order_index_.end()) {
    ++stats_.rejected_requests;
    return BookError::DuplicateOrderId;
  }
  const Quantity crossing_qty = max_crossing_qty(side, price, qty);
  const Quantity resting_qty = qty - crossing_qty;
  bool level_overflow = false;
  if (side == Side::Bid) {
    const auto level_it = bids_.find(price);
    level_overflow = level_it != bids_.end() &&
                     quantity_add_would_overflow(level_it->second.total_qty, resting_qty);
  } else {
    const auto level_it = asks_.find(price);
    level_overflow = level_it != asks_.end() &&
                     quantity_add_would_overflow(level_it->second.total_qty, resting_qty);
  }
  if (level_overflow || quantity_add_would_overflow(stats_.traded_qty, crossing_qty)) {
    ++stats_.rejected_requests;
    return BookError::QuantityOverflow;
  }
  if (free_head_ == kInvalidSlot) {
    ++stats_.rejected_requests;
    return BookError::CapacityExceeded;
  }

  const Quantity remaining = match_incoming(order_id, side, price, qty, trades);
  if (remaining == 0) {
    return BookError::None;
  }
  add_resting_order(order_id, side, price, remaining);
  return BookError::None;
}

BookError PooledOrderBook::cancel_order(OrderId order_id) {
  ++stats_.cancel_requests;
  const auto it = order_index_.find(order_id);
  if (it == order_index_.end()) {
    ++stats_.rejected_requests;
    return BookError::UnknownOrderId;
  }
  remove_order(order_id, it->second);
  return BookError::None;
}

BookError PooledOrderBook::modify_order(OrderId order_id, Price new_price, Quantity new_qty,
                                        std::vector<Trade>& trades) {
  ++stats_.modify_requests;
  const auto it = order_index_.find(order_id);
  if (it == order_index_.end()) {
    ++stats_.rejected_requests;
    return BookError::UnknownOrderId;
  }
  if (!is_valid_price(new_price)) {
    ++stats_.rejected_requests;
    return BookError::InvalidPrice;
  }
  if (!is_valid_qty(new_qty)) {
    ++stats_.rejected_requests;
    return BookError::InvalidQuantity;
  }

  Quantity existing_level_qty = 0;
  const OrderSlot& current = slots_[it->second];
  if (current.side == Side::Bid) {
    const auto level_it = bids_.find(new_price);
    if (level_it != bids_.end()) {
      existing_level_qty = level_it->second.total_qty;
      if (current.price == new_price && &level_it->second == &bids_.at(current.price)) {
        existing_level_qty -= current.qty;
      }
    }
  } else {
    const auto level_it = asks_.find(new_price);
    if (level_it != asks_.end()) {
      existing_level_qty = level_it->second.total_qty;
      if (current.price == new_price && &level_it->second == &asks_.at(current.price)) {
        existing_level_qty -= current.qty;
      }
    }
  }
  const Quantity crossing_qty = max_crossing_qty(current.side, new_price, new_qty);
  const Quantity resting_qty = new_qty - crossing_qty;
  if (quantity_add_would_overflow(existing_level_qty, resting_qty) ||
      quantity_add_would_overflow(stats_.traded_qty, crossing_qty)) {
    ++stats_.rejected_requests;
    return BookError::QuantityOverflow;
  }

  const Side side = slots_[it->second].side;
  remove_order(order_id, it->second);
  const Quantity remaining = match_incoming(order_id, side, new_price, new_qty, trades);
  if (remaining == 0) {
    return BookError::None;
  }
  add_resting_order(order_id, side, new_price, remaining);
  return BookError::None;
}

BookError PooledOrderBook::process(const Event& event, std::vector<Trade>& trades) {
  switch (event.type) {
    case EventType::Add:
      return add_order(event.order_id, event.side, event.price, event.qty, trades);
    case EventType::Cancel:
      return cancel_order(event.order_id);
    case EventType::Modify:
      return modify_order(event.order_id, event.new_price, event.new_qty, trades);
  }
  ++stats_.rejected_requests;
  return BookError::UnknownOrderId;
}

std::optional<LevelSnapshot> PooledOrderBook::top_of_book(Side side) const {
  const auto top = [](const auto& levels) -> std::optional<LevelSnapshot> {
    if (levels.empty()) {
      return std::nullopt;
    }
    const PriceLevel& level = levels.begin()->second;
    return LevelSnapshot{level.price, level.total_qty, level.order_count};
  };
  return side == Side::Bid ? top(bids_) : top(asks_);
}

std::vector<LevelSnapshot> PooledOrderBook::levels(Side side) const {
  std::vector<LevelSnapshot> snapshots;
  const auto append = [&snapshots](const auto& levels) {
    snapshots.reserve(snapshots.size() + levels.size());
    for (const auto& [price, level] : levels) {
      snapshots.push_back(LevelSnapshot{price, level.total_qty, level.order_count});
    }
  };
  if (side == Side::Bid) {
    append(bids_);
  } else {
    append(asks_);
  }
  return snapshots;
}

std::vector<OrderSnapshot> PooledOrderBook::orders(Side side) const {
  std::vector<OrderSnapshot> snapshots;
  const auto append = [&snapshots, this](const auto& levels) {
    for (const auto& [price, level] : levels) {
      SlotIndex slot = level.head;
      while (slot != kInvalidSlot) {
        const OrderSlot& order = slots_[slot];
        snapshots.push_back(OrderSnapshot{order.order_id, order.side, price, order.qty});
        slot = order.next;
      }
    }
  };
  if (side == Side::Bid) {
    append(bids_);
  } else {
    append(asks_);
  }
  return snapshots;
}

std::string PooledOrderBook::format_book(bool full_depth) const {
  std::ostringstream out;
  const auto write = [&out, full_depth](const auto& levels, const char* name) {
    out << name << "\n";
    if (levels.empty()) {
      out << "  <empty>\n";
    } else if (full_depth) {
      for (const auto& [price, level] : levels) {
        out << "  " << price << ' ' << level.total_qty << ' ' << level.order_count << "\n";
      }
    } else {
      const auto& [price, level] = *levels.begin();
      out << "  " << price << ' ' << level.total_qty << ' ' << level.order_count << "\n";
    }
  };
  write(asks_, "ASKS");
  write(bids_, "BIDS");
  return out.str();
}

}  // namespace lob
