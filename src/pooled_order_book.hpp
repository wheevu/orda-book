#pragma once

#include "types.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lob {

class PooledOrderBook {
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
  using SlotIndex = std::uint32_t;
  static constexpr SlotIndex kInvalidSlot = 0;

  struct OrderSlot {
    OrderId order_id{};
    Side side{};
    Price price{};
    Quantity qty{};
    SlotIndex prev{kInvalidSlot};
    SlotIndex next{kInvalidSlot};
    SlotIndex free_next{kInvalidSlot};
    bool live{false};
  };

  struct PriceLevel {
    Price price{};
    Quantity total_qty{};
    SlotIndex head{kInvalidSlot};
    SlotIndex tail{kInvalidSlot};
    std::size_t order_count{};
  };

  using BidLevels = std::map<Price, PriceLevel, std::greater<Price>>;
  using AskLevels = std::map<Price, PriceLevel, std::less<Price>>;

  BidLevels bids_;
  AskLevels asks_;
  std::vector<OrderSlot> slots_;
  SlotIndex free_head_{kInvalidSlot};
  std::unordered_map<OrderId, SlotIndex> order_index_;
  EngineStats stats_{};

  static bool is_valid_price(Price price) { return price > 0; }
  static bool is_valid_qty(Quantity qty) { return qty > 0; }

  PriceLevel& ensure_level(Side side, Price price);
  SlotIndex allocate_slot();
  void release_slot(SlotIndex slot);
  void append_slot(PriceLevel& level, SlotIndex slot);
  void unlink_slot(PriceLevel& level, SlotIndex slot);
  void add_resting_order(OrderId order_id, Side side, Price price, Quantity qty);
  void remove_order(OrderId order_id, SlotIndex slot);
  Quantity match_incoming(OrderId incoming_order_id, Side incoming_side, Price incoming_price,
                          Quantity incoming_qty, std::vector<Trade>& trades);
  Quantity max_crossing_qty(Side incoming_side, Price incoming_price,
                            Quantity incoming_qty) const;
  bool crosses(Side incoming_side, Price incoming_price, Price resting_price) const;
};

}  // namespace lob
