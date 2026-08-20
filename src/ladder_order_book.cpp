#include "ladder_order_book.hpp"
#include "quantity_arithmetic.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace lob {

LadderOrderBook::LadderOrderBook(Price min_price, Price max_price)
    : min_price_(min_price), max_price_(max_price) {
  if (min_price < 0 || max_price < min_price) {
    throw std::invalid_argument("invalid price ladder range");
  }
  const std::size_t count = static_cast<std::size_t>(max_price - min_price + 1);
  levels_.resize(count);
  bid_occupied_.resize((count + 63U) / 64U);
  ask_occupied_.resize((count + 63U) / 64U);
  bid_word_occupied_.resize((bid_occupied_.size() + 63U) / 64U);
  ask_word_occupied_.resize((ask_occupied_.size() + 63U) / 64U);
  for (std::size_t index = 0; index < count; ++index) {
    levels_[index].price = min_price + static_cast<Price>(index);
  }
}

void LadderOrderBook::reserve_orders(std::size_t capacity) {
  order_index_.reserve(capacity);
  order_index_.max_load_factor(0.7F);
}

std::size_t LadderOrderBook::index_for(Price price) const {
  return static_cast<std::size_t>(price - min_price_);
}

void LadderOrderBook::set_occupied(Side side, std::size_t index) {
  auto& occupied = side == Side::Bid ? bid_occupied_ : ask_occupied_;
  auto& word_occupied = side == Side::Bid ? bid_word_occupied_ : ask_word_occupied_;
  const std::size_t word = index / 64U;
  if ((occupied[word] & (std::uint64_t{1} << (index % 64U))) == 0) {
    word_occupied[word / 64U] |= std::uint64_t{1} << (word % 64U);
  }
  occupied[word] |= std::uint64_t{1} << (index % 64U);
}

void LadderOrderBook::clear_occupied(Side side, std::size_t index) {
  auto& occupied = side == Side::Bid ? bid_occupied_ : ask_occupied_;
  auto& word_occupied = side == Side::Bid ? bid_word_occupied_ : ask_word_occupied_;
  const std::size_t word = index / 64U;
  occupied[word] &= ~(std::uint64_t{1} << (index % 64U));
  if (occupied[word] == 0) {
    word_occupied[word / 64U] &= ~(std::uint64_t{1} << (word % 64U));
  }
}

std::optional<std::size_t> LadderOrderBook::best_index(Side side) const {
  const auto& occupied = side == Side::Bid ? bid_occupied_ : ask_occupied_;
  const auto& word_occupied = side == Side::Bid ? bid_word_occupied_ : ask_word_occupied_;
  if (side == Side::Ask) {
    for (std::size_t word = 0; word < word_occupied.size(); ++word) {
      const std::uint64_t word_bits = word_occupied[word];
      if (word_bits != 0) {
        const std::size_t occupied_word = word * 64U +
                                          static_cast<std::size_t>(__builtin_ctzll(word_bits));
        const std::uint64_t bits = occupied[occupied_word];
        return occupied_word * 64U + static_cast<std::size_t>(__builtin_ctzll(bits));
      }
    }
    return std::nullopt;
  }

  for (std::size_t word = word_occupied.size(); word > 0; --word) {
    const std::uint64_t word_bits = word_occupied[word - 1U];
    if (word_bits != 0) {
      const std::size_t occupied_word = (word - 1U) * 64U +
                                        (63U - static_cast<std::size_t>(__builtin_clzll(word_bits)));
      const std::uint64_t bits = occupied[occupied_word];
      return occupied_word * 64U +
             (63U - static_cast<std::size_t>(__builtin_clzll(bits)));
    }
  }
  return std::nullopt;
}

void LadderOrderBook::add_resting_order(OrderId order_id, Side side, Price price, Quantity qty) {
  PriceLevel& level = levels_[index_for(price)];
  level.orders.push_back(OrderSnapshot{order_id, side, price, qty});
  const auto order_it = std::prev(level.orders.end());
  level.total_qty += qty;
  order_index_.emplace(order_id, OrderLocation{&level, order_it});
  set_occupied(side, index_for(price));
}

void LadderOrderBook::remove_order(OrderId order_id, const OrderLocation& location) {
  PriceLevel& level = *location.level;
  const Side side = location.order_it->side;
  level.total_qty -= location.order_it->qty;
  level.orders.erase(location.order_it);
  if (level.orders.empty()) {
    clear_occupied(side, index_for(level.price));
  }
  order_index_.erase(order_id);
}

bool LadderOrderBook::crosses(Side incoming_side, Price incoming_price,
                              Price resting_price) const {
  return incoming_side == Side::Bid ? incoming_price >= resting_price
                                    : incoming_price <= resting_price;
}

