#include "spsc_queue.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <cstddef>
#include <thread>

TEST_CASE(spsc_queue_rejects_push_and_pop_at_zero_capacity) {
  lob::SpscQueue<int> queue(0);
  int value = 0;
  CHECK_TRUE(!queue.try_push(1));
  CHECK_TRUE(!queue.try_pop(value));
}

TEST_CASE(spsc_queue_rejects_full_push_and_preserves_order) {
  lob::SpscQueue<int> queue(2);
  int value = 0;
  CHECK_TRUE(queue.try_push(10));
  CHECK_TRUE(queue.try_push(20));
  CHECK_TRUE(!queue.try_push(30));
  CHECK_TRUE(queue.try_pop(value));
  CHECK_EQ(value, 10);
  CHECK_TRUE(queue.try_push(30));
  CHECK_TRUE(queue.try_pop(value));
  CHECK_EQ(value, 20);
  CHECK_TRUE(queue.try_pop(value));
  CHECK_EQ(value, 30);
  CHECK_TRUE(!queue.try_pop(value));
}

TEST_CASE(spsc_queue_transfers_ordered_values_between_threads) {
  constexpr std::size_t kCount = 100000;
  lob::SpscQueue<std::size_t> queue(1024);
  std::atomic<bool> producer_done{false};
  std::atomic<bool> failed{false};

  std::thread producer([&] {
    for (std::size_t value = 0; value < kCount;) {
      if (queue.try_push(value)) {
        ++value;
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    std::size_t expected = 0;
    std::size_t value = 0;
    while (!producer_done.load(std::memory_order_acquire) || expected < kCount) {
      if (!queue.try_pop(value)) {
        std::this_thread::yield();
        continue;
      }
      if (value != expected) {
        failed.store(true, std::memory_order_release);
        return;
      }
      ++expected;
    }
  });

  producer.join();
  consumer.join();
  CHECK_TRUE(!failed.load(std::memory_order_acquire));
}
