# orda-book

orda-book is a small single-threaded limit order book in C++17.

It matches limit orders using price-time priority, supports offline replay, and
measures both throughput and per-event latency on deterministic workloads.

## Quickstart

Requirements: CMake 3.20 or newer and a C++17 compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Replay a sample event file:

```sh
./build/orda_replay data/sample_events.txt
./build/orda_replay data/sample_events.txt --top
```

Run a benchmark:

```sh
./build/orda_benchmark --file data/benchmark_events.txt --rounds 5
./build/orda_benchmark \
  --events 200000 \
  --workload cross-heavy \
  --rounds 5 \
  --seed 305419896
```

For a timer-free throughput comparison, reuse the trade buffer and disable
per-event latency collection:

```sh
./build/orda_benchmark \
  --events 200000 \
  --workload cross-heavy \
  --rounds 5 \
  --seed 305419896 \
  --no-latency \
  --reuse-trades
```

## What it contains

- Ordered bid and ask price levels.
- FIFO queues within each price level.
- Order-ID lookup for cancellation and modification.
- Add, cancel, and cancel-replace modification events.
- Multi-level matching and trade output.
- Text and CSV-style event parsing.
- Six deterministic benchmark workload generators.
- Example tests, generated invariant histories, and differential reference
  histories.
- Aggregate throughput and p50, p99, and p999 event latency.

## Matching model

The best bid is the highest bid price.

The best ask is the lowest ask price.

An incoming order crosses while its limit price is compatible with the best
opposing price.

Orders at the same price execute in arrival order.

Modify is cancel-replace.

An invalid modification leaves the original order unchanged.

A valid modification loses its previous queue position.

## Benchmarks

The generated harness includes add-only, cross-heavy, partial-fill, sweep,
cancel-heavy, and modify-heavy workloads.

Each round reports elapsed time, throughput, aggregate nanoseconds per event,
and event latency percentiles.

Latency is measured around `OrderBook::process`.

It includes timestamp overhead and engine-side trade recording, but excludes
networking and response delivery.

Numbers are local comparisons, not production exchange performance claims.

Use `./build/orda_benchmark --calibrate-clock` to measure the local timestamp
floor.

## Verification

The suite covers matching examples, FIFO behavior, multi-level sweeps,
cancellation, modification, parser behavior, invalid inputs, generated book
invariants, differential reference histories, and deterministic replay.

The generated histories check the book after every event for ordering,
positive quantities, non-crossing state, live-order counts, and valid trades.

## Documentation

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md): data structures, event flow,
  complexity, and system boundaries.
- [`docs/CORRECTNESS_CAMPAIGN.md`](docs/CORRECTNESS_CAMPAIGN.md): generated
  histories, invariants, oracle boundaries, and commands.
- [`docs/BENCHMARKING.md`](docs/BENCHMARKING.md): benchmark measurements,
  percentile definitions, run rules, and limitations.
- [`docs/BENCHMARK_RESULTS.md`](docs/BENCHMARK_RESULTS.md): the first measured
  local baseline and its environment metadata.

## Limitations

orda-book currently has no networking layer, persistence, market orders, IOC or
FOK order types, or concurrent matching support.

The parser loads the complete input file into memory.

It is an educational matching-engine project, not a production exchange.
