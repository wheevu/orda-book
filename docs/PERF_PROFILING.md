# Linux profiling

The performance counters in this document are Linux-only measurements.

Do not compare them directly with the macOS timing tables.

## Environment record

Every run must record:

- CPU model and topology;
- kernel version;
- compiler and version;
- build flags;
- Git revision;
- CPU governor;
- process affinity;
- `perf` version;
- whether the counter run was permitted without virtualization or throttling.

## Counter campaign

The repository includes `scripts/profile_perf.sh`.

Example:

```sh
./scripts/profile_perf.sh baseline modify-heavy 200000
./scripts/profile_perf.sh pooled modify-heavy 200000
./scripts/profile_perf.sh ladder modify-heavy 200000
```

The script measures:

- cycles;
- instructions;
- instructions per cycle;
- branches;
- branch misses;
- cache references;
- cache misses.

Use the same event stream, seed, build, and machine for every backend.

## Call graph

Capture one representative call graph separately from the counter run:

```sh
perf record -g \
  build-perf/orda_benchmark \
  --backend baseline \
  --events 1000000 \
  --workload sweep \
  --rounds 1 \
  --seed 305419896 \
  --no-latency \
  --reuse-trades
perf report
```

The profile should answer which path consumes time before an optimization is
chosen.

## Workflow

The manual GitHub Actions workflow `perf.yml` provides a repeatable Linux
runner entry point.

Hosted-runner counters may be unavailable or noisy.

If `perf` refuses access, record the failure and do not replace it with guessed
numbers.

Local bare-metal measurements remain the preferred source for cache and branch
claims.
