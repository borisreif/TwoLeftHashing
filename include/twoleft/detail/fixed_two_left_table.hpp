#pragma once

/**
 * @file fixed_two_left_table.hpp
 * @brief Fixed-capacity storage layer used by `twoleft::TwoLeftHashMap`.
 *
 * `FixedTwoLeftTable` is the lower-level mechanism. It does not know about
 * growth policy, load-factor policy, or public container semantics. It only owns
 * one concrete storage image:
 *
 * @code{.text}
 * slots_ vector:
 *
 * |---------------- table 1 ----------------|---------------- table 2 ----------------|--- stash ---|
 * | bucket 0 | bucket 1 | ... | bucket n-1 | bucket 0 | bucket 1 | ... | bucket n-1 | 0 1 2 ...  |
 * | s0 s1... | s0 s1... |     | s0 s1...   | s0 s1... | s0 s1... |     | s0 s1...   | entries    |
 * @endcode
 *
 * Each main-table bucket stores `BucketSlots` physical slots. The occupied part
 * of a bucket is compact:
 *
 * @code{.text}
 * bucket with load = 3 and BucketSlots = 4:
 *
 * +---------+---------+---------+---------+
 * | used    | used    | used    | unused  |
 * +---------+---------+---------+---------+
 *   index 0   index 1   index 2   index 3
 * @endcode
 *
 * Because the occupied prefix is compact, deletion can remove a slot by moving
 * the last occupied slot of the bucket into the erased position. No tombstones
 * are required.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include <twoleft/hash_utils.hpp>
#include <twoleft/table_config.hpp>

namespace twoleft::detail {

/**
 * @brief Fixed-capacity 2-left table image.
 *
 * @tparam Key Key type.
 * @tparam Value Mapped value type.
 * @tparam BucketSlots Number of physical slots in each bucket.
 * @tparam Hash Hash functor used as the base hash.
 * @tparam Equal Equality functor used for key comparison.
 *
 * The fixed table implements only exact placement mechanics:
 *
 * - two independent bucket choices per key,
 * - choose the less-loaded candidate bucket,
 * - use the stash only if both candidate buckets are full,
 * - return an `InsertStatus` instead of resizing.
 */
template <
    class Key,
    class Value,
    std::size_t BucketSlots = 4,
    class Hash = std::hash<Key>,
    class Equal = std::equal_to<Key>
>
class FixedTwoLeftTable {
    static_assert(BucketSlots > 0, "BucketSlots must be positive");
    static_assert(BucketSlots <= 255, "BucketSlots must fit in one byte of load metadata");

public:
    /** @brief Type used for keys. */
    using key_type = Key;

    /** @brief Type used for mapped values. */
    using mapped_type = Value;

    /** @brief Stored key/value pair type. */
    using value_type = std::pair<Key, Value>;

    /**
     * @brief Construct a fixed table with the given configuration.
     *
     * The constructor normalizes bucket and stash sizes to powers of two and
     * creates random seeds when the config contains zero seeds.
     *
     * @param config Table size and seed configuration.
     * @param hash Hash functor.
     * @param equal Equality functor.
     */
    explicit FixedTwoLeftTable(TableConfig config, Hash hash = {}, Equal equal = {});

    /**
     * @brief Destroy all live entries and release storage.
     */
    ~FixedTwoLeftTable();

    /** @brief Copy construction is disabled because the table owns raw slots. */
    FixedTwoLeftTable(const FixedTwoLeftTable&) = delete;

    /** @brief Copy assignment is disabled because the table owns raw slots. */
    FixedTwoLeftTable& operator=(const FixedTwoLeftTable&) = delete;

    /**
     * @brief Move construct a fixed table.
     *
     * @param other Table to move from.
     */
    FixedTwoLeftTable(FixedTwoLeftTable&& other) noexcept;

    /**
     * @brief Move assign a fixed table.
     *
     * @param other Table to move from.
     * @return Reference to `*this`.
     */
    FixedTwoLeftTable& operator=(FixedTwoLeftTable&& other) noexcept;

    /**
     * @brief Try to insert an entry into this fixed-capacity table.
     *
     * This function never grows the table. It either inserts into a main bucket,
     * inserts into the stash, detects an existing key, or reports that the table
     * is full.
     *
     * @param entry Key/value pair to insert.
     * @return Detailed insertion status.
     */
    InsertStatus try_insert(const value_type& entry);

    /**
     * @brief Find a mutable mapped value by key.
     *
     * @param key Key to search for.
     * @return Pointer to the mapped value, or `nullptr` if absent.
     */
    Value* find(const Key& key);

