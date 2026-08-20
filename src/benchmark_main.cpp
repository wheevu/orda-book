#include "event_parser.hpp"
#include "order_book.hpp"
#include "pooled_order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr lob::Price kFarBidBase = 99000;
constexpr lob::Price kFarAskBase = 101000;
constexpr lob::Price kCrossBase = 100000;
constexpr std::uint64_t kDefaultSeed = 0x12345678ULL;

class XorShift64 {
 public:
  explicit XorShift64(std::uint64_t seed) : state_(seed == 0 ? 0x9e3779b97f4a7c15ULL : seed) {}

  std::uint64_t next() {
    std::uint64_t x = state_;
    x ^= x << 13U;
    x ^= x >> 7U;
    x ^= x << 17U;
    state_ = x;
    return x;
  }

  std::size_t uniform(std::size_t bound) {
    return bound == 0 ? 0 : static_cast<std::size_t>(next() % bound);
  }

 private:
  std::uint64_t state_;
};

enum class WorkloadKind {
  AddOnly,
  CrossHeavy,
  PartialFill,
  Sweep,
  CancelHeavy,
  ModifyHeavy,
};

enum class BackendKind {
  Baseline,
  Pooled,
};

std::string_view to_string(BackendKind backend) {
  return backend == BackendKind::Baseline ? "baseline" : "pooled";
}

std::optional<BackendKind> parse_backend(std::string_view token) {
  if (token == "baseline") {
    return BackendKind::Baseline;
  }
  if (token == "pooled") {
    return BackendKind::Pooled;
  }
  return std::nullopt;
}

std::string_view to_string(WorkloadKind workload) {
  switch (workload) {
    case WorkloadKind::AddOnly:
      return "add-only";
    case WorkloadKind::CrossHeavy:
      return "cross-heavy";
    case WorkloadKind::PartialFill:
      return "partial-fill";
    case WorkloadKind::Sweep:
      return "sweep";
    case WorkloadKind::CancelHeavy:
      return "cancel-heavy";
    case WorkloadKind::ModifyHeavy:
      return "modify-heavy";
  }
  return "unknown";
}

std::optional<WorkloadKind> parse_workload(std::string_view token) {
  if (token == "add-only") {
    return WorkloadKind::AddOnly;
  }
  if (token == "cross-heavy") {
    return WorkloadKind::CrossHeavy;
  }
  if (token == "partial-fill") {
    return WorkloadKind::PartialFill;
  }
  if (token == "sweep") {
    return WorkloadKind::Sweep;
  }
  if (token == "cancel-heavy") {
    return WorkloadKind::CancelHeavy;
  }
  if (token == "modify-heavy") {
    return WorkloadKind::ModifyHeavy;
  }
  return std::nullopt;
}

lob::Side opposite(lob::Side side) {
  return side == lob::Side::Bid ? lob::Side::Ask : lob::Side::Bid;
}

lob::Price far_price(lob::Side side, XorShift64& rng) {
  if (side == lob::Side::Bid) {
    return kFarBidBase - static_cast<lob::Price>(rng.uniform(64));
  }
  return kFarAskBase + static_cast<lob::Price>(rng.uniform(64));
}

lob::Quantity random_qty(XorShift64& rng, lob::Quantity max_qty) {
  return 1 + static_cast<lob::Quantity>(rng.uniform(static_cast<std::size_t>(max_qty)));
}

struct BenchmarkConfig {
  std::string file_path;
  std::string write_file_path;
  std::optional<WorkloadKind> workload;
  std::size_t event_count = 0;
  std::size_t rounds = 5;
  std::uint64_t seed = kDefaultSeed;
  BackendKind backend = BackendKind::Baseline;
  bool engine_only = false;
  bool collect_latency = true;
  bool reuse_trades = false;
  bool calibrate_clock = false;
};

struct WorkloadStream {
  WorkloadKind kind{};
  std::vector<lob::Event> setup;
  std::vector<lob::Event> timed;
};

struct RunStats {
  std::size_t setup_events{};
  std::size_t timed_events{};
  std::size_t trades{};
  std::size_t rejected_requests{};
  std::size_t live_orders{};
  lob::Quantity traded_qty{};
  double seconds{};
  std::uint64_t p50_ns{};
  std::uint64_t p99_ns{};
  std::uint64_t p999_ns{};
  std::size_t latency_samples{};
};

