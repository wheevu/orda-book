#include "event_parser.hpp"
#include "order_book.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

void print_usage() {
  std::cerr << "usage: orda_replay <event_file> [--top]\n";
}

void print_trade(const lob::Trade& trade) {
  std::cout << "TRADE resting=" << trade.resting_order_id << " incoming=" << trade.incoming_order_id
            << " price=" << trade.price << " qty=" << trade.qty
            << " aggressor=" << lob::to_string(trade.aggressor_side) << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    print_usage();
    return 1;
  }

  const std::string path = argv[1];
  const bool top_only = argc == 3 && std::string(argv[2]) == "--top";

  const lob::ParseResult parsed = lob::parse_event_file(path);
  if (!parsed.ok) {
    std::cerr << "parse error";
    if (parsed.error_line != 0) {
      std::cerr << " on line " << parsed.error_line;
    }
    std::cerr << ": " << parsed.error_message << '\n';
    return 1;
  }

  lob::OrderBook book;
  std::vector<lob::Trade> trades;
  trades.reserve(parsed.events.size());

  for (const lob::Event& event : parsed.events) {
    const lob::BookError error = book.process(event, trades);
    if (error != lob::BookError::None) {
      std::cerr << "engine error on line " << event.line_number << ": " << lob::to_string(error)
                << '\n';
      return 1;
    }
  }

  for (const lob::Trade& trade : trades) {
    print_trade(trade);
  }

  std::cout << "EVENTS " << parsed.events.size() << '\n';
  std::cout << "TRADES " << trades.size() << '\n';
  std::cout << "LIVE_ORDERS " << book.live_order_count() << '\n';
  std::cout << book.format_book(!top_only);

  return 0;
}
