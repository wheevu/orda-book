#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::int64_t;

constexpr Price kInvalidPrice = 0;
constexpr Quantity kInvalidQuantity = 0;

enum class Side {
  Bid,
  Ask,
};

inline std::string_view to_string(Side side) {
  return side == Side::Bid ? "BUY" : "SELL";
}

inline std::optional<Side> parse_side(std::string_view token) {
  if (token == "B" || token == "BUY" || token == "BID") {
    return Side::Bid;
  }
  if (token == "S" || token == "SELL" || token == "ASK") {
    return Side::Ask;
  }
  return std::nullopt;
}

enum class EventType {
  Add,
  Cancel,
  Modify,
};

struct Event {
  EventType type{};
  OrderId order_id{};
  Side side{};
  Price price{};
  Quantity qty{};
  Price new_price{};
  Quantity new_qty{};
  std::size_t line_number{};
};

struct Trade {
  OrderId resting_order_id{};
  OrderId incoming_order_id{};
  Price price{};
  Quantity qty{};
  Side aggressor_side{};
};

struct LevelSnapshot {
  Price price{};
  Quantity qty{};
  std::size_t order_count{};
};

struct OrderSnapshot {
  OrderId order_id{};
  Side side{};
  Price price{};
  Quantity qty{};
};

enum class BookError {
  None,
  DuplicateOrderId,
  UnknownOrderId,
  InvalidPrice,
  InvalidQuantity,
  QuantityOverflow,
  CapacityExceeded,
  PriceOutOfRange,
};

inline std::string_view to_string(BookError error) {
  switch (error) {
    case BookError::None:
      return "none";
    case BookError::DuplicateOrderId:
      return "duplicate_order_id";
    case BookError::UnknownOrderId:
      return "unknown_order_id";
    case BookError::InvalidPrice:
      return "invalid_price";
    case BookError::InvalidQuantity:
      return "invalid_quantity";
    case BookError::QuantityOverflow:
      return "quantity_overflow";
    case BookError::CapacityExceeded:
      return "capacity_exceeded";
    case BookError::PriceOutOfRange:
      return "price_out_of_range";
  }
  return "unknown";
}

struct EngineStats {
  std::size_t add_requests{};
  std::size_t cancel_requests{};
  std::size_t modify_requests{};
  std::size_t rejected_requests{};
  std::size_t trades{};
  Quantity traded_qty{};
};

struct ParseResult {
  std::vector<Event> events;
  bool ok{true};
  std::size_t error_line{};
  std::string error_message;
};

}  // namespace lob
