# TwoLeftHashMap

A small C++20 experimental 2-left hash map implementation.

## Architecture

```text
TwoLeftHashMap
    dynamic public wrapper
    owns resizing / rebuilding / stash-pressure policy

detail::FixedTwoLeftTable
    fixed-capacity table image
    owns one contiguous logical layout:
        | table 1 | table 2 | stash |
    never resizes itself
```

The code is intentionally split into headers and `.tpp` files because the hash
map is template-heavy. The template definitions must be visible to translation
units that instantiate the map.

## File layout

```text
include/twoleft/
    hash_utils.hpp
    table_config.hpp
    two_left_hash_map.hpp
    two_left_hash_map.tpp
    detail/
        fixed_two_left_table.hpp
        fixed_two_left_table.tpp

examples/
    demo.cpp

benchmarks/
    benchmark_int.cpp
    plot_results.gnuplot
```

## Build with CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/twoleft_demo
```

## Benchmark and plot

Run from the project root:

```bash
./build/twoleft_benchmark_int benchmarks/results.csv
gnuplot benchmarks/plot_results.gnuplot
```

This creates:

```text
benchmarks/results.csv
benchmarks/results.png
```

The benchmark compares insertion and successful lookup against
`std::unordered_map<int, int>`. Treat the numbers as exploratory, not as a final
performance claim. For serious benchmarking, use multiple repetitions, pin the
CPU, disable turbo/frequency scaling where possible, and consider Google
Benchmark or nanobench.
