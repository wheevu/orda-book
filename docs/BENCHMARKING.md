# Benchmark method

The benchmark compares deterministic workloads against the single-threaded
matching engine.

It is a local engine measurement, not an exchange-grade network latency
measurement.

## Workloads

The generated harness currently provides six workload shapes:

- add-only;
- cross-heavy;
- partial-fill;
- sweep;
- cancel-heavy;
- modify-heavy.

Each generated workload records its seed and separates setup events from timed
events.

File mode can measure parsing together with engine processing or run the engine
against events parsed before the timed rounds.

## Measurements

Each round reports:

- elapsed seconds;
- events per second;
- aggregate nanoseconds per event;
- p50 event processing latency;
- p99 event processing latency;
- p999 event processing latency;
- trades, traded quantity, rejected requests, and live orders.

The benchmark header records whether latency collection is enabled and whether
the trade buffer is accumulated or reused.

Per-event latency starts immediately before `OrderBook::process` and ends
immediately after it returns.

The measurement includes the cost of the timestamp calls and any vector growth
or trade recording performed by the engine.

It does not include network, scheduling, serialization, or response delivery.

The percentile values use nearest-rank indexing over the sorted event latency
samples for that round.

When file mode includes parsing in elapsed time, the percentile samples still
measure only the matching-engine call, while the throughput number includes the
parse step.

The summary currently reports the round selected by median elapsed duration.

## Measurement modes

Latency collection is enabled by default.

Use `--no-latency` for a timer-free throughput run.

Use `--reuse-trades` to clear the trade vector before each event while retaining
its allocated capacity.

The default accumulated mode keeps every emitted trade until the round ends.

These modes measure different workloads and should not be compared as though
they were the same engine path.

Use `--calibrate-clock` to measure the local cost of two back-to-back
`steady_clock::now()` calls.

The calibration is a measurement floor, not a correction that should be
subtracted from each event.

## Run rules

1. Use a Release build for published performance comparisons.
2. Record the compiler, operating system, architecture, build flags, Git
   revision, workload, seed, event count, and round count.
3. Use the same workload and seed when comparing changes.
4. Run enough events for the p999 sample to be meaningful.
5. Avoid publishing results from a busy or thermally constrained machine.
6. Treat the result as a comparison between builds on one environment, not as
   an absolute claim about production trading systems.

Example:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
./build-release/orda_benchmark \
  --events 200000 \
  --workload cross-heavy \
  --rounds 5 \
  --seed 305419896
```

Timer-free throughput comparison:

```sh
./build-release/orda_benchmark \
  --events 200000 \
  --workload cross-heavy \
  --rounds 5 \
  --seed 305419896 \
  --no-latency \
  --reuse-trades
```

Clock calibration:

```sh
./build-release/orda_benchmark --calibrate-clock
```

## Interpretation

Throughput and percentile latency answer different questions.

A build can process many events per second while still producing an undesirable
tail during sweeps, allocation-heavy paths, or large cancellation workloads.

The benchmark does not isolate allocator, clock, cache, operating-system, or
branch-prediction effects.

Those effects remain part of the evidence and must be recorded rather than
hidden behind a single number.
