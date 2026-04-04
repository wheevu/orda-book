# orda-book

Small C++ limit order book project for learning.

## Features
- matching
- replay
- benchmark
- tests

## Use it
```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build

./build/orda_replay data/sample_events.txt
./build/orda_benchmark --file data/benchmark_events.txt
```
