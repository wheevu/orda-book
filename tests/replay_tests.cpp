#include "event_parser.hpp"
#include "order_book.hpp"
#include "test_framework.hpp"

#include <sstream>
#include <string>
#include <vector>

TEST_CASE(parser_accepts_plain_text_and_csv_lines) {
  std::istringstream input(
      "# comment\n"
      "ADD 1 BUY 100 10\n"
      "ADD,2,SELL,101,5\n"
      "MODIFY 1 102 7\n"
      "CANCEL 2\n");

  const lob::ParseResult parsed = lob::parse_event_stream(input);
  CHECK_TRUE(parsed.ok);
  CHECK_EQ(parsed.events.size(), static_cast<std::size_t>(4));
  CHECK_EQ(static_cast<int>(parsed.events[0].type), static_cast<int>(lob::EventType::Add));
  CHECK_EQ(static_cast<int>(parsed.events[1].type), static_cast<int>(lob::EventType::Add));
  CHECK_EQ(static_cast<int>(parsed.events[2].type), static_cast<int>(lob::EventType::Modify));
  CHECK_EQ(static_cast<int>(parsed.events[3].type), static_cast<int>(lob::EventType::Cancel));
}

TEST_CASE(replaying_event_stream_produces_expected_trades_and_book) {
  std::istringstream input(
      "ADD 1 BUY 100 10\n"
      "ADD 2 SELL 103 4\n"
      "ADD 3 SELL 100 6\n"
      "MODIFY 2 100 3\n"
      "CANCEL 1\n");

  const lob::ParseResult parsed = lob::parse_event_stream(input);
  CHECK_TRUE(parsed.ok);

  lob::OrderBook book;
  std::vector<lob::Trade> trades;
  for (const lob::Event& event : parsed.events) {
    CHECK_EQ(static_cast<int>(book.process(event, trades)), static_cast<int>(lob::BookError::None));
  }

  CHECK_EQ(trades.size(), static_cast<std::size_t>(2));
  CHECK_EQ(trades[0].price, 100);
  CHECK_EQ(trades[0].qty, 6);
  CHECK_EQ(trades[1].price, 100);
  CHECK_EQ(trades[1].qty, 3);
  CHECK_EQ(book.live_order_count(), static_cast<std::size_t>(0));
  CHECK_TRUE(!book.top_of_book(lob::Side::Bid).has_value());
  CHECK_TRUE(!book.top_of_book(lob::Side::Ask).has_value());
}

TEST_CASE(parser_rejects_unknown_commands) {
  std::istringstream input("NOPE 1 2 3\n");
  const lob::ParseResult parsed = lob::parse_event_stream(input);
  CHECK_TRUE(!parsed.ok);
  CHECK_EQ(parsed.error_line, static_cast<std::size_t>(1));
}
