# orda-book change guide

## Scope

orda-book is a single-threaded limit-order matching engine and evidence lab.

The baseline, pooled, and ladder implementations must remain behaviorally equivalent within their documented price and capacity contracts.

The baseline is the correctness control path.

The pooled backend is the current general-purpose successor candidate.

The ladder backend is a bounded price-domain experiment.

## Invariants

- Price-time priority is FIFO within each price level.
- A valid modify is cancel-replace and loses queue position.
- Invalid input leaves the book and emitted trades unchanged.
- A market or IOC order never leaves a resting remainder.
- A FOK order either fills completely or leaves no state change.
- A post-only order rejects when it would trade immediately.
- The pooled backend rejects only a non-crossing resting add when its reserved capacity is full.
- The ladder backend rejects prices outside its inclusive configured range.
- Differential tests compare errors, trades, order snapshots, live count, and cumulative statistics.

## Commands

```sh
./scripts/verify.sh
./scripts/full_confidence.sh
```

The focused build loop is `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`, `cmake --build build --parallel`, and `ctest --test-dir build --output-on-failure`.

Use Release builds for measurements.

Use the optional `LOB_BUILD_FUZZER=ON` configuration for differential fuzzing.

## Evidence rules

Every published number must identify the commit, dirty-tree state, compiler, architecture, build type, workload, seed, and command line.

Keep raw events and result files committed or content-addressed when they support a headline claim.

Do not turn local comparison points into production, real-time, deterministic, or portability claims.

Do not edit historical result tables to make a new run appear current.

## Evidence conventions

Read `README.md`, `GUIDE.md`, and the relevant design note before changing behavior.
Add a focused regression test and run `./scripts/verify.sh`.
Validate result manifests with `./scripts/check_manifest.sh`.
Keep performance claims tied to committed measurements and their provenance.
