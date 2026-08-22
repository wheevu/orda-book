#include "event_parser.hpp"
#include "ladder_order_book.hpp"
#include "order_book.hpp"
#include "pooled_order_book.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace allocation_probe {

std::atomic<bool> enabled{false};
std::atomic<std::size_t> allocations{0};
std::atomic<std::size_t> bytes{0};

void reset() {
  allocations.store(0, std::memory_order_relaxed);
  bytes.store(0, std::memory_order_relaxed);
}

void record(std::size_t size) {
  if (enabled.load(std::memory_order_relaxed)) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(size, std::memory_order_relaxed);
  }
}

void* allocate_aligned(std::size_t size, std::size_t alignment) {
  void* pointer = nullptr;
  const std::size_t actual_size = size == 0 ? 1 : size;
  if (posix_memalign(&pointer, alignment, actual_size) != 0) {
    throw std::bad_alloc();
  }
  record(size);
  return pointer;
}

}  // namespace allocation_probe

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define LOB_THREAD_SANITIZER_ENABLED 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define LOB_THREAD_SANITIZER_ENABLED 1
#endif

#if !defined(LOB_THREAD_SANITIZER_ENABLED)
void* operator new(std::size_t size) {
  void* pointer = std::malloc(size == 0 ? 1 : size);
  if (pointer == nullptr) {
    throw std::bad_alloc();
  }
  allocation_probe::record(size);
  return pointer;
}

void* operator new[](std::size_t size) {
  return ::operator new(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocation_probe::allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
  std::free(pointer);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
  return ::operator new(size, tag);
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  try {
    return ::operator new(size, alignment);
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& tag) noexcept {
  return ::operator new(size, alignment, tag);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
  std::free(pointer);
}
#endif

namespace {

enum class Backend { Baseline, Pooled, Ladder };

Backend parse_backend(const std::string& value) {
  if (value == "baseline") {
    return Backend::Baseline;
  }
  if (value == "pooled") {
    return Backend::Pooled;
  }
  if (value == "ladder") {
    return Backend::Ladder;
  }
  throw std::runtime_error("unknown backend: " + value);
}

struct Config {
  std::string file_path = "data/benchmark_events.txt";
  Backend backend = Backend::Baseline;
  std::size_t rounds = 3;
};

Config parse_args(int argc, char** argv) {
  Config config;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--file" && index + 1 < argc) {
      config.file_path = argv[++index];
    } else if (arg == "--backend" && index + 1 < argc) {
      config.backend = parse_backend(argv[++index]);
    } else if (arg == "--rounds" && index + 1 < argc) {
      config.rounds = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
    } else {
      throw std::runtime_error("unknown or incomplete argument: " + arg);
    }
  }
  if (config.rounds == 0) {
    throw std::runtime_error("rounds must be greater than zero");
  }
  return config;
}

template <typename Engine>
void run(const std::vector<lob::Event>& events, std::size_t rounds) {
  for (std::size_t round = 0; round < rounds; ++round) {
    Engine book;
    book.reserve_orders(events.size());
    std::vector<lob::Trade> trades;
    trades.reserve(events.size() / 2U + 1U);

    allocation_probe::reset();
    allocation_probe::enabled.store(true, std::memory_order_release);
    for (const lob::Event& event : events) {
      trades.clear();
      if (book.process(event, trades) != lob::BookError::None) {
        allocation_probe::enabled.store(false, std::memory_order_release);
        throw std::runtime_error("allocation benchmark event rejected");
      }
    }
    allocation_probe::enabled.store(false, std::memory_order_release);

    const std::size_t count = allocation_probe::allocations.load(std::memory_order_relaxed);
    const std::size_t bytes = allocation_probe::bytes.load(std::memory_order_relaxed);
    std::cout << "round " << round + 1U << ": allocations=" << count
              << " allocations_per_event="
              << static_cast<double>(count) / static_cast<double>(events.size())
              << " bytes=" << bytes << " bytes_per_event="
              << static_cast<double>(bytes) / static_cast<double>(events.size()) << '\n';
  }
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
    if (parsed.events.empty()) {
      throw std::runtime_error("input contains no events");
    }
    std::cout << "events: " << parsed.events.size() << '\n';
    switch (config.backend) {
      case Backend::Baseline:
        std::cout << "backend: baseline\n";
        run<lob::OrderBook>(parsed.events, config.rounds);
        break;
      case Backend::Pooled:
        std::cout << "backend: pooled\n";
        run<lob::PooledOrderBook>(parsed.events, config.rounds);
        break;
      case Backend::Ladder:
        std::cout << "backend: ladder\n";
        run<lob::LadderOrderBook>(parsed.events, config.rounds);
        break;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "allocation benchmark error: " << error.what() << '\n';
    return 1;
  }
}