    /**
     * @brief Find a read-only mapped value by key.
     *
     * @param key Key to search for.
     * @return Pointer to the mapped value, or `nullptr` if absent.
     */
    const Value* find(const Key& key) const;

    /**
     * @brief Check whether a key is present.
     *
     * @param key Key to search for.
     * @return `true` if the key exists.
     */
    bool contains(const Key& key) const;

    /**
     * @brief Erase a key/value pair by key.
     *
     * If the erase frees a main-table slot, the table tries to promote compatible
     * stash entries back into their legal main buckets.
     *
     * @param key Key to erase.
     * @return `true` if an entry was removed.
     */
    bool erase(const Key& key);

    /**
     * @brief Destroy all live entries while keeping the current capacity.
     */
    void clear() noexcept;

    /**
     * @brief Visit every stored entry.
     *
     * @tparam F Callable type accepting `value_type&`.
     * @param f Callable invoked for each entry.
     */
    template <class F>
    void for_each_entry(F&& f);

    /**
     * @brief Visit every stored entry through a const table.
     *
     * @tparam F Callable type accepting `const value_type&`.
     * @param f Callable invoked for each entry.
     */
    template <class F>
    void for_each_entry(F&& f) const;

    /** @brief Return the number of stored entries, including stash entries. */
    std::size_t size() const noexcept;

    /** @brief Return whether the table contains no entries. */
    bool empty() const noexcept;

    /** @brief Return the number of buckets in each main table. */
    std::size_t bucket_count() const noexcept;

    /** @brief Return the number of physical slots per bucket. */
    std::size_t bucket_slots() const noexcept;

    /**
     * @brief Return the capacity of the two main tables, excluding the stash.
     *
     * @return `2 * bucket_count() * bucket_slots()`.
     */
    std::size_t main_capacity() const noexcept;

    /** @brief Return the number of available stash slots. */
    std::size_t stash_capacity() const noexcept;

    /** @brief Return the number of currently occupied stash slots. */
    std::size_t stash_size() const noexcept;

    /**
     * @brief Return the main-table load factor.
     *
     * The stash is intentionally excluded because stash occupancy is a separate
     * diagnostic signal.
     *
     * @return `size() / main_capacity()`.
     */
    double load_factor() const noexcept;

private:
    /**
     * @brief Raw storage for one `value_type` without automatically constructing it.
     *
     * Slots are constructed with `std::construct_at()` only when they become
     * occupied. This avoids requiring `Key` and `Value` to be default-constructible.
     */
    struct RawSlot {
        /** @brief Properly aligned byte storage for one value. */
        alignas(value_type) std::byte storage[sizeof(value_type)];

        /**
         * @brief Interpret this raw storage as a mutable value pointer.
         *
         * @return Pointer to the object living in the storage.
         */
        value_type* ptr() noexcept {
            return std::launder(reinterpret_cast<value_type*>(storage));
        }

        /**
         * @brief Interpret this raw storage as a read-only value pointer.
         *
         * @return Pointer to the object living in the storage.
         */
        const value_type* ptr() const noexcept {
            return std::launder(reinterpret_cast<const value_type*>(storage));
        }
    };

    TableConfig config_{};
    Hash hash_{};
    Equal equal_{};
    std::vector<RawSlot> slots_{};
    std::vector<std::uint8_t> load1_{};
    std::vector<std::uint8_t> load2_{};
    std::size_t size_ = 0;
    std::size_t stash_size_ = 0;

    /**
     * @brief Normalize capacities and seed values in a table config.
     *
     * @param config Input configuration.
     * @return Normalized configuration.
     */
    static TableConfig normalize_config(TableConfig config);

    /**
     * @brief Compute the total number of raw slots needed by a configuration.
     *
     * @param config Normalized table configuration.
     * @return Number of raw slots in `slots_`.
     */
    static std::size_t total_slots_for(const TableConfig& config) noexcept;

    /**
     * @brief Hash a key with one table seed and mix the result.
     *
     * @param key Key to hash.
     * @param seed Seed for one of the two tables.
     * @return Mixed 64-bit hash value.
     */
    std::uint64_t hash_with_seed(const Key& key, std::uint64_t seed) const;

    /** @brief Return the candidate bucket index in table 1 for a key. */
    std::size_t bucket1(const Key& key) const;

    /** @brief Return the candidate bucket index in table 2 for a key. */
    std::size_t bucket2(const Key& key) const;

