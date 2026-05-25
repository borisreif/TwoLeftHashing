#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <utility>

#include <twoleft/detail/fixed_two_left_table.hpp>
#include <twoleft/hash_utils.hpp>
#include <twoleft/table_config.hpp>

namespace twoleft {

template <
    class Key,
    class Value,
    std::size_t BucketSlots = 4,
    class Hash = std::hash<Key>,
    class Equal = std::equal_to<Key>
>
class TwoLeftHashMap {
public:
    using key_type = Key;
    using mapped_type = Value;
    using value_type = std::pair<Key, Value>;
    using fixed_table_type = detail::FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>;

    explicit TwoLeftHashMap(
        std::size_t initial_bucket_count = 16,
        std::size_t initial_stash_capacity = 16,
        double max_load_factor = 0.80,
        Hash hash = {},
        Equal equal = {}
    );

    bool insert(Key key, Value value);
    bool insert_or_assign(Key key, Value value);

    Value* find(const Key& key);
    const Value* find(const Key& key) const;
    bool contains(const Key& key) const;
    bool erase(const Key& key);

    Value& at(const Key& key);
    const Value& at(const Key& key) const;

    void clear();

    template <class F>
    void for_each_entry(F&& f);

    template <class F>
    void for_each_entry(F&& f) const;

    std::size_t size() const noexcept;
    bool empty() const noexcept;
    std::size_t bucket_count() const noexcept;
    std::size_t bucket_slots() const noexcept;
    std::size_t main_capacity() const noexcept;
    std::size_t stash_size() const noexcept;
    std::size_t stash_capacity() const noexcept;
    double load_factor() const noexcept;

private:
    enum class RebuildGoal {
        normal,
        reduce_stash
    };

    Hash hash_{};
    Equal equal_{};
    std::size_t min_stash_capacity_ = 16;
    double max_load_factor_ = 0.80;
    fixed_table_type table_;

    TableConfig make_config(std::size_t bucket_count) const;
    std::size_t choose_stash_capacity(std::size_t bucket_count) const;

    bool should_grow_before_insert() const noexcept;
    std::size_t stash_pressure_threshold() const noexcept;
    bool stash_pressure_is_high() const noexcept;
    bool satisfies_rebuild_goal(const fixed_table_type& candidate, RebuildGoal goal) const noexcept;

    void rebuild(std::size_t desired_bucket_count, RebuildGoal goal);
};

} // namespace twoleft

#include <twoleft/two_left_hash_map.tpp>
