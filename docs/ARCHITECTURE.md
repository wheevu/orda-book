# Architecture

orda-book is a single-threaded limit order book with an offline text replay
path and a benchmark harness.

## Scope

The engine accepts add, cancel, and modify events.

An add can match resting orders at crossed prices and leaves any remainder in
the book.

The current input path parses a complete file into memory before replay.

Networking, persistence, market orders, and multi-threaded matching are outside
the current scope.

## Data structure

The book stores bids and asks in separate ordered maps in
`src/order_book.hpp`.

Bid prices use descending order so the best bid is the first level.

Ask prices use ascending order so the best ask is the first level.

Each price level owns a `std::list<Order>`.

The list preserves arrival order and therefore implements FIFO time priority
within one price level.

The level also stores its aggregate quantity so top-of-book queries do not
need to scan every order.

`order_index_` maps an order ID to its side, price level, and list iterator.

This makes cancel lookup constant-time on average and lets cancellation erase
an order without searching its price level.

The map and list combination trades allocation and pointer chasing for clear
iterator stability and direct FIFO behavior.

## Event flow

1. `event_parser.cpp` converts text lines into typed `Event` values.
2. `OrderBook::process` dispatches the event to the matching operation.
3. An add validates its fields and order ID.
4. `match_incoming` walks the best opposing levels while the price crosses.
5. Trades execute against the oldest order at each level.
6. Any remaining quantity becomes a new resting order.
7. Replay and benchmark programs expose the resulting trades, statistics, and
   book state.

## Modify semantics

Modify is cancel-replace.

The replacement price and quantity are validated before the original order is
removed.

An invalid modify therefore leaves the original order unchanged.

A valid modify removes the old queue position and inserts the replacement at
the back of its new price level.

The behavior is covered by `tests/order_book_tests.cpp`.

## Complexity

The ordered-map portion of an add, cancel, or modify operation is logarithmic
in the number of price levels.

Order-ID lookup and list erasure are constant-time on average after the map
lookup.

Matching can visit multiple price levels and multiple resting orders, so its
cost is proportional to the number of orders consumed plus the number of
levels crossed.

## Verification boundary

The example tests cover individual matching scenarios.

The property tests in `tests/property_tests.cpp` generate deterministic event
sequences and check book ordering, positive quantities, live-order counts,
trade fields, and repeated-run determinism.

These tests increase confidence in the implemented behavior.

They do not establish a formal proof of matching correctness.
