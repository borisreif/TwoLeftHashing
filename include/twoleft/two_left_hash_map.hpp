#pragma once

/**
 * @file two_left_hash_map.hpp
 * @brief Public dynamic wrapper for the experimental 2-left hash map.
 *
 * ## Architectural idea
 *
 * This project deliberately separates **mechanism** from **policy**:
 *
 * @code{.text}
 * TwoLeftHashMap
 *     public user-facing container
 *     owns growth / rebuild / stash-pressure policy
 *
 * detail::FixedTwoLeftTable
 *     fixed-capacity storage image
 *     owns the raw layout:
 *         | table 1 | table 2 | stash |
 *     never resizes itself
 * @endcode
 *
 * `TwoLeftHashMap` is the class users normally instantiate. It has a map-like
 * interface and can grow dynamically. Internally it owns a `FixedTwoLeftTable`.
 * When the fixed table becomes too full or the stash becomes too occupied, the
 * wrapper constructs a fresh fixed table, reinserts all entries, and swaps the
 * new table into place.
 *
 * ## 2-left insertion rule
 *
 * For a key `k`, two independent bucket candidates are computed:
 *
 * @code{.text}
 * b1 = h1(k) & (bucket_count - 1)
 * b2 = h2(k) & (bucket_count - 1)
 * @endcode
 *
 * The entry is inserted into the less-loaded of the two candidate buckets. Ties
 * go to the left table. If both candidate buckets are full, the fixed table uses
 * the stash as a last fallback.
 */

#include <algorithm>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <utility>

#include <twoleft/detail/fixed_two_left_table.hpp>
#include <twoleft/hash_utils.hpp>
#include <twoleft/table_config.hpp>

namespace twoleft {

/**
 * @brief Dynamically growing experimental 2-left hash map.
 *
 * @tparam Key Key type. Tested with `int` and `std::string`.
 * @tparam Value Mapped value type.
 * @tparam BucketSlots Number of slots per bucket in each main table.
 * @tparam Hash Hash functor used as the base hash function.
 * @tparam Equal Equality functor used for key comparison.
 *
 * The public class is intentionally small and delegates exact placement to
 * `detail::FixedTwoLeftTable`. The wrapper is responsible for:
 *
 * - deciding when the load factor is too high,
 * - deciding when stash pressure is too high,
 * - rebuilding with fresh hash seeds,
 * - growing to a larger table when necessary.
 */
template <
    class Key,
    class Value,
    std::size_t BucketSlots = 4,
    class Hash = std::hash<Key>,
    class Equal = std::equal_to<Key>
>
class TwoLeftHashMap {
public:
    /** @brief Type used for keys. */
    using key_type = Key;

    /** @brief Type used for mapped values. */
    using mapped_type = Value;

    /** @brief Stored key/value pair type. */
    using value_type = std::pair<Key, Value>;

    /** @brief Internal fixed table type used by the dynamic wrapper. */
    using fixed_table_type = detail::FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>;

    /**
     * @brief Construct a dynamic 2-left hash map.
     *
     * @param initial_bucket_count Initial number of buckets per main table. The
     * value is normalized to a power of two.
     * @param initial_stash_capacity Minimum number of stash slots. The value is
     * normalized to a power of two.
     * @param max_load_factor Maximum main-table load factor before growing.
     * Must be in the open interval `(0, 1)`.
     * @param hash Hash functor.
     * @param equal Equality functor.
     *
     * @throws std::invalid_argument if `max_load_factor` is not in `(0, 1)`.
     */
    explicit TwoLeftHashMap(
        std::size_t initial_bucket_count = 16,
        std::size_t initial_stash_capacity = 16,
        double max_load_factor = 0.80,
        Hash hash = {},
        Equal equal = {}
    );

    /**
     * @brief Insert a new key/value pair if the key is not already present.
     *
     * The wrapper may rebuild or grow the fixed table before or after the actual
     * insertion. Existing references and pointers into the map should therefore
     * be considered invalid after insertion.
     *
     * @param key Key to insert.
     * @param value Value to insert.
     * @return `true` if a new entry was inserted, `false` if the key already
     * existed.
     */
    bool insert(Key key, Value value);

    /**
     * @brief Insert a key/value pair or assign to the existing value.
     *
     * @param key Key to insert or update.
     * @param value New mapped value.
     * @return `true` if a new entry was inserted, `false` if an existing entry
     * was overwritten.
     */
    bool insert_or_assign(Key key, Value value);

    /**
     * @brief Find a mutable value by key.
     *
     * @param key Key to search for.
     * @return Pointer to the mapped value, or `nullptr` if the key is absent.
     */
    Value* find(const Key& key);

    /**
     * @brief Find a read-only value by key.
     *
     * @param key Key to search for.
     * @return Pointer to the mapped value, or `nullptr` if the key is absent.
     */
    const Value* find(const Key& key) const;

    /**
     * @brief Check whether the map contains a key.
     *
     * @param key Key to search for.
     * @return `true` if the key exists, otherwise `false`.
     */
    bool contains(const Key& key) const;

