// Tests for the per-symbol single-writer partition lab.
//
// Includes the lab implementation (with PARTITION_LAB_INCLUDE so main() is
// excluded) to exercise its internals directly. No extra header is introduced:
// the lab owns exactly the three files the task scopes.

#define PARTITION_LAB_INCLUDE
#include "../src/partition_lab_main.cpp"

#include "test_framework.hpp"

#include <cstddef>
#include <vector>

namespace {

std::vector<std::uint64_t> staged_seqs(const std::vector<RoutedEvent>& events) {
  std::vector<std::uint64_t> out;
  out.reserve(events.size());
  for (const RoutedEvent& e : events) {
    out.push_back(e.stage_seq);
  }
  return out;
}

std::vector<lob::OrderId> staged_order_ids(const std::vector<RoutedEvent>& events) {
  std::vector<lob::OrderId> out;
  out.reserve(events.size());
  for (const RoutedEvent& e : events) {
    out.push_back(e.event.order_id);
  }
  return out;
}

bool seqs_equal(const std::vector<std::uint64_t>& a, const std::vector<std::uint64_t>& b) {
  return a == b;
}

}  // namespace

TEST_CASE(partition_routing_is_deterministic_and_symbol_affine) {
  const std::size_t shards = 8;

  // route_symbol is a pure function of order_id and shard count.
  lob::Event sample;
  sample.order_id = 12345;
  CHECK_EQ(route_symbol(sample, shards), static_cast<std::size_t>(12345 % shards));
  CHECK_EQ(route_symbol(sample, shards), route_symbol(sample, shards));

  // Every event that references an order id routes to the same shard, so an
  // order's add, cancel, and modify all land on one matcher.
  const auto workload = generate_workload(4000, 305419896ULL, shards);
  std::vector<std::size_t> shard_of_order;
  shard_of_order.assign(12345 + 1, static_cast<std::size_t>(-1));
  for (const lob::Event& event : workload) {
    const std::size_t shard = route_symbol(event, shards);
    CHECK_TRUE(shard < shards);
    if (event.order_id < shard_of_order.size()) {
      if (shard_of_order[event.order_id] != static_cast<std::size_t>(-1)) {
        CHECK_EQ(shard_of_order[event.order_id], shard);
      } else {
        shard_of_order[event.order_id] = shard;
      }
    }
  }

  // Routing is reproducible for the same workload and parameters.
  const auto again = generate_workload(4000, 305419896ULL, shards);
  CHECK_EQ(workload.size(), again.size());
  for (std::size_t i = 0; i < workload.size(); ++i) {
    CHECK_EQ(route_symbol(workload[i], shards), route_symbol(again[i], shards));
  }
}

TEST_CASE(partition_per_symbol_order_is_preserved) {
  const auto events = generate_workload(6000, 99ULL, 8);
  const PartitionReport report = run_partition_lab(events, 8, 256);

  for (std::size_t s = 0; s < report.shard_count; ++s) {
    // The matcher observes exactly the staged sequence, in order.
    CHECK_TRUE(seqs_equal(staged_seqs(report.consumed[s]), staged_seqs(report.staged[s])));
    CHECK_TRUE(staged_order_ids(report.consumed[s]) == staged_order_ids(report.staged[s]));

    // Staging sequence and timestamps are monotonic within a shard (no reorder).
    for (std::size_t i = 1; i < report.staged[s].size(); ++i) {
      CHECK_TRUE(report.staged[s][i].stage_seq > report.staged[s][i - 1].stage_seq);
      CHECK_TRUE(report.staged[s][i].stage_ns >= report.staged[s][i - 1].stage_ns);
    }
  }
}

TEST_CASE(partition_no_loss_and_no_duplicates) {
  const auto events = generate_workload(7000, 7ULL, 16);
  const PartitionReport report = run_partition_lab(events, 16, 64);

  std::size_t total_staged = 0;
  std::size_t total_consumed = 0;
  for (std::size_t s = 0; s < report.shard_count; ++s) {
    total_staged += report.staged[s].size();
    total_consumed += report.consumed[s].size();

    // Every routed copy is unique per shard: consumed count equals staged count
    // and the sequences are identical, so no event is lost or duplicated.
    CHECK_EQ(report.consumed[s].size(), report.staged[s].size());
    CHECK_TRUE(seqs_equal(staged_seqs(report.consumed[s]), staged_seqs(report.staged[s])));

    // Routing is internally consistent: every staged event actually belongs to
    // its shard.
    for (const RoutedEvent& r : report.staged[s]) {
      CHECK_EQ(route_symbol(r.event, report.shard_count), s);
    }
  }

  // The staged events form an exact partition of the input: no loss, no dup.
  CHECK_EQ(total_staged, events.size());
  CHECK_EQ(total_consumed, events.size());
}

