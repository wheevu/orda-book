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
};

RoundResult run_round(const std::vector<lob::Event>& events, std::size_t capacity) {
  lob::SpscQueue<TimedEvent> queue(capacity);
  std::atomic<bool> producer_done{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> failed{false};
  std::atomic<std::size_t> queue_full{0};
  std::vector<std::uint64_t> queue_latencies;
  queue_latencies.reserve(events.size());

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
    std::size_t processed = 0;
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
      ++processed;
    }
  });

  producer.join();
  consumer.join();
  const auto end = Clock::now();
  if (failed.load(std::memory_order_acquire) || queue_latencies.size() != events.size()) {
    throw std::runtime_error("ingress benchmark failed to process all events");
  }

  const std::chrono::duration<double> elapsed = end - start;
  return RoundResult{elapsed.count(),
                     queue_full.load(std::memory_order_relaxed),
                     percentile(queue_latencies, 50, 100),
                     percentile(queue_latencies, 99, 100),
                     percentile(queue_latencies, 999, 1000)};
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
    std::vector<RoundResult> results;
    results.reserve(config.rounds);
    for (std::size_t round = 0; round < config.rounds; ++round) {
      results.push_back(run_round(parsed.events, config.capacity));
      const RoundResult& result = results.back();
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
