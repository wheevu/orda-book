# Ingress boundary design

M6 now includes a bounded `lob::SpscQueue<T>` for the boundary between event
ingress and the single-threaded matching engine.

The book itself should remain single-threaded in this experiment.

## Proposed flow

```text
producer / feed handler -> bounded SPSC queue -> matching engine -> trade output
```

The producer owns enqueue operations.

The matching thread owns dequeue operations and all book mutation.

The queue carries typed `Event` values rather than pointers to mutable objects.

The queue implementation is generic so the first tests can use scalar values
without coupling queue correctness to matching behavior.

## Measurements

The boundary campaign should report separately:

- enqueue-to-dequeue delay;
- queue-full behavior;
- event-to-match delay;
- matching time;
- trade-output handling time;
- end-to-end producer-to-trade latency.

Engine-only results must remain comparable with the M2 and M3 benchmark modes.

## Required contract

The queue needs explicit behavior when full.

The first version should reject or count dropped events rather than silently
overwrite an event.

The producer must be able to observe queue-full status.

The consumer must not process an event twice.

The engine must remain correct if the queue drains from one event to empty.

## Verification plan

Before adding the boundary to the benchmark:

1. Test zero-capacity and one-capacity queues.
2. Test ordered transfer of a finite sequence.
3. Test full-queue rejection without mutation.
4. Run producer and consumer threads under ThreadSanitizer where available.
5. Compare consumed events and trade output with direct engine replay.
6. Measure queue and engine paths separately.

The current tests cover the first five items for a finite producer and consumer
run.

`orda_ingress_benchmark` now measures the threaded queue boundary using parsed
event files.

Run it with:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target orda_ingress_benchmark --parallel
./build-release/orda_ingress_benchmark \
  --file data/benchmark_events.txt \
  --capacity 1024 \
  --rounds 3
```

The benchmark reports total producer-to-consumer run time, queue-full retries,
and queue-delay p50, p99, and p999.

The queue-delay timestamp is taken before enqueue and after dequeue.

It includes scheduling and queue contention.

It excludes network ingress and trade-response delivery.

## First local run

On the current Release build, `data/benchmark_events.txt` contained 500,000
events.

With capacity 1,024 and three rounds, total run times were 0.157 s, 0.161 s,
and 0.209 s.

The queue-delay p99 values were 717,208 ns, 677,958 ns, and 841,792 ns.

The queue-full retry counts were 345,328, 210,264, and 125,262.

This is a first boundary observation, not a stable latency baseline.

The producer and consumer were unscheduled macOS threads, so scheduler activity
dominates the tail more than the queue's atomic operations alone.

The concurrency implementation should be reviewed independently before it is
used for performance claims.

## Scope limit

This is not a networking layer.

Binary wire formats, socket behavior, reconnects, backpressure across a feed
handler, and Norn integration remain separate decisions.
