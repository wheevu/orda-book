#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
  printf '%s\n' 'perf profiling requires Linux.' >&2
  exit 2
fi

if ! command -v perf >/dev/null 2>&1; then
  printf '%s\n' 'perf is not installed or is not on PATH.' >&2
  exit 2
fi

backend=${1:-baseline}
workload=${2:-modify-heavy}
events=${3:-200000}
build_dir=${BUILD_DIR:-build-perf}

cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target orda_benchmark --parallel

printf 'backend=%s workload=%s events=%s build=%s\n' \
  "$backend" "$workload" "$events" "$build_dir"

perf stat -r 5 \
  -e cycles,instructions,branches,branch-misses,cache-references,cache-misses \
  "$build_dir/orda_benchmark" \
  --backend "$backend" \
  --events "$events" \
  --workload "$workload" \
  --rounds 1 \
  --seed 305419896 \
  --no-latency \
  --reuse-trades