TEST_CASE(partition_queue_full_accounting_is_exact) {
  // Deterministic unit check of the enqueue accounting primitive against a
  // non-drained bounded queue: capacity 4, push 10 -> 4 succeed, 6 are full.
  lob::SpscQueue<RoutedEvent> queue(4);
  std::size_t full = 0;
  std::size_t succeeded = 0;
  for (std::uint64_t i = 0; i < 10; ++i) {
    RoutedEvent routed;
    routed.stage_seq = i;
    if (try_enqueue(queue, routed, full)) {
      ++succeeded;
    }
  }
  CHECK_EQ(succeeded, static_cast<std::size_t>(4));
  CHECK_EQ(full, static_cast<std::size_t>(6));

  // After draining one slot, exactly one more push succeeds.
  RoutedEvent drained;
  CHECK_TRUE(queue.try_pop(drained));
  if (try_enqueue(queue, RoutedEvent{}, full)) {
    ++succeeded;
  }
  CHECK_EQ(succeeded, static_cast<std::size_t>(5));

  // End-to-end accounting invariant under real backpressure: every routed event
  // is enqueued exactly once (pushed == staged) and arrives exactly once
  // (pushed == consumed). full counts rejected attempts, so enqueue attempts
  // (pushed + full) always cover every routed event, strictly more under
  // backpressure.
  const auto events = generate_workload(5000, 42ULL, 8);
  const PartitionReport report = run_partition_lab(events, 8, 1);
  std::size_t total_pushed = 0;
  std::size_t total_full = 0;
  std::size_t total_routed = 0;
  for (std::size_t s = 0; s < report.shard_count; ++s) {
    total_pushed += report.pushed[s];
    total_full += report.full[s];
    total_routed += report.staged[s].size();
    CHECK_EQ(report.pushed[s], report.staged[s].size());
    CHECK_EQ(report.pushed[s], report.consumed[s].size());
    CHECK_TRUE(report.pushed[s] + report.full[s] >= report.staged[s].size());
  }
  CHECK_EQ(total_pushed, total_routed);
  CHECK_EQ(total_pushed, events.size());
  // full counts rejected enqueue attempts; pushed + full always covers every
  // routed event (strictly more under backpressure). This is a deterministic
  // accounting invariant, not a timing-dependent threshold.
  CHECK_TRUE(total_pushed + total_full >= total_routed);
}

TEST_CASE(partition_threaded_per_shard_equivalence) {
  // Across several shard counts and queue capacities (including capacity 1, the
  // heaviest backpressure), the threaded matcher per shard must equal a
  // deterministic single-threaded direct replay of that shard's staged events.
  // No timing threshold is used; the check is a structural digest equality.
  const std::vector<std::size_t> shard_counts{1, 4, 8, 16};
  const std::vector<std::size_t> capacities{1, 16, 1024};

  for (const std::size_t shards : shard_counts) {
    for (const std::size_t capacity : capacities) {
      const auto events = generate_workload(3000, 12321ULL + shards, shards);
      const PartitionReport report = run_partition_lab(events, shards, capacity);

      std::size_t total_consumed = 0;
      for (std::size_t s = 0; s < report.shard_count; ++s) {
        total_consumed += report.consumed[s].size();
        CHECK_TRUE(seqs_equal(staged_seqs(report.consumed[s]), staged_seqs(report.staged[s])));

        std::vector<lob::Event> staged_events;
        staged_events.reserve(report.staged[s].size());
        for (const RoutedEvent& r : report.staged[s]) {
          staged_events.push_back(r.event);
        }
        const StateDigest oracle = direct_replay(staged_events);
        CHECK_TRUE(same_digest(report.matcher_digest[s], oracle));
      }
      CHECK_EQ(total_consumed, events.size());
    }
  }
}
