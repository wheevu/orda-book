# Correctness campaign

## Purpose

This campaign checks the invariants that should hold after every processed
event.

It complements the example tests with deterministic generated histories.

The campaign demonstrates behavioral properties.

It does not prove formal correctness or cover a live network boundary.

## Invariants

After every generated event:

- bid levels remain strictly descending;
- ask levels remain strictly ascending;
- all visible prices and quantities are positive;
- the best bid is below the best ask when both sides exist;
- every visible price level contains at least one order;
- the sum of level order counts equals `live_order_count()`;
- every emitted trade has a positive price and quantity.

The engine also checks that invalid modify requests do not remove the existing
order.

## Generated histories

`tests/property_tests.cpp` uses a local XorShift64 generator with fixed seeds.

The generated operations include adds, cancellations, valid modifications,
invalid modifications, and duplicate order IDs.

The tests use 20 seeds with 500 events for invariant coverage.

A separate 1,000-event history is replayed twice and compares errors, trades,
book formatting, and engine statistics.

The fixed seeds make failures reproducible without relying on scheduler timing
or external randomness.

## Test oracle

The current campaign checks externally observable book invariants rather than
reimplementing the matching algorithm in the test suite.

This keeps the oracle independent of the production containers.

The existing scenario tests remain responsible for detailed FIFO and
multi-level trade expectations.

Future order-type tests should extend the campaign with lifecycle accounting
for traded, resting, and cancelled quantity.

The simple equation `submitted = resting + traded + rejected` is not a valid
oracle once cancellation and modification exist.

## Commands

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The test executable currently reports the individual example and property
cases before printing its final result.

## Limits

The generated histories do not model every possible event distribution.

They do not test integer overflow, allocator failure, process crashes, or
concurrent access.

Those boundaries require separate tests and should not be implied by this
campaign.
