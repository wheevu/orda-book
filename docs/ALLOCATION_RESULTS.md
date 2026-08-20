# Allocation results

The dedicated allocation benchmark counts global `new` calls and requested
bytes only during the event-processing loop.

Setup allocations for the engine, order index, trade buffer, ladder levels, and
input parser are excluded.

This measurement uses a separate executable so normal throughput and latency
benchmarks do not pay for allocation counters.

## Provenance

| field | value |
| --- | --- |
| build | CMake Release |
| machine | MacBookAir10,1 |
| architecture | arm64 |
| compiler | Apple Clang 21.0.0 |
| events | 500,000 |
| rounds | 3 |
| input | `data/benchmark_events.txt` |
| trade buffer | reused |

## Results

| backend | allocations | allocations/event | bytes | bytes/event |
| --- | ---: | ---: | ---: | ---: |
| baseline | 800,520 | 1.60104 | 38,432,384 | 76.8648 |
| pooled | 400,376 | 0.800752 | 12,821,312 | 25.6426 |
| ladder | 800,288 | 1.60058 | 35,212,672 | 70.4253 |

All three rounds produced identical counters for each backend.

The pooled backend cuts measured event-path allocations by about half compared
with the baseline.

The ladder backend retains list-node allocation behavior and therefore remains
close to the baseline.

## Limits

The global allocation probe is process-local and counts calls that resolve to
the benchmark executable's allocation operators.

It does not identify allocation call sites.

It does not measure allocator lock contention, fragmentation, cache locality,
or page faults.

Use Linux `perf`, allocator tracing, or a heap profiler for those questions.

Run it with:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target orda_allocation_benchmark --parallel
./build-release/orda_allocation_benchmark \
  --backend pooled \
  --file data/benchmark_events.txt \
  --rounds 3
```