struct LiveOrder {
  lob::OrderId order_id{};
  lob::Side side{};
};

class LivePool {
 public:
  void add(lob::OrderId order_id, lob::Side side) {
    index_.emplace(order_id, orders_.size());
    orders_.push_back(LiveOrder{order_id, side});
  }

  void remove(lob::OrderId order_id) {
    const auto it = index_.find(order_id);
    if (it == index_.end()) {
      throw std::runtime_error("live pool remove on missing order");
    }

    const std::size_t index = it->second;
    const std::size_t last_index = orders_.size() - 1;
    if (index != last_index) {
      orders_[index] = orders_[last_index];
      index_[orders_[index].order_id] = index;
    }
    orders_.pop_back();
    index_.erase(it);
  }

  [[nodiscard]] bool empty() const { return orders_.empty(); }
  [[nodiscard]] std::size_t size() const { return orders_.size(); }

  [[nodiscard]] LiveOrder random(XorShift64& rng) const {
    if (orders_.empty()) {
      throw std::runtime_error("live pool is empty");
    }
    return orders_[rng.uniform(orders_.size())];
  }

 private:
  std::vector<LiveOrder> orders_;
  std::unordered_map<lob::OrderId, std::size_t> index_;
};

class StreamBuilder {
 public:
  explicit StreamBuilder(WorkloadKind kind) : stream_{kind, {}, {}} {}

  lob::OrderId add_setup(lob::Side side, lob::Price price, lob::Quantity qty) {
    return add_event(stream_.setup, side, price, qty);
  }

  lob::OrderId add_timed(lob::Side side, lob::Price price, lob::Quantity qty) {
    return add_event(stream_.timed, side, price, qty);
  }

  void cancel_timed(lob::OrderId order_id) {
    lob::Event event;
    event.type = lob::EventType::Cancel;
    event.order_id = order_id;
    event.line_number = next_line_++;
    stream_.timed.push_back(event);
  }

  void modify_timed(lob::OrderId order_id, lob::Price new_price, lob::Quantity new_qty) {
    lob::Event event;
    event.type = lob::EventType::Modify;
    event.order_id = order_id;
    event.new_price = new_price;
    event.new_qty = new_qty;
    event.line_number = next_line_++;
    stream_.timed.push_back(event);
  }

  [[nodiscard]] const WorkloadStream& stream() const { return stream_; }
  [[nodiscard]] WorkloadStream take() { return std::move(stream_); }

 private:
  lob::OrderId add_event(std::vector<lob::Event>& destination, lob::Side side, lob::Price price,
                         lob::Quantity qty) {
    lob::Event event;
    event.type = lob::EventType::Add;
    event.order_id = next_order_id_++;
    event.side = side;
    event.price = price;
    event.qty = qty;
    event.line_number = next_line_++;
    destination.push_back(event);
    return event.order_id;
  }

  WorkloadStream stream_;
  lob::OrderId next_order_id_ = 1;
  std::size_t next_line_ = 1;
};

