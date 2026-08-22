# Order storage design

This note defines the M3 storage experiment.

The pooled backend is now implemented as `lob::PooledOrderBook`.

## Baseline

The current engine stores each order in a `std::list<Order>` owned by its price
level.

`order_index_` stores a list iterator and a pointer to the owning level.

The design is clear and supports direct FIFO operations.

It also creates separately allocated order nodes and pointer-heavy cancellation
lookups.

The M2 results provide the baseline for any storage change.

## Pooled experiment

The pooled backend stores orders in stable integer slots.

Each slot contains:

```cpp
struct OrderSlot {
  OrderId order_id;
  Side side;
  Price price;
  Quantity qty;
  SlotIndex next;
  SlotIndex prev;
};
```

Each price level stores a head slot, a tail slot, aggregate quantity, and order
count.

The free list recycles slots after cancellation or a complete fill.

The order index maps an order ID to a slot index rather than a list iterator.

## Capacity contract

The pooled implementation uses explicit capacity.

`reserve_orders` allocates the slot array and free list before event processing.

The backend does not silently allocate new order slots in the timed path.

A slot becomes available in two ways: it is free when the event is processed
(`free_head_ != kInvalidSlot`), or matching frees a slot from a fully consumed
opposing order.

Capacity rejection therefore applies only to a resting add that cannot obtain a
slot by either route:

- the incoming order would leave resting quantity (`resting_qty > 0`);
- no slot is free now (`free_head_ == kInvalidSlot`); and
- the incoming order crosses no opposing quantity (`crossing_qty == 0`).

Every other add proceeds. In particular, an add that crosses opposing quantity is
accepted even at full capacity: matching fully consumes each crossed order before
the incoming is exhausted, which frees at least one slot, so any residual resting
quantity reuses it. A non-crossing resting add at full capacity has no such route
and is rejected with `BookError::CapacityExceeded`.

The rejection path mutates no book state and records one rejected request.
Modification always removes the existing order before re-adding, so it has the
freed slot available and never rejects on capacity.

Price-level map nodes and order-index bucket growth remain separate concerns.

The first experiment measures order-slot behavior, not complete allocation
freedom.

Price-level map nodes can still allocate when a new price appears.

The order-index table can still allocate if the caller does not reserve enough
capacity.

## Required equivalence

The pooled backend must match the baseline engine for the same event history.

The differential campaign will compare errors, trades, order snapshots, live
orders, and statistics after every event.

The reference engine remains the independent correctness oracle.

The fixed campaign currently runs 64 pooled histories in addition to the 256
baseline histories.

## Measurement plan

Compare baseline and pooled backends with identical:

- event history;
- seed;
- build type;
- capacity;
- trade-buffer mode;
- latency mode;
- compiler and machine.

Report throughput, p50, p99, p999, and any measured allocation counters.

Run concentrated and sparse price-level workloads separately.

Reject an optimization if it improves the average while producing a materially
worse tail without a documented workload-specific reason.

## First comparison

The first comparison used the Release build on the same MacBook baseline as
`docs/BENCHMARK_RESULTS.md`.

It used 200,000 events, three rounds, seed `305419896`, `--no-latency`, and
`--reuse-trades`.

| workload | baseline | pooled | first reading |
| --- | ---: | ---: | --- |
| add-only | 17.14M ev/s | 16.51M ev/s | pooled slower |
| cross-heavy | 22.80M ev/s | 28.16M ev/s | pooled faster |
| partial-fill | 90.97M ev/s | 59.00M ev/s | pooled slower |
| sweep | 12.14M ev/s | 12.51M ev/s | close |
| cancel-heavy | 13.47M ev/s | 9.30M ev/s | pooled slower |
| modify-heavy | 5.63M ev/s | 4.67M ev/s | pooled slower |

These numbers are directional only.

The runs were short, local, and not isolated from host scheduling.

The pooled backend is not a general improvement yet.

The result supports keeping the two implementations as separate experiments
and measuring latency tails before changing the design further.

## Open implementation questions

- Capacity exhaustion is represented by `BookError::CapacityExceeded`.
  The pooled backend rejects only a non-crossing resting add at full capacity; a
  crossing add that frees a slot during matching is accepted. See the capacity
  contract above.
- Should price levels remain `std::map` in the first pooled comparison?
- How should the benchmark count allocations without changing the hot path?
- The pooled backend exposes the same diagnostic order snapshot API.
