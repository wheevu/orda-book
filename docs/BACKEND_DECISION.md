# Backend decision

The project keeps three implementations because they answer different design
questions rather than because one benchmark number is universally best.

## Decision

| backend | decision | reason |
| --- | --- | --- |
| baseline | retain as default and correctness reference | general price domain, simplest control path, easiest oracle |
| pooled | retain as general performance candidate | roughly half the measured event-path allocations of baseline |
| ladder | retain as bounded-market candidate | lowest elapsed time at the largest tested depth |

The baseline remains the public default.

The pooled backend is the next general-purpose implementation to investigate
because it improves allocation behavior without imposing a fixed price range.

The ladder backend should only be selected when the instrument's price domain is
known and the configured bounds are part of the product contract.

## Evidence

The allocation campaign measured 0.800752 allocations per event for pooled,
compared with 1.60052 for baseline, on the 500,000-event replay.

The local sweep campaign measured the lowest elapsed time at depth 4,096 for
the ladder backend, while pooled had the lowest p50 at depth 64.

These measurements include timestamp overhead and are not portable production
performance claims.

## Follow-up gate

Before changing the default, repeat the comparison on Linux with CPU affinity,
fixed frequency policy, multiple rounds, and `perf` counters.

The follow-up must include reject-heavy and modify-heavy workloads, not only
the sweep workload.

The follow-up must also compare memory footprint and correctness campaign cost.
