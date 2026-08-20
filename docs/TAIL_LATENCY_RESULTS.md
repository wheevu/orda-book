# Tail latency results

`orda_tail_benchmark` runs controlled depth sweeps and records per-event
latency around `process` for each storage backend.

The sweep workload creates `depth` resting asks, then submits one aggressive bid
that consumes the level set.

The depth values below use 32, 8, and 4 cycles respectively so the runs remain
small while changing the active book depth.

## Provenance

| field | value |
| --- | --- |
| build | CMake Release |
| machine | MacBookAir10,1 |
| architecture | arm64 |
| compiler | Apple Clang 21.0.0 |
| rounds | 1 |
| clock | `std::chrono::steady_clock` |

## Sweep workload

| backend | depth | events | p50 (ns) | p99 (ns) | p999 (ns) | elapsed (s) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| baseline | 64 | 2,080 | 250 | 9,541 | 32,250 | 0.001347 |
| baseline | 1,024 | 8,200 | 334 | 750 | 6,541 | 0.005526 |
| baseline | 4,096 | 16,388 | 333 | 666 | 14,417 | 0.011981 |
| pooled | 64 | 2,080 | 209 | 8,209 | 93,083 | 0.001849 |
| pooled | 1,024 | 8,200 | 291 | 625 | 34,459 | 0.004452 |
| pooled | 4,096 | 16,388 | 250 | 375 | 2,917 | 0.014748 |
| ladder | 64 | 2,080 | 375 | 8,583 | 9,792 | 0.001241 |
| ladder | 1,024 | 8,200 | 250 | 458 | 101,666 | 0.003930 |
| ladder | 4,096 | 16,388 | 250 | 417 | 7,250 | 0.007217 |

The bounded ladder had the lowest elapsed time at the largest tested depth.

The pooled backend had the lowest p50 at depth 64 and the lowest p999 at depth
4,096 in this sample.

The depth trend is not monotonic because the run is short and timestamping adds
noise at this scale.

The benchmark is a measurement harness, not a calibrated exchange latency
claim.

## Reproduce

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target orda_tail_benchmark --parallel
./build-release/orda_tail_benchmark \
  --backend ladder \
  --workload sweep \
  --depth 4096 \
  --cycles 4 \
  --rounds 5
```

The executable also supports `cancel` and `modify` workloads.

Use Linux `perf` and CPU pinning before turning these values into a portfolio
performance claim.
