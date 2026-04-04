#include "event_parser.hpp"

#include <charconv>
#include <fstream>
#include <string>
#include <string_view>

namespace lob {
namespace {

constexpr bool is_delimiter(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == ',';
}

void skip_delimiters(std::string_view& input) {
  while (!input.empty() && is_delimiter(input.front())) {
    input.remove_prefix(1);
  }
}

bool is_blank_or_comment(std::string_view line) {
  skip_delimiters(line);
  return line.empty() || line.front() == '#';
}

std::string_view next_token(std::string_view& input) {
  skip_delimiters(input);
  std::size_t length = 0;
  while (length < input.size() && !is_delimiter(input[length])) {
    ++length;
  }
  const std::string_view token = input.substr(0, length);
  input.remove_prefix(length);
  return token;
}

template <typename Integer>
bool parse_integer(std::string_view token, Integer& value) {
  if (token.empty()) {
    return false;
  }
  const char* begin = token.data();
  const char* end = token.data() + token.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  return ec == std::errc{} && ptr == end;
}

ParseResult fail(std::size_t line_number, std::string message) {
  ParseResult result;
  result.ok = false;
  result.error_line = line_number;
  result.error_message = std::move(message);
  return result;
}

ParseResult parse_event_stream_impl(std::istream& input, std::size_t reserve_hint) {
  ParseResult result;
  if (reserve_hint != 0) {
    result.events.reserve(reserve_hint);
  }

  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    std::string_view view(line);
    if (is_blank_or_comment(view)) {
      continue;
    }

    Event event;
    event.line_number = line_number;

    const std::string_view command = next_token(view);
    if (command == "ADD") {
      event.type = EventType::Add;
      const std::string_view order_id = next_token(view);
      const std::string_view side = next_token(view);
      const std::string_view price = next_token(view);
      const std::string_view qty = next_token(view);
      if (!parse_integer(order_id, event.order_id) || !parse_integer(price, event.price) ||
          !parse_integer(qty, event.qty)) {
        return fail(line_number, "invalid ADD event");
      }
      const auto parsed_side = parse_side(side);
      if (!parsed_side.has_value()) {
        return fail(line_number, "invalid side");
      }
      event.side = *parsed_side;
      if (!next_token(view).empty()) {
        return fail(line_number, "extra tokens in ADD event");
      }
      result.events.push_back(event);
      continue;
    }

    if (command == "CANCEL") {
      event.type = EventType::Cancel;
      const std::string_view order_id = next_token(view);
      if (!parse_integer(order_id, event.order_id)) {
        return fail(line_number, "invalid CANCEL event");
      }
      if (!next_token(view).empty()) {
        return fail(line_number, "extra tokens in CANCEL event");
      }
      result.events.push_back(event);
      continue;
    }

    if (command == "MODIFY") {
      event.type = EventType::Modify;
      const std::string_view order_id = next_token(view);
      const std::string_view new_price = next_token(view);
      const std::string_view new_qty = next_token(view);
      if (!parse_integer(order_id, event.order_id) || !parse_integer(new_price, event.new_price) ||
          !parse_integer(new_qty, event.new_qty)) {
        return fail(line_number, "invalid MODIFY event");
      }
      if (!next_token(view).empty()) {
        return fail(line_number, "extra tokens in MODIFY event");
      }
      result.events.push_back(event);
      continue;
    }

    return fail(line_number, "unknown command");
  }

  return result;
}

}  // namespace

ParseResult parse_event_stream(std::istream& input) { return parse_event_stream_impl(input, 0); }

ParseResult parse_event_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return fail(0, "failed to open file");
  }

  const auto end = input.tellg();
  const std::size_t reserve_hint = end > 0 ? static_cast<std::size_t>(end) / 18U : 0U;
  input.seekg(0, std::ios::beg);
  return parse_event_stream_impl(input, reserve_hint);
}

}  // namespace lob