BenchmarkConfig parse_args(int argc, char** argv) {
  BenchmarkConfig config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--file" && i + 1 < argc) {
      config.file_path = argv[++i];
      continue;
    }
    if ((arg == "--events" || arg == "--generate") && i + 1 < argc) {
      config.event_count = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
      continue;
    }
    if (arg == "--workload" && i + 1 < argc) {
      config.workload = parse_workload(argv[++i]);
      if (!config.workload.has_value()) {
        throw std::runtime_error("unknown workload");
      }
      continue;
    }
    if (arg == "--rounds" && i + 1 < argc) {
      config.rounds = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
      continue;
    }
    if (arg == "--seed" && i + 1 < argc) {
      config.seed = std::strtoull(argv[++i], nullptr, 10);
      continue;
    }
    if (arg == "--backend" && i + 1 < argc) {
      const std::optional<BackendKind> backend = parse_backend(argv[++i]);
      if (!backend.has_value()) {
        throw std::runtime_error("unknown backend");
      }
      config.backend = *backend;
      continue;
    }
    if (arg == "--write-file" && i + 1 < argc) {
      config.write_file_path = argv[++i];
      continue;
    }
    if (arg == "--engine-only") {
      config.engine_only = true;
      continue;
    }
    if (arg == "--no-latency") {
      config.collect_latency = false;
      continue;
    }
    if (arg == "--reuse-trades") {
      config.reuse_trades = true;
      continue;
    }
    if (arg == "--calibrate-clock") {
      config.calibrate_clock = true;
      continue;
    }
    throw std::runtime_error("unknown argument: " + arg);
  }

  if (config.calibrate_clock) {
    if (config.event_count != 0 || !config.file_path.empty() || config.engine_only ||
        !config.write_file_path.empty() || config.workload.has_value()) {
      throw std::runtime_error("--calibrate-clock cannot be combined with benchmark inputs");
    }
    return config;
  }

  const bool generated_mode = config.event_count != 0;
  const bool file_mode = !config.file_path.empty();
  if (generated_mode == file_mode) {
    throw std::runtime_error("choose exactly one of --file or --events");
  }
  if (generated_mode && !config.workload.has_value()) {
    throw std::runtime_error("--workload is required with --events");
  }
  if (file_mode && config.workload.has_value()) {
    throw std::runtime_error("--workload cannot be used with --file");
  }
  if (!config.write_file_path.empty() && !generated_mode) {
    throw std::runtime_error("--write-file requires --events");
  }
  if (config.rounds == 0) {
    throw std::runtime_error("--rounds must be greater than zero");
  }
  return config;
}

void write_events_file(const WorkloadStream& stream, const std::string& path) {
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("failed to open output file");
  }

  const auto write_event = [&](const lob::Event& event) {
    switch (event.type) {
      case lob::EventType::Add:
        output << "ADD " << event.order_id << ' ' << lob::to_string(event.side) << ' ' << event.price
               << ' ' << event.qty << '\n';
        break;
      case lob::EventType::Cancel:
        output << "CANCEL " << event.order_id << '\n';
        break;
      case lob::EventType::Modify:
        output << "MODIFY " << event.order_id << ' ' << event.new_price << ' ' << event.new_qty
               << '\n';
        break;
    }
  };

  for (const lob::Event& event : stream.setup) {
    write_event(event);
  }
  for (const lob::Event& event : stream.timed) {
    write_event(event);
  }
}

WorkloadStream build_add_only(std::size_t count, std::uint64_t seed) {
  StreamBuilder builder(WorkloadKind::AddOnly);
  XorShift64 rng(seed);

  for (std::size_t i = 0; i < count; ++i) {
    const lob::Side side = (i & 1U) == 0U ? lob::Side::Bid : lob::Side::Ask;
    builder.add_timed(side, far_price(side, rng), random_qty(rng, 256));
  }
  return builder.take();
}

WorkloadStream build_cross_heavy(std::size_t count, std::uint64_t seed) {
  StreamBuilder builder(WorkloadKind::CrossHeavy);
  XorShift64 rng(seed);

  while (builder.stream().timed.size() < count) {
    const bool ask_first = (builder.stream().timed.size() & 1U) == 0U;
    const lob::Price price = kCrossBase + static_cast<lob::Price>(rng.uniform(8));
    const lob::Quantity qty = random_qty(rng, 64);
    if (ask_first) {
      builder.add_timed(lob::Side::Ask, price, qty);
      if (builder.stream().timed.size() < count) {
        builder.add_timed(lob::Side::Bid, price + static_cast<lob::Price>(rng.uniform(2)), qty);
      }
    } else {
      builder.add_timed(lob::Side::Bid, price, qty);
      if (builder.stream().timed.size() < count) {
        builder.add_timed(lob::Side::Ask, price - static_cast<lob::Price>(rng.uniform(2)), qty);
      }
    }
  }
  return builder.take();
}

