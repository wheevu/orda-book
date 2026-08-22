// Per-symbol single-writer partition lab.
//
// This is a standalone experiment. A single writer thread routes events to one
// bounded SPSC queue per synthetic symbol shard, and one matcher thread per
// shard applies its events to its own OrderBook. The lab captures the exact
// staged (enqueue) sequence per shard so a deterministic single-threaded
// direct-replay oracle can verify that threaded per-shard execution is
// equivalent, in-order, lossless, and duplicate-free.
//
// It reuses the existing lob::SpscQueue and lob::OrderBook unchanged. Event
// routing is synthetic and local to this experiment: a symbol is derived from
// order_id, so every event for an order (add/cancel/modify) lands on the same
// shard, which is the invariant a per-symbol matcher needs.
//
// The test translation unit includes this file with PARTITION_LAB_INCLUDE
// defined so it can call the lab internals without a separate header.

#include "event_parser.hpp"
#include "order_book.hpp"
#include "spsc_queue.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// A staged event: the original Event plus the synthetic symbol it was routed to
// and the staging metadata recorded by the single writer at enqueue time.
struct RoutedEvent {
  lob::Event event{};
  std::size_t shard{0};
  std::uint64_t stage_seq{0};
  std::uint64_t stage_ns{0};
};

// Deterministic per-shard digest, mirrored from the repository's ingress
// benchmark so the equivalence check is the same shape the project already uses.
struct StateDigest {
  lob::EngineStats stats{};
  std::size_t live_orders{0};
  std::size_t trade_count{0};
  std::uint64_t trade_hash{0};
  std::uint64_t book_hash{0};
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

// Synthetic, experiment-local symbol routing. The symbol is a pure function of
// order_id, so an order and all later events that reference it always share a
// shard. This is the per-symbol affinity the partition needs.
std::size_t route_symbol(const lob::Event& event, std::size_t shard_count) {
  return static_cast<std::size_t>(event.order_id % shard_count);
}

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
}

// Single-writer enqueue with backpressure accounting. Returns true on success;
// on a full queue it increments `full` and returns false so the caller can
// retry. This is the exact accounting primitive the queue-full tests target.
bool try_enqueue(lob::SpscQueue<RoutedEvent>& queue, const RoutedEvent& value,
                 std::size_t& full) {
  if (queue.try_push(value)) {
    return true;
  }
  ++full;
  return false;
}

// Deterministic direct-replay oracle for one shard's event sequence. Tolerates
// rejected events the same way the matcher does, so the two paths see identical
// sequences and therefore produce identical digests.
StateDigest direct_replay(const std::vector<lob::Event>& events) {
  lob::OrderBook book;
  book.reserve_orders(events.size() == 0 ? 1U : events.size());
  std::vector<lob::Trade> trades;
  trades.reserve(events.size() / 2U + 1U);
  std::size_t trade_count = 0;
  std::uint64_t trade_hash = 0;
  for (const lob::Event& event : events) {
    trades.clear();
    book.process(event, trades);
    for (const lob::Trade& trade : trades) {
      ++trade_count;
      hash_trade(trade_hash, trade);
    }
  }
  return digest_book(book, trade_count, trade_hash);
}

struct PartitionReport {
  std::size_t total_events{0};
  std::size_t shard_count{0};
  // Exact staged sequence recorded by the writer, in enqueue order, per shard.
  std::vector<std::vector<RoutedEvent>> staged;
  // Exact sequence observed by each matcher thread, in consume order, per shard.
  std::vector<std::vector<RoutedEvent>> consumed;
  std::vector<std::size_t> pushed;   // successful enqueues per shard
  std::vector<std::size_t> full;     // rejected (full) enqueues per shard
  std::vector<StateDigest> matcher_digest;
};

