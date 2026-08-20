# Price ladder design

`lob::LadderOrderBook` is the first price-level representation experiment.

It is compared with the ordered-map baseline and the pooled order-slot backend.

## Domain assumptions

The ladder uses integer prices in a fixed inclusive range.

The default range is 1 through 200,000.

An order outside that range is rejected with `BookError::PriceOutOfRange`.

This backend is therefore not a drop-in replacement for an unbounded price
domain.

## Representation

The ladder preallocates one `PriceLevel` object per price in the range.

Each level keeps the same FIFO `std::list` order storage as the baseline.

An order-ID index preserves constant-time average cancellation lookup.

Two occupied-level bitmaps track bids and asks independently.

A second bitmap tracks which bitmap words are non-empty.

This two-level index avoids scanning the complete price range to find the best
occupied price.

The ladder changes price-level lookup and best-price discovery.

It does not remove list-node allocation or change order storage.

That separation keeps the first comparison interpretable.

## Comparability

Use the same event history, seed, trade-buffer mode, latency mode, and Release
configuration for every backend.

The default generated workloads stay inside the ladder's configured range.

Sparse and concentrated price distributions must be measured separately.

A dense ladder can waste memory and initialization time when the active price
range is sparse.

An ordered map can pay more lookup cost while avoiding that fixed memory cost.

## Verification

The ladder backend participates in the differential campaign with the same
order-level snapshot and trade oracle as the baseline and pooled backends.

The campaign currently runs 32 fixed ladder histories.

The ladder's out-of-range rejection behavior has a focused test boundary.

## Limits

The current implementation uses compiler bit-count builtins for its bitmap
search.

The range is configured at construction and cannot grow during processing.

The backend is an experiment, not a claim that dense price ladders are always
faster.

## First comparison

The first three-backend comparison used a Release build on the same local
machine as the M2 baseline.

It used 200,000 events, three rounds, seed `305419896`, `--no-latency`, and
`--reuse-trades`.

| workload | baseline | pooled | ladder |
| --- | ---: | ---: | ---: |
| add-only | 17.39M ev/s | 15.57M ev/s | 21.10M ev/s |
| cross-heavy | 23.73M ev/s | 25.94M ev/s | 14.21M ev/s |
| partial-fill | 97.19M ev/s | 92.91M ev/s | 36.29M ev/s |
| sweep | 13.24M ev/s | 10.76M ev/s | 9.72M ev/s |
| cancel-heavy | 14.96M ev/s | 9.33M ev/s | 12.20M ev/s |
| modify-heavy | 8.75M ev/s | 7.47M ev/s | 7.95M ev/s |

The ladder wins the add-only workload but loses badly on matching-heavy
workloads in this first run.

That result is consistent with the ladder changing level discovery while still
using list-node order storage.

The results are directional and local.

They do not isolate initialization, memory footprint, allocation count, or
latency tails.
