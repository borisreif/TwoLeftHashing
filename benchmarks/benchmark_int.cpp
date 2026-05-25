#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <twoleft/two_left_hash_map.hpp>

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t ns_since(Clock::time_point start, Clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
    );
}

std::vector<int> make_keys(std::size_t n, std::uint32_t seed) {
    std::vector<int> keys(n);
    std::iota(keys.begin(), keys.end(), 0);
    std::mt19937 rng(seed);
    std::shuffle(keys.begin(), keys.end(), rng);
    return keys;
}

template <class Map>
std::uint64_t time_insert_twoleft(Map& map, const std::vector<int>& keys) {
    const auto start = Clock::now();
    for (int key : keys) {
        map.insert(key, key * 2);
    }
    const auto end = Clock::now();
    return ns_since(start, end);
}

std::uint64_t time_insert_unordered(
    std::unordered_map<int, int>& map,
    const std::vector<int>& keys
) {
    const auto start = Clock::now();
    for (int key : keys) {
        map.emplace(key, key * 2);
    }
    const auto end = Clock::now();
    return ns_since(start, end);
}

template <class Map>
std::uint64_t time_hit_find(Map& map, const std::vector<int>& keys, std::uint64_t& sink) {
    const auto start = Clock::now();
    for (int key : keys) {
        auto* p = map.find(key);
        if (p) {
            sink += static_cast<std::uint64_t>(*p);
        }
    }
    const auto end = Clock::now();
    return ns_since(start, end);
}

std::uint64_t time_hit_find_unordered(
    std::unordered_map<int, int>& map,
    const std::vector<int>& keys,
    std::uint64_t& sink
) {
    const auto start = Clock::now();
    for (int key : keys) {
        const auto it = map.find(key);
        if (it != map.end()) {
            sink += static_cast<std::uint64_t>(it->second);
        }
    }
    const auto end = Clock::now();
    return ns_since(start, end);
}

std::size_t initial_buckets_for(std::size_t n) {
    // main capacity = 2 * bucket_count * BucketSlots. With BucketSlots=4 and
    // max_load_factor=0.80, this avoids most resizing during the benchmark.
    const double wanted = static_cast<double>(n) / (2.0 * 4.0 * 0.80);
    return twoleft::ceil_power_of_two(static_cast<std::size_t>(wanted) + 1);
}

} // namespace

int main(int argc, char** argv) {
    const std::string output_path = argc >= 2 ? argv[1] : "benchmarks/results.csv";

    const std::vector<std::size_t> sizes = {
        1'000, 2'000, 4'000, 8'000, 16'000, 32'000, 64'000, 128'000
    };

    std::ofstream out(output_path);
    if (!out) {
        std::cerr << "Could not open output file: " << output_path << '\n';
        return 1;
    }

    out << "n,twoleft_insert_ns,unordered_insert_ns,twoleft_find_hit_ns,unordered_find_hit_ns,"
           "twoleft_stash_size,twoleft_bucket_count\n";

    std::uint64_t sink = 0;

    for (std::size_t n : sizes) {
        auto keys = make_keys(n, 12345U + static_cast<std::uint32_t>(n));
        auto lookup_keys = keys;
        std::mt19937 rng(98765U + static_cast<std::uint32_t>(n));
        std::shuffle(lookup_keys.begin(), lookup_keys.end(), rng);

        twoleft::TwoLeftHashMap<int, int> twoleft_map(initial_buckets_for(n));
        const auto twoleft_insert = time_insert_twoleft(twoleft_map, keys);
        const auto twoleft_find = time_hit_find(twoleft_map, lookup_keys, sink);

        std::unordered_map<int, int> unordered;
        unordered.reserve(n);
        const auto unordered_insert = time_insert_unordered(unordered, keys);
        const auto unordered_find = time_hit_find_unordered(unordered, lookup_keys, sink);

        out << n << ','
            << static_cast<double>(twoleft_insert) / static_cast<double>(n) << ','
            << static_cast<double>(unordered_insert) / static_cast<double>(n) << ','
            << static_cast<double>(twoleft_find) / static_cast<double>(n) << ','
            << static_cast<double>(unordered_find) / static_cast<double>(n) << ','
            << twoleft_map.stash_size() << ','
            << twoleft_map.bucket_count() << '\n';

        std::cout << "n=" << n
                  << " twoleft_insert_ns/op=" << static_cast<double>(twoleft_insert) / n
                  << " unordered_insert_ns/op=" << static_cast<double>(unordered_insert) / n
                  << " twoleft_find_ns/op=" << static_cast<double>(twoleft_find) / n
                  << " unordered_find_ns/op=" << static_cast<double>(unordered_find) / n
                  << " stash=" << twoleft_map.stash_size()
                  << '\n';
    }

    // Prevent the compiler from optimizing away lookup loops.
    std::cerr << "sink=" << sink << '\n';
    return 0;
}
