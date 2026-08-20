#include "event_parser.hpp"
#include "order_book.hpp"
#include "spsc_queue.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct TimedEvent {
  lob::Event event;
  std::uint64_t enqueue_ns{};
};

struct Config {
  std::string file_path = "data/benchmark_events.txt";
  std::size_t capacity = 1024;
  std::size_t rounds = 3;
};

struct StateDigest {
  lob::EngineStats stats{};
  std::size_t live_orders{};
  std::size_t trade_count{};
  std::uint64_t trade_hash{};
  std::uint64_t book_hash{};
};

void hash_mix(std::uint64_t& hash, std::uint64_t value) {
  hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
}

void hash_trade(std::uint64_t& hash, const lob::Trade& trade) {
  hash_mix(hash, trade.resting_order_id);
  hash_mix(hash, trade.incoming_order_id);
  hash_mix(hash, static_cast<std::uint64_t>(trade.price));
  hash_mix(hash, static_cast<std::uint64_t>(trade.qty));
  hash_mix(hash, static_cast<std::uint64_t>(trade.aggressor_side == lob::Side::Bid));
}

void hash_orders(std::uint64_t& hash, const std::vector<lob::OrderSnapshot>& orders) {
  for (const lob::OrderSnapshot& order : orders) {
    hash_mix(hash, order.order_id);
    hash_mix(hash, static_cast<std::uint64_t>(order.side == lob::Side::Bid));
    hash_mix(hash, static_cast<std::uint64_t>(order.price));
    hash_mix(hash, static_cast<std::uint64_t>(order.qty));
  }
}

StateDigest digest_book(const lob::OrderBook& book, std::size_t trade_count,
                        std::uint64_t trade_hash) {
  StateDigest digest;
  digest.stats = book.stats();
  digest.live_orders = book.live_order_count();
  digest.trade_count = trade_count;
  digest.trade_hash = trade_hash;
  digest.book_hash = 0;
  hash_orders(digest.book_hash, book.orders(lob::Side::Bid));
  hash_mix(digest.book_hash, 0xfeedbeefU);
  hash_orders(digest.book_hash, book.orders(lob::Side::Ask));
  return digest;
}

bool same_digest(const StateDigest& actual, const StateDigest& expected) {
  return actual.stats.add_requests == expected.stats.add_requests &&
         actual.stats.cancel_requests == expected.stats.cancel_requests &&
         actual.stats.modify_requests == expected.stats.modify_requests &&
         actual.stats.rejected_requests == expected.stats.rejected_requests &&
         actual.stats.trades == expected.stats.trades &&
         actual.stats.traded_qty == expected.stats.traded_qty &&
         actual.live_orders == expected.live_orders &&
         actual.trade_count == expected.trade_count && actual.trade_hash == expected.trade_hash &&
         actual.book_hash == expected.book_hash;
}

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
}

Config parse_args(int argc, char** argv) {
  Config config;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--file" && index + 1 < argc) {
      config.file_path = argv[++index];
    } else if (arg == "--capacity" && index + 1 < argc) {
      config.capacity = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
    } else if (arg == "--rounds" && index + 1 < argc) {
      config.rounds = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
    } else {
      throw std::runtime_error("unknown or incomplete argument: " + arg);
    }
  }
  if (config.capacity == 0 || config.rounds == 0) {
    throw std::runtime_error("capacity and rounds must be greater than zero");
  }
  return config;
}

std::uint64_t percentile(std::vector<std::uint64_t> sorted, std::size_t numerator,
                         std::size_t denominator) {
  if (sorted.empty()) {
    return 0;
  }
  std::sort(sorted.begin(), sorted.end());
  const std::size_t rank = (sorted.size() * numerator + denominator - 1U) / denominator;
  return sorted[std::min(rank - 1U, sorted.size() - 1U)];
}

struct RoundResult {
  double seconds{};
  std::size_t queue_full{};
  std::uint64_t p50_queue_ns{};
  std::uint64_t p99_queue_ns{};
  std::uint64_t p999_queue_ns{};
  std::size_t processed{};
  StateDigest digest{};
};