WorkloadStream build_partial_fill(std::size_t count, std::uint64_t seed) {
  StreamBuilder builder(WorkloadKind::PartialFill);
  XorShift64 rng(seed);

  while (builder.stream().timed.size() < count) {
    const std::size_t remaining = count - builder.stream().timed.size();
    if (remaining == 1) {
      builder.add_timed(lob::Side::Bid, far_price(lob::Side::Bid, rng), random_qty(rng, 16));
      break;
    }

    const std::size_t chips = std::min<std::size_t>(32, remaining - 1);
    const bool ask_head = ((builder.stream().timed.size() / 2U) & 1U) == 0U;
    const lob::Side resting_side = ask_head ? lob::Side::Ask : lob::Side::Bid;
    const lob::Side incoming_side = opposite(resting_side);
    const lob::Price price = kCrossBase + static_cast<lob::Price>(rng.uniform(4));
    builder.add_timed(resting_side, price, static_cast<lob::Quantity>(chips));
    for (std::size_t chip = 0; chip < chips; ++chip) {
      builder.add_timed(incoming_side, price, 1);
    }
  }
  return builder.take();
}

WorkloadStream build_sweep(std::size_t count, std::uint64_t seed) {
  StreamBuilder builder(WorkloadKind::Sweep);
  XorShift64 rng(seed);

  while (builder.stream().timed.size() < count) {
    const std::size_t remaining = count - builder.stream().timed.size();
    if (remaining == 1) {
      builder.add_timed(lob::Side::Ask, far_price(lob::Side::Ask, rng), random_qty(rng, 16));
      break;
    }

    const std::size_t levels = std::min<std::size_t>(8, remaining - 1);
    const lob::Quantity level_qty = 4;
    const bool ask_ladder = ((builder.stream().timed.size() / 3U) & 1U) == 0U;
    if (ask_ladder) {
      for (std::size_t level = 0; level < levels; ++level) {
        builder.add_timed(lob::Side::Ask, kCrossBase + static_cast<lob::Price>(level), level_qty);
      }
      builder.add_timed(lob::Side::Bid, kCrossBase + static_cast<lob::Price>(levels - 1),
                        static_cast<lob::Quantity>(levels) * level_qty);
    } else {
      for (std::size_t level = 0; level < levels; ++level) {
        builder.add_timed(lob::Side::Bid, kCrossBase - static_cast<lob::Price>(level), level_qty);
      }
      builder.add_timed(lob::Side::Ask, kCrossBase - static_cast<lob::Price>(levels - 1),
                        static_cast<lob::Quantity>(levels) * level_qty);
    }
  }
  return builder.take();
}

WorkloadStream build_cancel_heavy(std::size_t count, std::uint64_t seed) {
  StreamBuilder builder(WorkloadKind::CancelHeavy);
  XorShift64 rng(seed);
  LivePool pool;

  const std::size_t target_live = std::max<std::size_t>(1024, std::min<std::size_t>(8192, count / 8 + 1));
  for (std::size_t i = 0; i < target_live; ++i) {
    const lob::Side side = (i & 1U) == 0U ? lob::Side::Bid : lob::Side::Ask;
    const lob::OrderId order_id = builder.add_setup(side, far_price(side, rng), random_qty(rng, 256));
    pool.add(order_id, side);
  }

  while (builder.stream().timed.size() < count) {
    const LiveOrder live = pool.random(rng);
    builder.cancel_timed(live.order_id);
    pool.remove(live.order_id);

    if (builder.stream().timed.size() < count) {
      const lob::Side replacement_side = (rng.next() & 1ULL) == 0ULL ? lob::Side::Bid : lob::Side::Ask;
      const lob::OrderId replacement =
          builder.add_timed(replacement_side, far_price(replacement_side, rng), random_qty(rng, 256));
      pool.add(replacement, replacement_side);
    }
  }
  return builder.take();
}