    /**
     * @brief Return the raw-slot offset at which a main table begins.
     *
     * @param table Table index: `0` for table 1, `1` for table 2.
     * @return Offset into `slots_`.
     */
    std::size_t table_offset(int table) const noexcept;

    /**
     * @brief Return the raw-slot offset for the first slot of a bucket.
     *
     * @param table Table index: `0` for table 1, `1` for table 2.
     * @param bucket Bucket index.
     * @return Offset into `slots_`.
     */
    std::size_t bucket_offset(int table, std::size_t bucket) const noexcept;

    /**
     * @brief Return the raw-slot offset at which the stash begins.
     *
     * @return Offset into `slots_`.
     */
    std::size_t stash_offset() const noexcept;

    /**
     * @brief Return a mutable pointer to a main-table slot.
     *
     * @param table Table index.
     * @param bucket Bucket index.
     * @param index Slot index inside the bucket.
     * @return Pointer to the slot value.
     */
    value_type* bucket_slot(int table, std::size_t bucket, std::size_t index);

    /**
     * @brief Return a read-only pointer to a main-table slot.
     *
     * @param table Table index.
     * @param bucket Bucket index.
     * @param index Slot index inside the bucket.
     * @return Pointer to the slot value.
     */
    const value_type* bucket_slot(int table, std::size_t bucket, std::size_t index) const;

    /**
     * @brief Return a mutable pointer to a stash slot.
     *
     * @param index Slot index inside the stash.
     * @return Pointer to the stash value.
     */
    value_type* stash_slot(std::size_t index);

    /**
     * @brief Return a read-only pointer to a stash slot.
     *
     * @param index Slot index inside the stash.
     * @return Pointer to the stash value.
     */
    const value_type* stash_slot(std::size_t index) const;

    /**
     * @brief Construct an entry in an unoccupied main-table slot.
     *
     * @param table Table index.
     * @param bucket Bucket index.
     * @param index Slot index inside the bucket.
     * @param entry Entry to copy into the slot.
     */
    void construct_in_bucket(int table, std::size_t bucket, std::size_t index, const value_type& entry);

    /**
     * @brief Construct an entry in an unoccupied stash slot.
     *
     * @param index Slot index inside the stash.
     * @param entry Entry to copy into the slot.
     */
    void construct_in_stash(std::size_t index, const value_type& entry);

    /**
     * @brief Find a mutable full entry by key.
     *
     * @param key Key to search for.
     * @return Pointer to the entry, or `nullptr` if absent.
     */
    value_type* find_entry(const Key& key);

    /**
     * @brief Find a read-only full entry by key.
     *
     * @param key Key to search for.
     * @return Pointer to the entry, or `nullptr` if absent.
     */
    const value_type* find_entry(const Key& key) const;

    /**
     * @brief Erase a key from one candidate main bucket.
     *
     * @param table Table index.
     * @param bucket Bucket index.
     * @param key Key to erase.
     * @return `true` if an entry was erased.
     */
    bool erase_from_bucket(int table, std::size_t bucket, const Key& key);

    /**
     * @brief Erase a key from the stash.
     *
     * @param key Key to erase.
     * @return `true` if an entry was erased.
     */
    bool erase_from_stash(const Key& key);

    /**
     * @brief Promote all currently promotable stash entries into main buckets.
     */
    void promote_from_stash_while_possible();

    /**
     * @brief Try to promote a single stash entry into one of its legal buckets.
     *
     * @return `true` if one entry was promoted.
     */
    bool try_promote_one_from_stash();

    /**
     * @brief Remove a stash slot while preserving the total entry count.
     *
     * Used when a stash entry is promoted to a main bucket. `size_` does not
     * change because the entry remains stored in the table as a whole.
     *
     * @param index Stash index to remove.
     */
    void remove_stash_slot_without_changing_size(std::size_t index);

    /**
     * @brief Destroy every live entry and reset all loads to zero.
     */
    void destroy_all() noexcept;

    /**
     * @brief Shared implementation for const and non-const iteration.
     *
     * @tparam Self Either `FixedTwoLeftTable` or `const FixedTwoLeftTable`.
     * @tparam F Callable type.
     * @param self Table being iterated.
     * @param f Callable invoked for each entry.
     */
    template <class Self, class F>
    static void for_each_entry_impl(Self& self, F&& f);

    /**
     * @brief Move all internal state from another table.
     *
     * @param other Table to move from.
     */
    void move_from(FixedTwoLeftTable&& other) noexcept;
};

} // namespace twoleft::detail

#include <twoleft/detail/fixed_two_left_table.tpp>
