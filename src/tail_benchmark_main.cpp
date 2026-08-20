#include "ladder_order_book.hpp"
#include "order_book.hpp"
#include "pooled_order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

enum class Backend { Baseline, Pooled, Ladder };
enum class Workload { Sweep, Cancel, Modify };

struct Config {
  Backend backend = Backend::Baseline;
  Workload workload = Workload::Sweep;
  std::size_t depth = 1024;
  std::size_t rounds = 3;
  std::size_t cycles = 32;
};

Backend parse_backend(const std::string& value) {
  if (value == "baseline") return Backend::Baseline;
  if (value == "pooled") return Backend::Pooled;
  if (value == "ladder") return Backend::Ladder;
  throw std::runtime_error("unknown backend");
}

Workload parse_workload(const std::string& value) {
  if (value == "sweep") return Workload::Sweep;
  if (value == "cancel") return Workload::Cancel;
  if (value == "modify") return Workload::Modify;
  throw std::runtime_error("unknown tail workload");
}

Config parse_args(int argc, char** argv) {
  Config config;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--backend" && index + 1 < argc) {
      config.backend = parse_backend(argv[++index]);
    } else if (arg == "--workload" && index + 1 < argc) {
      config.workload = parse_workload(argv[++index]);
    } else if (arg == "--depth" && index + 1 < argc) {
      config.depth = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
    } else if (arg == "--cycles" && index + 1 < argc) {
      config.cycles = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
    } else if (arg == "--rounds" && index + 1 < argc) {
      config.rounds = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
    } else {
      throw std::runtime_error("unknown or incomplete argument: " + arg);
    }
  }
  if (config.depth == 0 || config.cycles == 0 || config.rounds == 0 || config.depth > 50000) {
    throw std::runtime_error("depth, cycles, and rounds must be valid");
  }
  return config;
}

lob::Event add(lob::OrderId id, lob::Side side, lob::Price price, lob::Quantity qty) {
  lob::Event event;
  event.type = lob::EventType::Add;
  event.order_id = id;
  event.side = side;
  event.price = price;
  event.qty = qty;
  return event;
}

lob::Event cancel(lob::OrderId id) {
  lob::Event event;
  event.type = lob::EventType::Cancel;
  event.order_id = id;
  return event;
}

lob::Event modify(lob::OrderId id, lob::Price price, lob::Quantity qty) {
  lob::Event event;
  event.type = lob::EventType::Modify;
  event.order_id = id;
  event.new_price = price;
  event.new_qty = qty;
  return event;
}

std::vector<lob::Event> build_events(const Config& config) {
  std::vector<lob::Event> events;
  const lob::Price base = 1000;
  lob::OrderId next_id = 1;
  if (config.workload == Workload::Sweep) {
    events.reserve(config.cycles * (config.depth + 1U));
    for (std::size_t cycle = 0; cycle < config.cycles; ++cycle) {
      for (std::size_t index = 0; index < config.depth; ++index) {
        events.push_back(add(next_id++, lob::Side::Ask, base + static_cast<lob::Price>(index), 1));
      }
      events.push_back(add(next_id++, lob::Side::Bid,
                           base + static_cast<lob::Price>(config.depth),
                           static_cast<lob::Quantity>(config.depth)));
    }
  } else if (config.workload == Workload::Cancel) {
    events.reserve(config.depth + config.cycles * 2U);
    for (std::size_t index = 0; index < config.depth; ++index) {
      events.push_back(add(next_id++, lob::Side::Bid,
                           base + static_cast<lob::Price>(index), 1));
    }
    for (std::size_t cycle = 0; cycle < config.cycles; ++cycle) {
      const lob::OrderId id = 1U + (cycle % config.depth);
      events.push_back(cancel(id));
      events.push_back(add(id, lob::Side::Bid,
                           base + static_cast<lob::Price>(cycle % config.depth), 1));
    }
  } else {
    events.reserve(config.depth + config.cycles * config.depth);
    for (std::size_t index = 0; index < config.depth; ++index) {
      events.push_back(add(next_id++, lob::Side::Bid,
                           base + static_cast<lob::Price>(index), 1));
    }
    for (std::size_t cycle = 0; cycle < config.cycles; ++cycle) {
      for (std::size_t index = 0; index < config.depth; ++index) {
        const lob::OrderId id = 1U + static_cast<lob::OrderId>(index);
        events.push_back(modify(id, base + static_cast<lob::Price>(index), 1));
      }
    }
  }
  return events;
}

struct Result {
  double seconds{};
  std::uint64_t p50{};
  std::uint64_t p99{};
  std::uint64_t p999{};
};

std::uint64_t percentile(std::vector<std::uint64_t> values, std::size_t numerator,
                         std::size_t denominator) {
  std::sort(values.begin(), values.end());
  const std::size_t rank = (values.size() * numerator + denominator - 1U) / denominator;
  return values[std::min(rank - 1U, values.size() - 1U)];
}

template <typename Engine>
Result run(const std::vector<lob::Event>& events) {
  Engine book;
  book.reserve_orders(events.size());
  std::vector<lob::Trade> trades;
  trades.reserve(events.size() / 2U + 1U);
  std::vector<std::uint64_t> latencies;
  latencies.reserve(events.size());
  const auto start = Clock::now();
  for (const lob::Event& event : events) {
    const auto event_start = Clock::now();
    trades.clear();
    if (book.process(event, trades) != lob::BookError::None) {
      throw std::runtime_error("tail workload event rejected");
    }
    const auto event_end = Clock::now();
    latencies.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(event_end - event_start).count()));
  }
  const auto end = Clock::now();
  return Result{std::chrono::duration<double>(end - start).count(),
                percentile(latencies, 50, 100), percentile(latencies, 99, 100),
                percentile(latencies, 999, 1000)};
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Config config = parse_args(argc, argv);
    const std::vector<lob::Event> events = build_events(config);
    std::cout << "events: " << events.size() << "\n"
              << "depth: " << config.depth << "\n"
              << "cycles: " << config.cycles << "\n";
    for (std::size_t round = 0; round < config.rounds; ++round) {
      Result result{};
      if (config.backend == Backend::Baseline) {
        result = run<lob::OrderBook>(events);
      } else if (config.backend == Backend::Pooled) {
        result = run<lob::PooledOrderBook>(events);
      } else {
        result = run<lob::LadderOrderBook>(events);
      }
      std::cout << "round " << round + 1U << ": " << result.seconds << " s"
                << "  p50=" << result.p50 << " ns  p99=" << result.p99
                << " ns  p999=" << result.p999 << " ns\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "tail benchmark error: " << error.what() << '\n';
    return 1;
  }
}