// Run the full lab: one writer, one SPSC queue and one matcher thread per shard.
PartitionReport run_partition_lab(const std::vector<lob::Event>& events,
                                  std::size_t shard_count, std::size_t queue_capacity) {
  if (shard_count == 0) {
    throw std::invalid_argument("shard_count must be at least 1");
  }
  if (queue_capacity == 0) {
    throw std::invalid_argument("queue_capacity must be at least 1");
  }

  PartitionReport report;
  report.total_events = events.size();
  report.shard_count = shard_count;
  report.staged.assign(shard_count, {});
  report.consumed.assign(shard_count, {});
  report.pushed.assign(shard_count, 0);
  report.full.assign(shard_count, 0);
  report.matcher_digest.assign(shard_count, StateDigest{});

  // Capacity hint keeps per-shard reservations bounded regardless of input size.
  const std::size_t per_shard_hint = events.size() / shard_count + 16U;
  for (std::size_t s = 0; s < shard_count; ++s) {
    report.staged[s].reserve(per_shard_hint);
    report.consumed[s].reserve(per_shard_hint);
  }

  std::vector<std::unique_ptr<lob::SpscQueue<RoutedEvent>>> queues;
  queues.reserve(shard_count);
  for (std::size_t s = 0; s < shard_count; ++s) {
    queues.emplace_back(std::make_unique<lob::SpscQueue<RoutedEvent>>(queue_capacity));
  }

  std::atomic<bool> producer_done{false};

  std::thread writer([&] {
    std::uint64_t seq = 0;
    for (const lob::Event& event : events) {
      const std::size_t shard = route_symbol(event, shard_count);
      RoutedEvent routed{event, shard, seq, now_ns()};
      report.staged[shard].push_back(routed);
      for (;;) {
        if (try_enqueue(*queues[shard], routed, report.full[shard])) {
          break;
        }
        std::this_thread::yield();
      }
      ++report.pushed[shard];
      ++seq;
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::vector<std::thread> matchers;
  matchers.reserve(shard_count);
  for (std::size_t s = 0; s < shard_count; ++s) {
    matchers.emplace_back([&, s] {
      lob::OrderBook book;
      book.reserve_orders(per_shard_hint);
      std::vector<lob::Trade> trades;
      trades.reserve(per_shard_hint / 2U + 1U);
      std::size_t trade_count = 0;
      std::uint64_t trade_hash = 0;

      RoutedEvent routed{};
      for (;;) {
        if (queues[s]->try_pop(routed)) {
          report.consumed[s].push_back(routed);
          trades.clear();
          book.process(routed.event, trades);
          for (const lob::Trade& trade : trades) {
            ++trade_count;
            hash_trade(trade_hash, trade);
          }
          continue;
        }
        if (producer_done.load(std::memory_order_acquire)) {
          while (queues[s]->try_pop(routed)) {
            report.consumed[s].push_back(routed);
            trades.clear();
            book.process(routed.event, trades);
            for (const lob::Trade& trade : trades) {
              ++trade_count;
              hash_trade(trade_hash, trade);
            }
          }
          break;
        }
        std::this_thread::yield();
      }
      report.matcher_digest[s] = digest_book(book, trade_count, trade_hash);
    });
  }

  writer.join();
  for (std::thread& matcher : matchers) {
    matcher.join();
  }
  return report;
}

// Deterministic synthetic workload generator. Produces a self-contained stream
// of ADD/CANCEL/MODIFY events that never references an unknown order id and
// never re-adds a live id, so the matcher and oracle never diverge on a reject.
std::vector<lob::Event> generate_workload(std::size_t count, std::uint64_t seed,
                                          std::size_t shard_count) {
  std::vector<lob::Event> events;
  events.reserve(count);
  std::uint64_t state = seed == 0 ? 0x1234567890abcdefULL : seed;
  auto next_u64 = [&]() -> std::uint64_t {
    std::uint64_t x = state;
    x ^= x << 13U;
    x ^= x >> 7U;
    x ^= x << 17U;
    state = x;
    return x;
  };

  std::vector<lob::OrderId> live;
  lob::OrderId next_id = 1;

  for (std::size_t i = 0; i < count; ++i) {
    const bool add = live.empty() || (next_u64() % 100U) < 55U;
    lob::Event event;
    event.line_number = i + 1;
    if (add) {
      event.type = lob::EventType::Add;
      event.order_id = next_id++;
      const bool bid = (next_u64() & 1U) == 0U;
      event.side = bid ? lob::Side::Bid : lob::Side::Ask;
      event.price = static_cast<lob::Price>(1 + (next_u64() % 1000U));
      event.qty = static_cast<lob::Quantity>(1 + (next_u64() % 100U));
      live.push_back(event.order_id);
    } else {
      const std::size_t index = next_u64() % live.size();
      const lob::OrderId id = live[index];
      if ((next_u64() & 1U) == 0U) {
        event.type = lob::EventType::Cancel;
        event.order_id = id;
        live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
      } else {
        event.type = lob::EventType::Modify;
        event.order_id = id;
        event.new_price = static_cast<lob::Price>(1 + (next_u64() % 1000U));
        event.new_qty = static_cast<lob::Quantity>(1 + (next_u64() % 100U));
      }
    }
    // Touch shard_count so the compiler cannot drop the parameter in an unused
    // build; routing itself happens in run_partition_lab.
    (void)shard_count;
    events.push_back(event);
  }
  return events;
}

struct Config {
  std::string file_path;
  bool has_file{false};
  std::size_t shard_count{8};
  std::size_t queue_capacity{1024};
  std::size_t generate_count{200000};
  std::uint64_t generate_seed{305419896ULL};
};

Config parse_args(int argc, char** argv) {
  Config config;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--file" && index + 1 < argc) {
      config.file_path = argv[++index];
      config.has_file = true;
    } else if (arg == "--shards" && index + 1 < argc) {
      config.shard_count = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
    } else if (arg == "--capacity" && index + 1 < argc) {
      config.queue_capacity = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
    } else if (arg == "--generate" && index + 1 < argc) {
      config.generate_count = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
    } else if (arg == "--seed" && index + 1 < argc) {
      config.generate_seed = std::strtoull(argv[++index], nullptr, 10);
    } else {
      throw std::runtime_error("unknown or incomplete argument: " + arg);
    }
  }
  if (config.shard_count == 0 || config.queue_capacity == 0 || config.generate_count == 0) {
    throw std::runtime_error("--shards, --capacity and --generate must be greater than zero");
  }
  return config;
}

int run_lab(const Config& config) {
  std::vector<lob::Event> events;
  if (config.has_file) {
    const lob::ParseResult parsed = lob::parse_event_file(config.file_path);
    if (!parsed.ok) {
      std::cerr << "parse error on line " << parsed.error_line << ": " << parsed.error_message
                << '\n';
      return 1;
    }
    events = std::move(parsed.events);
  } else {
    events = generate_workload(config.generate_count, config.generate_seed, config.shard_count);
  }

  std::cout << "events: " << events.size() << '\n'
            << "shards: " << config.shard_count << '\n'
            << "queue_capacity: " << config.queue_capacity << '\n';

  const PartitionReport report =
      run_partition_lab(events, config.shard_count, config.queue_capacity);

  std::size_t total_pushed = 0;
  std::size_t total_full = 0;
  std::size_t total_consumed = 0;
  bool all_match = true;
  for (std::size_t s = 0; s < report.shard_count; ++s) {
    const std::size_t staged = report.staged[s].size();
    total_pushed += report.pushed[s];
    total_full += report.full[s];
    total_consumed += report.consumed[s].size();

    std::vector<lob::Event> staged_events;
    staged_events.reserve(staged);
    for (const RoutedEvent& r : report.staged[s]) {
      staged_events.push_back(r.event);
    }
    const StateDigest oracle = direct_replay(staged_events);
    const bool match = same_digest(report.matcher_digest[s], oracle);
    if (!match) {
      all_match = false;
    }
    std::cout << "shard " << s << ": routed=" << staged
              << " pushed=" << report.pushed[s] << " full=" << report.full[s]
              << " consumed=" << report.consumed[s].size() << " digest_match=" << (match ? 1 : 0)
              << '\n';
  }

  std::cout << "total_pushed: " << total_pushed << '\n'
            << "total_full: " << total_full << '\n'
            << "total_consumed: " << total_consumed << '\n';

  if (total_pushed != events.size() || total_consumed != events.size()) {
    std::cerr << "LOSS: pushed=" << total_pushed << " consumed=" << total_consumed
              << " expected=" << events.size() << '\n';
    return 1;
  }
  if (!all_match) {
    std::cerr << "MISMATCH: a shard digest differed from its direct-replay oracle\n";
    return 1;
  }
  std::cout << "RESULT: OK (no loss, no duplicates, per-shard equivalence holds)\n";
  return 0;
}

}  // namespace

#ifndef PARTITION_LAB_INCLUDE

int main(int argc, char** argv) {
  try {
    return run_lab(parse_args(argc, argv));
  } catch (const std::exception& error) {
    std::cerr << "partition lab error: " << error.what() << '\n';
    return 1;
  }
}

#endif
