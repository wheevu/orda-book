# orda-book

Lil' limit order book thing. Does what you'd expect 🤷🏻‍♂️

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
