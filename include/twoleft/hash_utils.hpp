#pragma once

/**
 * @file hash_utils.hpp
 * @brief Small hashing and power-of-two helpers used by the hash-map internals.
 *
 * The table deliberately uses power-of-two capacities so that bucket selection can
 * be implemented as a cheap bit mask:
 *
 * @code
 * bucket = hash & (bucket_count - 1);
 * @endcode
 *
 * Because this only uses the low bits of the hash value, we run the user-provided
 * hash through a small 64-bit mixing function before masking. This reduces the
 * chance that weak low bits in `std::hash<Key>` or a custom hash produce many
 * collisions.
 */

#include <bit>
#include <cstddef>
#include <cstdint>
#include <random>

namespace twoleft {

/**
 * @brief Return the smallest power of two that is at least `n`.
 *
 * The hash table normalizes bucket counts and stash capacities to powers of two.
 * A value of zero is mapped to one so that the result is always usable as a
 * non-zero capacity.
 *
 * @param n Requested size.
 * @return The smallest power of two greater than or equal to `n`.
 */
inline std::size_t ceil_power_of_two(std::size_t n) {
    return n <= 1 ? 1 : std::bit_ceil(n);
}

/**
 * @brief Generate a 64-bit seed for deriving independent bucket choices.
 *
 * 2-left hashing needs two independent-looking hash functions. This project
 * derives them from one user hash by combining it with two random seeds and then
 * applying `mix64()`.
 *
 * @return A 64-bit seed value.
 */
inline std::uint64_t make_seed() {
    static std::random_device rd;
    const auto hi = static_cast<std::uint64_t>(rd());
    const auto lo = static_cast<std::uint64_t>(rd());
    return (hi << 32) ^ lo;
}

/**
 * @brief Mix a 64-bit integer so that its low bits are well distributed.
 *
 * This is a SplitMix64-style finalizer. It is not used as a cryptographic hash;
 * it is only used to scramble hash values before the table applies a power-of-two
 * mask.
 *
 * @param x Input value to mix.
 * @return Mixed 64-bit value.
 */
inline std::uint64_t mix64(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

} // namespace twoleft
