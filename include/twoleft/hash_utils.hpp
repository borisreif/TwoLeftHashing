#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <random>

namespace twoleft {

inline std::size_t ceil_power_of_two(std::size_t n) {
    return n <= 1 ? 1 : std::bit_ceil(n);
}

inline std::uint64_t make_seed() {
    static std::random_device rd;
    const auto hi = static_cast<std::uint64_t>(rd());
    const auto lo = static_cast<std::uint64_t>(rd());
    return (hi << 32) ^ lo;
}

inline std::uint64_t mix64(std::uint64_t x) noexcept {
    // SplitMix64-style finalizer. Important because table sizes are powers of two
    // and bucket selection uses a bit mask.
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

} // namespace twoleft
