#pragma once

#include "types.hpp"

#include <map>
#include <vector>

namespace reference_lob {

class OrderBook {
 public:
  lob::BookError process(const lob::Event& event, std::vector<lob::Trade>& trades);

  [[nodiscard]] std::size_t live_order_count() const;
  [[nodiscard]] const lob::EngineStats& stats() const { return stats_; }
  [[nodiscard]] std::vector<lob::OrderSnapshot> orders(lob::Side side) const;

 private:
  struct Order {
    lob::OrderId order_id{};
    lob::Side side{};
    lob::Price price{};
    lob::Quantity qty{};
  };

  using BidLevels = std::map<lob::Price, std::vector<Order>, std::greater<lob::Price>>;
  using AskLevels = std::map<lob::Price, std::vector<Order>, std::less<lob::Price>>;

  BidLevels bids_;
  AskLevels asks_;
  lob::EngineStats stats_{};

  static bool valid_price(lob::Price price) { return price > 0; }
  static bool valid_qty(lob::Quantity qty) { return qty > 0; }
  static bool crosses(lob::Side incoming_side, lob::Price incoming_price,
                      lob::Price resting_price);

  lob::BookError add_order(lob::OrderId order_id, lob::Side side, lob::Price price,
                           lob::Quantity qty, std::vector<lob::Trade>& trades);
  lob::BookError cancel_order(lob::OrderId order_id);
  lob::BookError modify_order(lob::OrderId order_id, lob::Price new_price,
                              lob::Quantity new_qty, std::vector<lob::Trade>& trades);

  lob::Quantity match_incoming(lob::OrderId incoming_order_id, lob::Side incoming_side,
                               lob::Price incoming_price, lob::Quantity incoming_qty,
                               std::vector<lob::Trade>& trades);
  lob::Quantity max_crossing_qty(lob::Side incoming_side, lob::Price incoming_price,
                                 lob::Quantity incoming_qty) const;
  bool contains(lob::OrderId order_id) const;
  bool remove_order(lob::OrderId order_id);
  void add_resting_order(lob::OrderId order_id, lob::Side side, lob::Price price,
                         lob::Quantity qty);
};

}  // namespace reference_lob
