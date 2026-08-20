# Benchmark results

These results are the first local M2 baseline for the node-based matching
engine.

They are a comparison point for later storage and price-level experiments.

They are not production exchange performance claims.

## Provenance

| field | value |
| --- | --- |
| Git revision | `f04edc2` |
| working tree | dirty during measurement because M1 and M2 changes were not committed |
| machine | MacBookAir10,1 |
| CPU architecture | arm64 |
| logical CPUs | 8 |
| memory | 16 GiB |
| operating system | macOS Darwin 25.5.0 |
| compiler | Apple Clang 21.0.0 |
| build | CMake Release |
| generated events | 200,000 timed events |
| rounds | 5 |
| seed | `305419896` |
| trade buffer | reused |

The summary row is the round selected by median elapsed duration.

The latency values are the percentiles from that same selected round.

## Timer calibration

The local p50 cost of two back-to-back `steady_clock::now()` calls was 41 ns.

The p99 cost was 42 ns and the p999 cost was 42 ns.

The calibration is reported as a measurement floor.

It was not subtracted from event measurements.

## Results

| workload | throughput, no latency | ns/event, no latency | p50 | p99 | p999 |
| --- | ---: | ---: | ---: | ---: | ---: |
| add-only | 19.10M ev/s | 52.37 ns | 42 ns | 84 ns | 208 ns |
| cross-heavy | 25.99M ev/s | 38.47 ns | 42 ns | 125 ns | 833 ns |
| partial-fill | 95.98M ev/s | 10.42 ns | 41 ns | 83 ns | 166 ns |
| sweep | 13.05M ev/s | 76.65 ns | 42 ns | 375 ns | 458 ns |
| cancel-heavy | 14.87M ev/s | 67.27 ns | 83 ns | 167 ns | 917 ns |
| modify-heavy | 9.54M ev/s | 104.84 ns | 125 ns | 500 ns | 2,958 ns |

The throughput columns use `--no-latency --reuse-trades`.

The percentile columns use `--reuse-trades` with latency collection enabled.

The two modes have different measurement overhead and should not be treated as
one combined run.

## Commands

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel

./build-release/orda_benchmark --calibrate-clock

./build-release/orda_benchmark \
  --events 200000 \
  --workload cross-heavy \
  --rounds 5 \
  --seed 305419896 \
  --reuse-trades

./build-release/orda_benchmark \
  --events 200000 \
  --workload cross-heavy \
  --rounds 5 \
  --seed 305419896 \
  --no-latency \
  --reuse-trades
```

## Interpretation

The timer-free run is faster than the latency run because it avoids two clock
reads and latency sample storage for every event.

The modify-heavy workload has the highest median event cost among these runs and
the largest p999 tail.

The sweep, cancel-heavy, and modify-heavy tails are the first workloads to use
when evaluating allocation and pointer-locality changes.

The results do not identify the cause of the tail.

Linux `perf` counters and allocation instrumentation are deferred to the next
measurement pass.
