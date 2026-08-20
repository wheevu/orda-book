#pragma once

#include <atomic>
#include <cstddef>
#include <memory>

namespace lob {

template <typename T>
class SpscQueue {
 public:
  explicit SpscQueue(std::size_t capacity)
      : capacity_(capacity), slots_(capacity == 0 ? nullptr : std::make_unique<T[]>(capacity)) {}

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  [[nodiscard]] std::size_t capacity() const { return capacity_; }

  bool try_push(const T& value) {
    if (capacity_ == 0) {
      return false;
    }
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (tail - head == capacity_) {
      return false;
    }
    slots_[tail % capacity_] = value;
    tail_.store(tail + 1U, std::memory_order_release);
    return true;
  }

  bool try_pop(T& value) {
    if (capacity_ == 0) {
      return false;
    }
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (head == tail) {
      return false;
    }
    value = slots_[head % capacity_];
    head_.store(head + 1U, std::memory_order_release);
    return true;
  }

 private:
  const std::size_t capacity_;
  std::unique_ptr<T[]> slots_;
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace lob