WorkloadStream build_modify_heavy(std::size_t count, std::uint64_t seed) {
  StreamBuilder builder(WorkloadKind::ModifyHeavy);
  XorShift64 rng(seed);
  LivePool pool;

  const std::size_t target_live = std::max<std::size_t>(1024, std::min<std::size_t>(4096, count / 8 + 1));
  for (std::size_t i = 0; i < target_live; ++i) {
    const lob::Side side = (i & 1U) == 0U ? lob::Side::Bid : lob::Side::Ask;
    const lob::OrderId order_id = builder.add_setup(side, far_price(side, rng), random_qty(rng, 128));
    pool.add(order_id, side);
  }

  while (builder.stream().timed.size() < count) {
    const std::size_t remaining = count - builder.stream().timed.size();
    const bool cross_modify = remaining >= 3 && rng.uniform(8) == 0;
    const LiveOrder live = pool.random(rng);

    if (!cross_modify) {
      builder.modify_timed(live.order_id, far_price(live.side, rng), random_qty(rng, 128));
      continue;
    }

    builder.add_timed(opposite(live.side), kCrossBase, 1);
    builder.modify_timed(live.order_id, kCrossBase, 1);
    pool.remove(live.order_id);

    const lob::OrderId replacement =
        builder.add_timed(live.side, far_price(live.side, rng), random_qty(rng, 128));
    pool.add(replacement, live.side);
  }
  return builder.take();
}

WorkloadStream build_workload(WorkloadKind workload, std::size_t count, std::uint64_t seed) {
  switch (workload) {
    case WorkloadKind::AddOnly:
      return build_add_only(count, seed);
    case WorkloadKind::CrossHeavy:
      return build_cross_heavy(count, seed);
    case WorkloadKind::PartialFill:
      return build_partial_fill(count, seed);
    case WorkloadKind::Sweep:
      return build_sweep(count, seed);
    case WorkloadKind::CancelHeavy:
      return build_cancel_heavy(count, seed);
    case WorkloadKind::ModifyHeavy:
      return build_modify_heavy(count, seed);
  }
  throw std::runtime_error("unreachable workload");
}

std::uint64_t percentile_ns(const std::vector<std::uint64_t>& sorted_latencies,
                            std::size_t numerator, std::size_t denominator) {
  if (sorted_latencies.empty()) {
    return 0;
  }
  const std::size_t rank =
      (sorted_latencies.size() * numerator + denominator - 1U) / denominator;
  const std::size_t index = std::min(rank - 1U, sorted_latencies.size() - 1U);
  return sorted_latencies[index];
}

template <typename Engine>
RunStats make_run_stats(std::size_t setup_events, std::size_t timed_events,
                        const Engine& book,
                        std::size_t pre_timed_trade_count, lob::Quantity pre_timed_traded_qty,
                        double seconds, std::vector<std::uint64_t> latencies) {
  std::sort(latencies.begin(), latencies.end());
  const auto& stats = book.stats();
  return RunStats{setup_events,
                  timed_events,
                  stats.trades - pre_timed_trade_count,
                  stats.rejected_requests,
                  book.live_order_count(),
                  stats.traded_qty - pre_timed_traded_qty,
                  seconds,
                  percentile_ns(latencies, 50, 100),
                  percentile_ns(latencies, 99, 100),
                  percentile_ns(latencies, 999, 1000),
                  latencies.size()};
}

template <typename Engine>
RunStats run_engine(const WorkloadStream& stream, bool collect_latency, bool reuse_trades) {
  Engine book;
  book.reserve_orders(stream.setup.size() + stream.timed.size());
  std::vector<lob::Trade> trades;
  trades.reserve((stream.setup.size() + stream.timed.size()) / 2 + 1);
  std::vector<std::uint64_t> latencies;
  if (collect_latency) {
    latencies.reserve(stream.timed.size());
  }

  for (const lob::Event& event : stream.setup) {
    if (reuse_trades) {
      trades.clear();
    }
    const lob::BookError error = book.process(event, trades);
    if (error != lob::BookError::None) {
      throw std::runtime_error("setup event rejected");
    }
  }

  const std::size_t pre_timed_trade_count = book.stats().trades;
  const lob::Quantity pre_timed_traded_qty = book.stats().traded_qty;

  const auto start = Clock::now();
  for (const lob::Event& event : stream.timed) {
    if (reuse_trades) {
      trades.clear();
    }
    const auto event_start = collect_latency ? Clock::now() : Clock::time_point{};
    const lob::BookError error = book.process(event, trades);
    const auto event_end = collect_latency ? Clock::now() : Clock::time_point{};
    if (error != lob::BookError::None) {
      throw std::runtime_error("timed event rejected");
    }
    if (collect_latency) {
      latencies.push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(event_end - event_start).count()));
    }
  }
  const auto end = Clock::now();

  const std::chrono::duration<double> elapsed = end - start;
  return make_run_stats(stream.setup.size(), stream.timed.size(), book,
                        pre_timed_trade_count, pre_timed_traded_qty,
                        elapsed.count(), std::move(latencies));
}