    /**
     * @brief Erase a key/value pair by key.
     *
     * If an entry is removed from a main table, the fixed table tries to promote
     * compatible entries from the stash back into normal buckets.
     *
     * @param key Key to erase.
     * @return `true` if an entry was erased, otherwise `false`.
     */
    bool erase(const Key& key);

    /**
     * @brief Access a mutable mapped value and throw if the key is absent.
     *
     * @param key Key to search for.
     * @return Reference to the mapped value.
     * @throws std::out_of_range if the key is absent.
     */
    Value& at(const Key& key);

    /**
     * @brief Access a read-only mapped value and throw if the key is absent.
     *
     * @param key Key to search for.
     * @return Reference to the mapped value.
     * @throws std::out_of_range if the key is absent.
     */
    const Value& at(const Key& key) const;

    /**
     * @brief Remove all entries while keeping the current capacity.
     */
    void clear();

    /**
     * @brief Visit every stored key/value pair.
     *
     * Iteration order is an implementation detail: table 1 buckets, table 2
     * buckets, then stash.
     *
     * @tparam F Callable type accepting `value_type&`.
     * @param f Callable invoked for each entry.
     */
    template <class F>
    void for_each_entry(F&& f);

    /**
     * @brief Visit every stored key/value pair through a const map.
     *
     * @tparam F Callable type accepting `const value_type&`.
     * @param f Callable invoked for each entry.
     */
    template <class F>
    void for_each_entry(F&& f) const;

    /** @brief Return the number of stored entries. */
    std::size_t size() const noexcept;

    /** @brief Return whether the map currently stores no entries. */
    bool empty() const noexcept;

    /** @brief Return the number of buckets in each of the two main tables. */
    std::size_t bucket_count() const noexcept;

    /** @brief Return the number of slots in each bucket. */
    std::size_t bucket_slots() const noexcept;

    /**
     * @brief Return the normal main-table capacity, excluding the stash.
     *
     * @return `2 * bucket_count() * bucket_slots()`.
     */
    std::size_t main_capacity() const noexcept;

    /** @brief Return the current number of entries stored in the stash. */
    std::size_t stash_size() const noexcept;

    /** @brief Return the total number of stash slots. */
    std::size_t stash_capacity() const noexcept;

    /**
     * @brief Return the main-table load factor.
     *
     * The stash is deliberately excluded because it is an overflow mechanism, not
     * normal capacity.
     *
     * @return `size() / main_capacity()`.
     */
    double load_factor() const noexcept;

private:
    /** @brief Goal used to decide whether a rebuild attempt is acceptable. */
    enum class RebuildGoal {
        /** @brief A normal grow/rehash with no special stash constraint. */
        normal,

        /** @brief Try to reduce stash pressure by changing seeds before growing. */
        reduce_stash
    };

    Hash hash_{};
    Equal equal_{};
    std::size_t min_stash_capacity_ = 16;
    double max_load_factor_ = 0.80;
    fixed_table_type table_;

    /**
     * @brief Create a new fixed-table configuration for a bucket count.
     *
     * @param bucket_count Desired number of buckets per main table.
     * @return Normalized configuration with fresh hash seeds.
     */
    TableConfig make_config(std::size_t bucket_count) const;

    /**
     * @brief Choose the stash capacity for a given bucket count.
     *
     * @param bucket_count Number of buckets per main table.
     * @return Power-of-two stash capacity.
     */
    std::size_t choose_stash_capacity(std::size_t bucket_count) const;

    /**
     * @brief Check whether the table should grow before inserting one more entry.
     *
     * @return `true` if inserting one more element would exceed the load policy.
     */
    bool should_grow_before_insert() const noexcept;

    /**
     * @brief Return the stash size at which the wrapper considers pressure high.
     *
     * @return Stash pressure threshold.
     */
    std::size_t stash_pressure_threshold() const noexcept;

    /**
     * @brief Check whether the current stash size is above the pressure threshold.
     *
     * @return `true` if the stash is considered too occupied.
     */
    bool stash_pressure_is_high() const noexcept;

    /**
     * @brief Decide whether a candidate rebuilt table satisfies the rebuild goal.
     *
     * @param candidate Freshly built table candidate.
     * @param goal Rebuild goal.
     * @return `true` if the candidate should replace the current table.
     */
    bool satisfies_rebuild_goal(const fixed_table_type& candidate, RebuildGoal goal) const noexcept;

    /**
     * @brief Rebuild the map into a new fixed table.
     *
     * The method tries several fresh seed pairs at the requested size. If all
     * attempts fail or do not satisfy the requested goal, it doubles the requested
     * bucket count and tries again.
     *
     * @param desired_bucket_count Desired number of buckets per main table.
     * @param goal Rebuild goal.
     */
    void rebuild(std::size_t desired_bucket_count, RebuildGoal goal);
};

} // namespace twoleft

#include <twoleft/two_left_hash_map.tpp>
