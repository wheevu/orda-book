# Benchmark results

These are current local Release measurements for the three matching-engine
backends.

They compare deterministic workloads under the same event count, seed, and
round selection rules.

They are not production exchange performance claims.

## Provenance

| field | value |
| --- | --- |
| Git revision | `3c79930` |
| working tree | source tree clean; unrelated untracked `.vscode/` preserved |
| machine | MacBookAir10,1 |
| CPU architecture | arm64 |
| logical CPUs | 8 |
| memory | 16 GiB |
| operating system | macOS Darwin 25.5.0 |
| compiler | Apple Clang 21.0.0 |
| build | CMake Release |
| generated events | 200,000 timed events |
| rounds | 3 |
| seed | `305419896` |
| trade buffer | reused |

The summary row is the round selected by median elapsed duration.

## Timer-free throughput

| backend | workload | throughput | ns/event |
| --- | --- | ---: | ---: |
| baseline | add-only | 5.47M ev/s | 183 ns |
| baseline | cross-heavy | 10.2M ev/s | 98.2 ns |
| baseline | partial-fill | 41.3M ev/s | 24.2 ns |
| baseline | sweep | 7.02M ev/s | 142 ns |
| baseline | cancel-heavy | 7.53M ev/s | 133 ns |
| baseline | modify-heavy | 4.50M ev/s | 222 ns |
| pooled | add-only | 11.4M ev/s | 88.0 ns |
| pooled | cross-heavy | 18.3M ev/s | 54.7 ns |
| pooled | partial-fill | 48.9M ev/s | 20.5 ns |
| pooled | sweep | 9.01M ev/s | 111 ns |
| pooled | cancel-heavy | 7.61M ev/s | 131 ns |
| pooled | modify-heavy | 5.00M ev/s | 200 ns |
| ladder | add-only | 9.55M ev/s | 105 ns |
| ladder | cross-heavy | 8.12M ev/s | 123 ns |
| ladder | partial-fill | 14.7M ev/s | 67.8 ns |
| ladder | sweep | 4.48M ev/s | 223 ns |
| ladder | cancel-heavy | 6.49M ev/s | 154 ns |
| ladder | modify-heavy | 4.19M ev/s | 239 ns |

The throughput columns use `--no-latency --reuse-trades`.

## Latency comparison

| backend | workload | p50 | p99 | p999 |
| --- | --- | ---: | ---: | ---: |
| baseline | cross-heavy | 83 ns | 125 ns | 208 ns |
| baseline | modify-heavy | 250 ns | 375 ns | 541 ns |
| pooled | cross-heavy | 42 ns | 84 ns | 208 ns |
| pooled | modify-heavy | 208 ns | 292 ns | 708 ns |
| ladder | cross-heavy | 125 ns | 125 ns | 209 ns |
| ladder | modify-heavy | 208 ns | 292 ns | 375 ns |

The percentile columns use `--reuse-trades` with latency collection enabled.

The two modes have different measurement overhead and should not be treated as
one combined run.

The local p50 cost of two back-to-back `steady_clock::now()` calls was measured
separately at 41 ns in the earlier calibration run.

It was not subtracted from event measurements.

## Commands

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel

./build-release/orda_benchmark \
  --backend pooled \
  --events 200000 \
  --workload cross-heavy \
  --rounds 3 \
  --seed 305419896 \
  --reuse-trades

./build-release/orda_benchmark \
  --backend pooled \
  --events 200000 \
  --workload cross-heavy \
  --rounds 3 \
  --seed 305419896 \
  --no-latency \
  --reuse-trades
```

## Interpretation

The pooled backend leads this local cross-heavy throughput comparison and has
the lowest measured allocation rate.

The ladder backend is bounded by its configured price domain and does not lead
every workload.

The modify-heavy workload remains a useful stress case for order lookup and
cancel-replace behavior.

The measurements include timestamp cost where latency is enabled, trade
recording, and the engine's own data-structure work.

They exclude networking, scheduling, serialization, and response delivery.

Linux `perf`, CPU affinity, and longer repeated runs are needed before making
cache, branch, or absolute latency claims.

Full allocation, ingress, tail-latency, and profiling evidence is indexed from
the README documentation section.
