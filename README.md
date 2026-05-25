# TwoLeftHashMap

A small C++20 experimental implementation of a **2-left hash map**.

This project is meant for learning and experimentation. It is not intended to be
a production replacement for `std::unordered_map`.

## Introduction

2-left hashing uses two candidate buckets for each key. A key is hashed with two
independent-looking hash functions:

```text
key
 ├── h1(key) -> bucket in table 1
 └── h2(key) -> bucket in table 2
```

Insertion chooses the candidate bucket with the smaller current load. If both
candidate buckets are full, the entry is placed in a small fallback area called
the **stash**.

The fixed table layout is:

```text
| table 1 buckets | table 2 buckets | stash |
```

Each bucket contains a fixed number of slots:

```text
bucket with BucketSlots = 4:

+--------+--------+--------+--------+
| slot 0 | slot 1 | slot 2 | slot 3 |
+--------+--------+--------+--------+
```

Only the occupied prefix of each bucket is live. This means the implementation
can erase without tombstones by moving the last occupied slot into the removed
position.

## Architecture

The code separates **fixed placement mechanics** from **dynamic growth policy**:

```text
TwoLeftHashMap
    dynamic public wrapper
    owns resizing / rebuilding / stash-pressure policy

namespace twoleft::detail
    FixedTwoLeftTable
        fixed-capacity table image
        owns one contiguous logical layout:
            | table 1 | table 2 | stash |
        never resizes itself
```

The wrapper can rebuild the table with new hash seeds or grow to a larger bucket
count. The fixed table only reports whether an insert succeeded, used the stash,
found an existing key, or failed because the stash was full.

For a more detailed explanation with diagrams, see:

```text
docs/architecture.md
```

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

docs/
    architecture.md

examples/
    demo.cpp

benchmarks/
    benchmark_int.cpp
    plot_results.gnuplot

tests/
    basic_tests.cpp
```

The code is split into headers and `.tpp` files because the hash map is
template-heavy. Template definitions must be visible to translation units that
instantiate the map.

## Build with CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/twoleft_demo
```

## Run tests

```bash
./build/twoleft_basic_tests
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

## VS Code on Ubuntu

Recommended packages:

```bash
sudo apt update
sudo apt install build-essential cmake gdb gnuplot
```

Recommended VS Code extensions are listed in `.vscode/extensions.json`:

- C/C++ (`ms-vscode.cpptools`)
- CMake Tools (`ms-vscode.cmake-tools`)
- CMake syntax highlighting (`twxs.cmake`)

Useful tasks:

```text
Terminal -> Run Task... -> CMake: build debug
Terminal -> Run Task... -> Run demo
Terminal -> Run Task... -> Run tests
Terminal -> Run Task... -> Run benchmark release
Terminal -> Run Task... -> Plot benchmark with gnuplot
```

Debugger launch configurations:

```text
Run and Debug -> Debug demo
Run and Debug -> Debug tests
Run and Debug -> Debug benchmark
```

## Documentation comments

The public and internal headers contain Doxygen-style comments. Most comments are
placed on declarations in `.hpp` files so that generated documentation focuses on
the interface and architecture, while `.tpp` files contain the template bodies and
additional implementation notes.
