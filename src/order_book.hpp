#pragma once

#include "types.hpp"

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace lob {

class OrderBook {
 public:
  void reserve_orders(std::size_t capacity);

  BookError add_order(OrderId order_id, Side side, Price price, Quantity qty,
                      std::vector<Trade>& trades);
  BookError cancel_order(OrderId order_id);
  BookError modify_order(OrderId order_id, Price new_price, Quantity new_qty,
                         std::vector<Trade>& trades);
  BookError process(const Event& event, std::vector<Trade>& trades);

  [[nodiscard]] std::size_t live_order_count() const { return order_index_.size(); }
  [[nodiscard]] const EngineStats& stats() const { return stats_; }

  [[nodiscard]] std::optional<LevelSnapshot> top_of_book(Side side) const;
  [[nodiscard]] std::vector<LevelSnapshot> levels(Side side) const;
  [[nodiscard]] std::vector<OrderSnapshot> orders(Side side) const;
  [[nodiscard]] std::string format_book(bool full_depth = true) const;

 private:
  struct Order {
    OrderId order_id{};
    Side side{};
    Price price{};
    Quantity qty{};
  };

  struct PriceLevel {
    Price price{};
    Quantity total_qty{};
    std::list<Order> orders;
  };

  using BidLevels = std::map<Price, PriceLevel, std::greater<Price>>;
  using AskLevels = std::map<Price, PriceLevel, std::less<Price>>;
  using OrderIterator = std::list<Order>::iterator;

  struct OrderLocation {
    Side side{};
    PriceLevel* level{};
    OrderIterator order_it;
  };

  BidLevels bids_;
  AskLevels asks_;
  std::unordered_map<OrderId, OrderLocation> order_index_;
  EngineStats stats_{};

  static bool is_valid_price(Price price) { return price > 0; }
  static bool is_valid_qty(Quantity qty) { return qty > 0; }

  PriceLevel& ensure_level(Side side, Price price);
  void add_resting_order(OrderId order_id, Side side, Price price, Quantity qty);
  void remove_order(OrderId order_id, const OrderLocation& location);
  Quantity match_incoming(OrderId incoming_order_id, Side incoming_side, Price incoming_price,
                          Quantity incoming_qty, std::vector<Trade>& trades);
  Quantity max_crossing_qty(Side incoming_side, Price incoming_price,
                            Quantity incoming_qty) const;
  bool crosses(Side incoming_side, Price incoming_price, Price resting_price) const;
};

}  // namespace lob
