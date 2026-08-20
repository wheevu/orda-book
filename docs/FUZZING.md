# Fuzzing

The optional `orda_fuzz` target differentially exercises the production engine
and the independent reference engine.

## Build

The target requires a Clang toolchain with libFuzzer support.

```sh
cmake -S . -B build-fuzz -DLOB_BUILD_FUZZER=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=fuzzer,address,undefined' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=fuzzer,address,undefined'
cmake --build build-fuzz --target orda_fuzz --parallel
```

## Run

Run a bounded smoke campaign locally:

```sh
./build-fuzz/orda_fuzz -runs=10000
```

The fuzzer maps arbitrary bytes into add, cancel, and modify histories.

It compares each event's error, trades, order-level book state, live-order
count, and cumulative statistics.

The target uses bounded positive prices and quantities for its first campaign,
while dedicated abuse tests cover the quantity-overflow contract.

## Failure handling

The fuzzer traps on the first differential mismatch.

LibFuzzer prints a reproducing input and saves a minimized artifact when run
with a corpus directory.

Preserve a minimized input as a focused regression test when it exposes a real
engine defect.

The fuzz target is opt-in and is not part of the normal CTest suite.