RoundResult run_round(const std::vector<lob::Event>& events, std::size_t capacity) {
  lob::SpscQueue<TimedEvent> queue(capacity);
  std::atomic<bool> producer_done{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> failed{false};
  std::atomic<std::size_t> queue_full{0};
  std::size_t processed = 0;
  StateDigest digest;
  std::vector<std::uint64_t> queue_latencies;
  queue_latencies.reserve(events.size());
  Clock::time_point processing_end{};

  const auto start = Clock::now();
  std::thread producer([&] {
    for (const lob::Event& event : events) {
      if (stop.load(std::memory_order_acquire)) {
        return;
      }
      TimedEvent timed{event, now_ns()};
      while (!stop.load(std::memory_order_acquire) && !queue.try_push(timed)) {
        queue_full.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
      }
      if (stop.load(std::memory_order_acquire)) {
        return;
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    lob::OrderBook book;
    book.reserve_orders(events.size());
    std::vector<lob::Trade> trades;
    trades.reserve(events.size() / 2U + 1U);
    std::size_t trade_count = 0;
    std::uint64_t trade_hash = 0;
    TimedEvent timed{};
    while (!producer_done.load(std::memory_order_acquire) || processed < events.size()) {
      if (!queue.try_pop(timed)) {
        std::this_thread::yield();
        continue;
      }
      queue_latencies.push_back(now_ns() - timed.enqueue_ns);
      trades.clear();
      if (book.process(timed.event, trades) != lob::BookError::None) {
        failed.store(true, std::memory_order_release);
        stop.store(true, std::memory_order_release);
        return;
      }
      for (const lob::Trade& trade : trades) {
        ++trade_count;
        hash_trade(trade_hash, trade);
      }
      ++processed;
    }
    processing_end = Clock::now();
    digest = digest_book(book, trade_count, trade_hash);
    digest.live_orders = book.live_order_count();
  });

  producer.join();
  consumer.join();
  if (failed.load(std::memory_order_acquire) || queue_latencies.size() != events.size()) {
    throw std::runtime_error("ingress benchmark failed to process all events");
  }

  const std::chrono::duration<double> elapsed = processing_end - start;
  return RoundResult{elapsed.count(),
                     queue_full.load(std::memory_order_relaxed),
                     percentile(queue_latencies, 50, 100),
                     percentile(queue_latencies, 99, 100),
                     percentile(queue_latencies, 999, 1000),
                      processed,
                     digest};
}

StateDigest direct_replay(const std::vector<lob::Event>& events) {
  lob::OrderBook book;
  book.reserve_orders(events.size());
  std::vector<lob::Trade> trades;
  std::size_t trade_count = 0;
  std::uint64_t trade_hash = 0;
  for (const lob::Event& event : events) {
    trades.clear();
    if (book.process(event, trades) != lob::BookError::None) {
      throw std::runtime_error("direct replay rejected an event");
    }
    for (const lob::Trade& trade : trades) {
      ++trade_count;
      hash_trade(trade_hash, trade);
    }
  }
  return digest_book(book, trade_count, trade_hash);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Config config = parse_args(argc, argv);
    const lob::ParseResult parsed = lob::parse_event_file(config.file_path);
    if (!parsed.ok) {
      throw std::runtime_error("parse error on line " + std::to_string(parsed.error_line) +
                               ": " + parsed.error_message);
    }

    std::cout << "events: " << parsed.events.size() << "\n"
              << "capacity: " << config.capacity << "\n"
              << "rounds: " << config.rounds << "\n";
    const StateDigest expected = direct_replay(parsed.events);
    std::vector<RoundResult> results;
    results.reserve(config.rounds);
    for (std::size_t round = 0; round < config.rounds; ++round) {
      results.push_back(run_round(parsed.events, config.capacity));
      const RoundResult& result = results.back();
      if (result.processed != parsed.events.size() || !same_digest(result.digest, expected)) {
        throw std::runtime_error("ingress result differs from direct replay");
      }
      std::cout << "round " << round + 1U << ": " << result.seconds << " s"
                << "  queue_full=" << result.queue_full << "  queue_p50="
                << result.p50_queue_ns << " ns  queue_p99=" << result.p99_queue_ns
                << " ns  queue_p999=" << result.p999_queue_ns << " ns\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ingress benchmark error: " << error.what() << '\n';
    return 1;
  }
}