Quantity LadderOrderBook::max_crossing_qty(Side incoming_side, Price incoming_price,
                                           Quantity incoming_qty) const {
  Quantity crossing_qty = 0;
  const auto add_level_qty = [&crossing_qty, incoming_qty](Quantity level_qty) {
    const Quantity remaining = incoming_qty - crossing_qty;
    crossing_qty += std::min(remaining, level_qty);
  };
  if (incoming_side == Side::Bid) {
    for (std::size_t word = 0; word < ask_word_occupied_.size(); ++word) {
      std::uint64_t word_bits = ask_word_occupied_[word];
      while (word_bits != 0) {
        const std::size_t occupied_word =
            word * 64U + static_cast<std::size_t>(__builtin_ctzll(word_bits));
        std::uint64_t level_bits = ask_occupied_[occupied_word];
        while (level_bits != 0) {
          const std::size_t index =
              occupied_word * 64U + static_cast<std::size_t>(__builtin_ctzll(level_bits));
          if (!crosses(incoming_side, incoming_price, levels_[index].price)) return crossing_qty;
          add_level_qty(levels_[index].total_qty);
          if (crossing_qty == incoming_qty) return crossing_qty;
          level_bits &= level_bits - 1U;
        }
        word_bits &= word_bits - 1U;
      }
    }
  } else {
    for (std::size_t word = bid_word_occupied_.size(); word > 0; --word) {
      std::uint64_t word_bits = bid_word_occupied_[word - 1U];
      while (word_bits != 0) {
        const std::size_t occupied_word = (word - 1U) * 64U +
                                          (63U - static_cast<std::size_t>(__builtin_clzll(word_bits)));
        std::uint64_t level_bits = bid_occupied_[occupied_word];
        while (level_bits != 0) {
          const std::size_t index = occupied_word * 64U +
                                    (63U - static_cast<std::size_t>(__builtin_clzll(level_bits)));
          if (!crosses(incoming_side, incoming_price, levels_[index].price)) return crossing_qty;
          add_level_qty(levels_[index].total_qty);
          if (crossing_qty == incoming_qty) return crossing_qty;
          level_bits &= level_bits - 1U;
        }
        word_bits &= word_bits - 1U;
      }
    }
  }
  return crossing_qty;
}

Quantity LadderOrderBook::match_incoming(OrderId incoming_order_id, Side incoming_side,
                                         Price incoming_price, Quantity incoming_qty,
                                         std::vector<Trade>& trades) {
  const Side resting_side = incoming_side == Side::Bid ? Side::Ask : Side::Bid;
  while (incoming_qty > 0) {
    const std::optional<std::size_t> best = best_index(resting_side);
    if (!best.has_value()) {
      break;
    }
    PriceLevel& level = levels_[*best];
    if (!crosses(incoming_side, incoming_price, level.price)) {
      break;
    }
    while (incoming_qty > 0 && !level.orders.empty()) {
      OrderSnapshot& resting = level.orders.front();
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
        level.orders.pop_front();
      }
    }
    if (level.orders.empty()) {
      clear_occupied(resting_side, *best);
    }
  }
  return incoming_qty;
}

BookError LadderOrderBook::add_order(OrderId order_id, Side side, Price price, Quantity qty,
                                     std::vector<Trade>& trades) {
  ++stats_.add_requests;
  if (!valid_price(price)) {
    ++stats_.rejected_requests;
    return price > 0 ? BookError::PriceOutOfRange : BookError::InvalidPrice;
  }
  if (!valid_qty(qty)) {
    ++stats_.rejected_requests;
    return BookError::InvalidQuantity;
  }
  if (order_index_.find(order_id) != order_index_.end()) {
    ++stats_.rejected_requests;
    return BookError::DuplicateOrderId;
  }
  const PriceLevel& level = levels_[index_for(price)];
  const Quantity crossing_qty = max_crossing_qty(side, price, qty);
  const Quantity resting_qty = qty - crossing_qty;
  if (quantity_add_would_overflow(level.total_qty, resting_qty) ||
      quantity_add_would_overflow(stats_.traded_qty, crossing_qty)) {
    ++stats_.rejected_requests;
    return BookError::QuantityOverflow;
  }
  const Quantity remaining = match_incoming(order_id, side, price, qty, trades);
  if (remaining > 0) {
    add_resting_order(order_id, side, price, remaining);
  }
  return BookError::None;
}

BookError LadderOrderBook::cancel_order(OrderId order_id) {
  ++stats_.cancel_requests;
  const auto it = order_index_.find(order_id);
  if (it == order_index_.end()) {
    ++stats_.rejected_requests;
    return BookError::UnknownOrderId;
  }
  remove_order(order_id, it->second);
  return BookError::None;
}

