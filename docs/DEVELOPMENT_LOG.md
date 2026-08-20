# Development log

This log records the measured path from a compact matching-engine exercise to a
more defensible quantitative-development study.

## Milestones

| milestone | result |
| --- | --- |
| M0 | established the C++17 single-threaded matching model and replay path |
| M1 | added deterministic benchmark workloads and latency percentiles |
| M2 | added generated invariants and differential reference histories |
| M3 | added release, AddressSanitizer, and ThreadSanitizer verification |
| M4 | added pooled order storage and bounded bitmap-ladder experiments |
| M5 | recorded throughput, allocation, and ingress boundary measurements |
| M6 | added optional differential fuzzing and Linux profiling procedures |
| R1 | added checked quantity overflow behavior and abuse coverage |
| R2 | added ingress capacity sweep evidence |
| R3 | added event-path allocation measurement and results |
| R4 | added `perf` profiling script and manual CI workflow |
| R5 | added controlled depth and workload tail-latency benchmark |
| R6 | documented the backend selection decision |

## Current interpretation

The matching engine is deterministic and has a differential reference path for
the main state transitions.

The pooled backend is the strongest general optimization candidate because it
reduces measured event-path allocation pressure.

The ladder backend is attractive for bounded instruments but is not a universal
replacement because it requires a configured price domain.

The ingress queue is a boundary experiment, not a networking implementation.

The current latency values are local observations with scheduler and timer
noise.

## Remaining work

Linux `perf` artifacts are still needed before making cache, branch, or cycle
claims.

The optional fuzzer needs to run in Linux CI because the local macOS toolchain
does not provide the required libFuzzer runtime archive.

The final portfolio pass should record the exact host, compiler, build flags,
workload parameters, and limitations beside every reported number.
