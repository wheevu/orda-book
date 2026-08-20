# Ingress capacity results

This is a small local capacity sweep for the bounded SPSC ingress benchmark.

The producer replays `data/benchmark_events.txt` while the consumer owns the
matching engine.

The queue-full counter records failed enqueue attempts, not dropped events.

## Provenance

| field | value |
| --- | --- |
| build | CMake Release |
| machine | MacBookAir10,1 |
| architecture | arm64 |
| compiler | Apple Clang 21.0.0 |
| events | 500,000 |
| rounds | 1 |
| queue | bounded SPSC |

## Results

| capacity | total seconds | queue-full retries | queue p50 (ns) | queue p99 (ns) | queue p999 (ns) |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 256 | 0.635765 | 39,604 | 119,625 | 1,178,959 | 7,428,083 |
| 1,024 | 0.474216 | 15,688 | 526,833 | 3,975,000 | 12,364,541 |
| 4,096 | 0.328813 | 11,297 | 2,091,417 | 3,858,791 | 4,865,375 |

The larger queues reduced producer retry pressure and total replay time in this
run.

They did not reduce queue-delay percentiles because the producer and consumer
were unscheduled macOS threads and the larger queue allowed more backlog.

The reported elapsed time stops when the consumer finishes processing the final
event and excludes the post-run state digest used for validation.

These values are directional evidence for the boundary, not production latency
claims.

## Reproduce

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target orda_ingress_benchmark --parallel
./build-release/orda_ingress_benchmark \
  --file data/benchmark_events.txt \
  --capacity 1024 \
  --rounds 3
```

Linux scheduling and CPU affinity should be controlled before comparing this
boundary with an exchange-style latency target.