template <typename Engine>
RunStats run_parse_and_engine(const std::string& file_path, bool collect_latency,
                              bool reuse_trades) {
  const auto start = Clock::now();
  const lob::ParseResult parsed = lob::parse_event_file(file_path);
  if (!parsed.ok) {
    throw std::runtime_error("parse failed");
  }

  Engine book;
  book.reserve_orders(parsed.events.size());
  std::vector<lob::Trade> trades;
  trades.reserve(parsed.events.size() / 2 + 1);
  std::vector<std::uint64_t> latencies;
  if (collect_latency) {
    latencies.reserve(parsed.events.size());
  }
  for (const lob::Event& event : parsed.events) {
    if (reuse_trades) {
      trades.clear();
    }
    const auto event_start = collect_latency ? Clock::now() : Clock::time_point{};
    const lob::BookError error = book.process(event, trades);
    const auto event_end = collect_latency ? Clock::now() : Clock::time_point{};
    if (error != lob::BookError::None) {
      throw std::runtime_error("replay failed");
    }
    if (collect_latency) {
      latencies.push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(event_end - event_start).count()));
    }
  }
  const auto end = Clock::now();

  const std::chrono::duration<double> elapsed = end - start;
  return make_run_stats(0, parsed.events.size(), book, 0, 0, elapsed.count(),
                        std::move(latencies));
}

double events_per_second(const RunStats& stats) {
  return stats.seconds == 0.0 ? 0.0 : static_cast<double>(stats.timed_events) / stats.seconds;
}

double ns_per_event(const RunStats& stats) {
  return stats.timed_events == 0 ? 0.0 : (stats.seconds * 1e9) / static_cast<double>(stats.timed_events);
}

RunStats median_run(std::vector<RunStats> runs) {
  std::sort(runs.begin(), runs.end(), [](const RunStats& left, const RunStats& right) {
    return left.seconds < right.seconds;
  });
  return runs[runs.size() / 2];
}

void calibrate_clock() {
  constexpr std::size_t kSamples = 100000;
  std::vector<std::uint64_t> samples;
  samples.reserve(kSamples);
  for (std::size_t index = 0; index < kSamples; ++index) {
    const auto start = Clock::now();
    const auto end = Clock::now();
    samples.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
  }
  std::sort(samples.begin(), samples.end());
  std::cout << "mode: clock-calibration\n"
            << "samples: " << samples.size() << "\n"
            << "p50: " << percentile_ns(samples, 50, 100) << " ns\n"
            << "p99: " << percentile_ns(samples, 99, 100) << " ns\n"
            << "p999: " << percentile_ns(samples, 999, 1000) << " ns\n";
}

void print_round(std::size_t round, const RunStats& stats) {
  std::cout << "round " << round << ": " << std::fixed << std::setprecision(6) << stats.seconds << " s"
             << "  " << std::setprecision(2) << events_per_second(stats) << " ev/s"
             << "  " << ns_per_event(stats) << " ns/ev"
             << "  latency_samples=" << stats.latency_samples;
  if (stats.latency_samples != 0) {
    std::cout << "  p50=" << stats.p50_ns << " ns"
              << "  p99=" << stats.p99_ns << " ns"
              << "  p999=" << stats.p999_ns << " ns";
  }
  std::cout
             << "  trades=" << stats.trades << " traded_qty=" << stats.traded_qty
             << " rejected=" << stats.rejected_requests << " live=" << stats.live_orders << '\n';
}

void print_summary(const RunStats& stats) {
  std::cout << "median : " << std::fixed << std::setprecision(6) << stats.seconds << " s"
             << "  " << std::setprecision(2) << events_per_second(stats) << " ev/s"
             << "  " << ns_per_event(stats) << " ns/ev"
             << "  latency_samples=" << stats.latency_samples;
  if (stats.latency_samples != 0) {
    std::cout << "  p50=" << stats.p50_ns << " ns"
              << "  p99=" << stats.p99_ns << " ns"
              << "  p999=" << stats.p999_ns << " ns";
  }
  std::cout
             << "  trades=" << stats.trades << " traded_qty=" << stats.traded_qty
            << " rejected=" << stats.rejected_requests << " live=" << stats.live_orders << '\n';
}

