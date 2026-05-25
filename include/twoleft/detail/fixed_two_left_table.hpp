#pragma once

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

template <
    class Key,
    class Value,
    std::size_t BucketSlots = 4,
    class Hash = std::hash<Key>,
    class Equal = std::equal_to<Key>
>
class FixedTwoLeftTable {
    static_assert(BucketSlots > 0);
    static_assert(BucketSlots <= 255, "BucketSlots must fit in one byte of load metadata");

public:
    using key_type = Key;
    using mapped_type = Value;
    using value_type = std::pair<Key, Value>;

    explicit FixedTwoLeftTable(TableConfig config, Hash hash = {}, Equal equal = {});
    ~FixedTwoLeftTable();

    FixedTwoLeftTable(const FixedTwoLeftTable&) = delete;
    FixedTwoLeftTable& operator=(const FixedTwoLeftTable&) = delete;

    FixedTwoLeftTable(FixedTwoLeftTable&& other) noexcept;
    FixedTwoLeftTable& operator=(FixedTwoLeftTable&& other) noexcept;

    InsertStatus try_insert(const value_type& entry);

    Value* find(const Key& key);
    const Value* find(const Key& key) const;
    bool contains(const Key& key) const;
    bool erase(const Key& key);
    void clear() noexcept;

    template <class F>
    void for_each_entry(F&& f);

    template <class F>
    void for_each_entry(F&& f) const;

    std::size_t size() const noexcept;
    bool empty() const noexcept;
    std::size_t bucket_count() const noexcept;
    std::size_t bucket_slots() const noexcept;
    std::size_t main_capacity() const noexcept;
    std::size_t stash_capacity() const noexcept;
    std::size_t stash_size() const noexcept;
    double load_factor() const noexcept;

private:
    struct RawSlot {
        alignas(value_type) std::byte storage[sizeof(value_type)];

        value_type* ptr() noexcept {
            return std::launder(reinterpret_cast<value_type*>(storage));
        }

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

    static TableConfig normalize_config(TableConfig config);
    static std::size_t total_slots_for(const TableConfig& config) noexcept;

    std::uint64_t hash_with_seed(const Key& key, std::uint64_t seed) const;
    std::size_t bucket1(const Key& key) const;
    std::size_t bucket2(const Key& key) const;

    std::size_t table_offset(int table) const noexcept;
    std::size_t bucket_offset(int table, std::size_t bucket) const noexcept;
    std::size_t stash_offset() const noexcept;

    value_type* bucket_slot(int table, std::size_t bucket, std::size_t index);
    const value_type* bucket_slot(int table, std::size_t bucket, std::size_t index) const;
    value_type* stash_slot(std::size_t index);
    const value_type* stash_slot(std::size_t index) const;

    void construct_in_bucket(int table, std::size_t bucket, std::size_t index, const value_type& entry);
    void construct_in_stash(std::size_t index, const value_type& entry);

    value_type* find_entry(const Key& key);
    const value_type* find_entry(const Key& key) const;

    bool erase_from_bucket(int table, std::size_t bucket, const Key& key);
    bool erase_from_stash(const Key& key);

    void promote_from_stash_while_possible();
    bool try_promote_one_from_stash();
    void remove_stash_slot_without_changing_size(std::size_t index);

    void destroy_all() noexcept;

    template <class Self, class F>
    static void for_each_entry_impl(Self& self, F&& f);

    void move_from(FixedTwoLeftTable&& other) noexcept;
};

} // namespace twoleft::detail

#include <twoleft/detail/fixed_two_left_table.tpp>
