#pragma once

#include "types.hpp"

#include <functional>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lob {

class LadderOrderBook {
 public:
  LadderOrderBook(Price min_price = 1, Price max_price = 200000);

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
  struct PriceLevel {
    Price price{};
    Quantity total_qty{};
    std::list<OrderSnapshot> orders;
  };

  struct OrderLocation {
    PriceLevel* level{};
    std::list<OrderSnapshot>::iterator order_it;
  };

  Price min_price_;
  Price max_price_;
  std::vector<PriceLevel> levels_;
  std::vector<std::uint64_t> bid_occupied_;
  std::vector<std::uint64_t> ask_occupied_;
  std::vector<std::uint64_t> bid_word_occupied_;
  std::vector<std::uint64_t> ask_word_occupied_;
  std::unordered_map<OrderId, OrderLocation> order_index_;
  EngineStats stats_{};

  static bool valid_qty(Quantity qty) { return qty > 0; }
  bool valid_price(Price price) const { return price >= min_price_ && price <= max_price_; }
  std::size_t index_for(Price price) const;
  void set_occupied(Side side, std::size_t index);
  void clear_occupied(Side side, std::size_t index);
  std::optional<std::size_t> best_index(Side side) const;
  void add_resting_order(OrderId order_id, Side side, Price price, Quantity qty);
  void remove_order(OrderId order_id, const OrderLocation& location);
  Quantity match_incoming(OrderId incoming_order_id, Side incoming_side, Price incoming_price,
                          Quantity incoming_qty, std::vector<Trade>& trades);
  Quantity max_crossing_qty(Side incoming_side, Price incoming_price,
                            Quantity incoming_qty) const;
  bool crosses(Side incoming_side, Price incoming_price, Price resting_price) const;
};

}  // namespace lob