BookError LadderOrderBook::modify_order(OrderId order_id, Price new_price, Quantity new_qty,
                                        std::vector<Trade>& trades) {
  ++stats_.modify_requests;
  const auto it = order_index_.find(order_id);
  if (it == order_index_.end()) {
    ++stats_.rejected_requests;
    return BookError::UnknownOrderId;
  }
  if (!valid_price(new_price)) {
    ++stats_.rejected_requests;
    return new_price > 0 ? BookError::PriceOutOfRange : BookError::InvalidPrice;
  }
  if (!valid_qty(new_qty)) {
    ++stats_.rejected_requests;
    return BookError::InvalidQuantity;
  }
  Quantity existing_level_qty = levels_[index_for(new_price)].total_qty;
  const OrderSnapshot& current = *it->second.order_it;
  if (current.price == new_price) {
    existing_level_qty -= current.qty;
  }
  const Quantity crossing_qty = max_crossing_qty(current.side, new_price, new_qty);
  const Quantity resting_qty = new_qty - crossing_qty;
  if (quantity_add_would_overflow(existing_level_qty, resting_qty) ||
      quantity_add_would_overflow(stats_.traded_qty, crossing_qty)) {
    ++stats_.rejected_requests;
    return BookError::QuantityOverflow;
  }
  const Side side = it->second.order_it->side;
  remove_order(order_id, it->second);
  const Quantity remaining = match_incoming(order_id, side, new_price, new_qty, trades);
  if (remaining > 0) {
    add_resting_order(order_id, side, new_price, remaining);
  }
  return BookError::None;
}

BookError LadderOrderBook::process(const Event& event, std::vector<Trade>& trades) {
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

std::optional<LevelSnapshot> LadderOrderBook::top_of_book(Side side) const {
  const auto best = best_index(side);
  if (!best.has_value()) {
    return std::nullopt;
  }
  const PriceLevel& level = levels_[*best];
  return LevelSnapshot{level.price, level.total_qty, level.orders.size()};
}

std::vector<LevelSnapshot> LadderOrderBook::levels(Side side) const {
  std::vector<LevelSnapshot> snapshots;
  const auto& occupied = side == Side::Bid ? bid_occupied_ : ask_occupied_;
  const auto append = [&snapshots, this](std::size_t index) {
    const PriceLevel& level = levels_[index];
    snapshots.push_back(LevelSnapshot{level.price, level.total_qty, level.orders.size()});
  };
  if (side == Side::Ask) {
    for (std::size_t word = 0; word < occupied.size(); ++word) {
      std::uint64_t bits = occupied[word];
      while (bits != 0) {
        const std::size_t bit = static_cast<std::size_t>(__builtin_ctzll(bits));
        append(word * 64U + bit);
        bits &= bits - 1U;
      }
    }
  } else {
    for (std::size_t word = occupied.size(); word > 0; --word) {
      std::uint64_t bits = occupied[word - 1U];
      while (bits != 0) {
        const std::size_t bit = 63U - static_cast<std::size_t>(__builtin_clzll(bits));
        append((word - 1U) * 64U + bit);
        bits &= ~(std::uint64_t{1} << bit);
      }
    }
  }
  return snapshots;
}

std::vector<OrderSnapshot> LadderOrderBook::orders(Side side) const {
  std::vector<OrderSnapshot> snapshots;
  const auto level_orders = [&snapshots, this](std::size_t index) {
    const PriceLevel& level = levels_[index];
    snapshots.insert(snapshots.end(), level.orders.begin(), level.orders.end());
  };
  const auto& occupied = side == Side::Bid ? bid_occupied_ : ask_occupied_;
  if (side == Side::Ask) {
    for (std::size_t word = 0; word < occupied.size(); ++word) {
      std::uint64_t bits = occupied[word];
      while (bits != 0) {
        const std::size_t bit = static_cast<std::size_t>(__builtin_ctzll(bits));
        level_orders(word * 64U + bit);
        bits &= bits - 1U;
      }
    }
  } else {
    for (std::size_t word = occupied.size(); word > 0; --word) {
      std::uint64_t bits = occupied[word - 1U];
      while (bits != 0) {
        const std::size_t bit = 63U - static_cast<std::size_t>(__builtin_clzll(bits));
        level_orders((word - 1U) * 64U + bit);
        bits &= ~(std::uint64_t{1} << bit);
      }
    }
  }
  return snapshots;
}

std::string LadderOrderBook::format_book(bool full_depth) const {
  std::ostringstream out;
  const auto write = [&out, full_depth](const std::vector<LevelSnapshot>& levels,
                                        const char* name) {
    out << name << "\n";
    if (levels.empty()) {
      out << "  <empty>\n";
    } else if (full_depth) {
      for (const LevelSnapshot& level : levels) {
        out << "  " << level.price << ' ' << level.qty << ' ' << level.order_count << "\n";
      }
    } else {
      out << "  " << levels.front().price << ' ' << levels.front().qty << ' '
          << levels.front().order_count << "\n";
    }
  };
  write(levels(Side::Ask), "ASKS");
  write(levels(Side::Bid), "BIDS");
  return out.str();
}

}  // namespace lob