void print_generated_header(const BenchmarkConfig& config, const WorkloadStream& stream) {
  std::cout << "mode: generated-engine\n";
  std::cout << "workload: " << to_string(stream.kind) << "\n";
  std::cout << "seed: " << config.seed << "\n";
  std::cout << "backend: " << to_string(config.backend) << "\n";
  std::cout << "setup_events: " << stream.setup.size() << "\n";
  std::cout << "timed_events: " << stream.timed.size() << "\n";
  std::cout << "latency: " << (config.collect_latency ? "enabled" : "disabled") << "\n";
  std::cout << "trade_buffer: " << (config.reuse_trades ? "reused" : "accumulated") << "\n";
}

void print_file_header(const BenchmarkConfig& config, std::size_t event_count) {
  std::cout << "mode: " << (config.engine_only ? "file-engine-only" : "file-parse+engine") << "\n";
  std::cout << "file: " << config.file_path << "\n";
  std::cout << "backend: " << to_string(config.backend) << "\n";
  std::cout << "timed_events: " << event_count << "\n";
  std::cout << "latency: " << (config.collect_latency ? "enabled" : "disabled") << "\n";
  std::cout << "trade_buffer: " << (config.reuse_trades ? "reused" : "accumulated") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const BenchmarkConfig config = parse_args(argc, argv);

    if (config.calibrate_clock) {
      calibrate_clock();
      return 0;
    }

    const auto run_stream = [&config](const WorkloadStream& stream) {
      if (config.backend == BackendKind::Pooled) {
        return run_engine<lob::PooledOrderBook>(stream, config.collect_latency,
                                                config.reuse_trades);
      }
      return run_engine<lob::OrderBook>(stream, config.collect_latency, config.reuse_trades);
    };
    const auto run_file = [&config](const std::string& file_path) {
      if (config.backend == BackendKind::Pooled) {
        return run_parse_and_engine<lob::PooledOrderBook>(file_path, config.collect_latency,
                                                          config.reuse_trades);
      }
      return run_parse_and_engine<lob::OrderBook>(file_path, config.collect_latency,
                                                  config.reuse_trades);
    };

    if (config.event_count != 0) {
      const WorkloadStream stream = build_workload(*config.workload, config.event_count, config.seed);
      if (!config.write_file_path.empty()) {
        write_events_file(stream, config.write_file_path);
      }

      print_generated_header(config, stream);
      std::vector<RunStats> runs;
      runs.reserve(config.rounds);
      for (std::size_t round = 0; round < config.rounds; ++round) {
        runs.push_back(run_stream(stream));
        print_round(round + 1, runs.back());
      }
      print_summary(median_run(runs));
      return 0;
    }

    if (config.engine_only) {
      const lob::ParseResult parsed = lob::parse_event_file(config.file_path);
      if (!parsed.ok) {
        std::cerr << "parse error on line " << parsed.error_line << ": " << parsed.error_message << '\n';
        return 1;
      }

      WorkloadStream stream;
      stream.timed = parsed.events;
      print_file_header(config, stream.timed.size());

      std::vector<RunStats> runs;
      runs.reserve(config.rounds);
      for (std::size_t round = 0; round < config.rounds; ++round) {
        runs.push_back(run_stream(stream));
        print_round(round + 1, runs.back());
      }
      print_summary(median_run(runs));
      return 0;
    }

    const lob::ParseResult parsed = lob::parse_event_file(config.file_path);
    if (!parsed.ok) {
      std::cerr << "parse error on line " << parsed.error_line << ": " << parsed.error_message << '\n';
      return 1;
    }

    print_file_header(config, parsed.events.size());
    std::vector<RunStats> runs;
    runs.reserve(config.rounds);
    for (std::size_t round = 0; round < config.rounds; ++round) {
      runs.push_back(run_file(config.file_path));
      print_round(round + 1, runs.back());
    }
    print_summary(median_run(runs));
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "benchmark error: " << ex.what() << '\n';
    return 1;
  }
}
