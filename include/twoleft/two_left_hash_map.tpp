#pragma once

namespace twoleft {

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::TwoLeftHashMap(
    std::size_t initial_bucket_count,
    std::size_t initial_stash_capacity,
    double max_load_factor,
    Hash hash,
    Equal equal
)
    : hash_(std::move(hash)),
      equal_(std::move(equal)),
      min_stash_capacity_(ceil_power_of_two(initial_stash_capacity)),
      max_load_factor_(max_load_factor),
      table_(make_config(ceil_power_of_two(initial_bucket_count)), hash_, equal_) {
    if (!(max_load_factor_ > 0.0 && max_load_factor_ < 1.0)) {
        throw std::invalid_argument("max_load_factor must be in the interval (0, 1)");
    }
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
bool TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::insert(Key key, Value value) {
    value_type entry{std::move(key), std::move(value)};

    for (;;) {
        if (should_grow_before_insert()) {
            rebuild(table_.bucket_count() * 2, RebuildGoal::normal);
        }

        const auto status = table_.try_insert(entry);

        switch (status) {
            case InsertStatus::inserted_main_table:
                return true;

            case InsertStatus::inserted_stash:
                if (stash_pressure_is_high()) {
                    // First try same size with fresh seeds. If that is still
                    // stash-heavy, rebuild() will grow.
                    rebuild(table_.bucket_count(), RebuildGoal::reduce_stash);
                }
                return true;

            case InsertStatus::already_present:
                return false;

            case InsertStatus::full:
                rebuild(table_.bucket_count() * 2, RebuildGoal::normal);
                break;
        }
    }
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
bool TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::insert_or_assign(Key key, Value value) {
    if (auto* existing = find(key)) {
        *existing = std::move(value);
        return false;
    }
    return insert(std::move(key), std::move(value));
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
Value* TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::find(const Key& key) {
    return table_.find(key);
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
const Value* TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::find(const Key& key) const {
    return table_.find(key);
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
bool TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::contains(const Key& key) const {
    return table_.contains(key);
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
bool TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::erase(const Key& key) {
    return table_.erase(key);
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
Value& TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::at(const Key& key) {
    if (auto* value = find(key)) {
        return *value;
    }
    throw std::out_of_range("TwoLeftHashMap::at: key not found");
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
const Value& TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::at(const Key& key) const {
    if (auto* value = find(key)) {
        return *value;
    }
    throw std::out_of_range("TwoLeftHashMap::at: key not found");
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
void TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::clear() {
    table_.clear();
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
template <class F>
void TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::for_each_entry(F&& f) {
    table_.for_each_entry(std::forward<F>(f));
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
template <class F>
void TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::for_each_entry(F&& f) const {
    table_.for_each_entry(std::forward<F>(f));
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::size() const noexcept {
    return table_.size();
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
bool TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::empty() const noexcept {
    return table_.empty();
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::bucket_count() const noexcept {
    return table_.bucket_count();
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::bucket_slots() const noexcept {
    return table_.bucket_slots();
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::main_capacity() const noexcept {
    return table_.main_capacity();
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::stash_size() const noexcept {
    return table_.stash_size();
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::stash_capacity() const noexcept {
    return table_.stash_capacity();
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
double TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::load_factor() const noexcept {
    return table_.load_factor();
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
TableConfig TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::make_config(
    std::size_t bucket_count
) const {
    bucket_count = ceil_power_of_two(bucket_count);
    return TableConfig{
        .bucket_count = bucket_count,
        .stash_capacity = choose_stash_capacity(bucket_count),
        .seed1 = make_seed(),
        .seed2 = make_seed()
    };
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::choose_stash_capacity(
    std::size_t bucket_count
) const {
    // Tiny but scaling stash. This is wrapper policy, not fixed-table logic.
    const auto scaled = ceil_power_of_two(std::max<std::size_t>(8, bucket_count / 16));
    return std::max(min_stash_capacity_, scaled);
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
bool TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::should_grow_before_insert() const noexcept {
    const auto projected = static_cast<double>(table_.size() + 1);
    const auto threshold = max_load_factor_ * static_cast<double>(table_.main_capacity());
    return projected > threshold;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::stash_pressure_threshold() const noexcept {
    return std::max<std::size_t>(1, table_.stash_capacity() / 2);
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
bool TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::stash_pressure_is_high() const noexcept {
    return table_.stash_size() > stash_pressure_threshold();
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
bool TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::satisfies_rebuild_goal(
    const fixed_table_type& candidate,
    RebuildGoal goal
) const noexcept {
    if (goal == RebuildGoal::normal) {
        return true;
    }
    const auto threshold = std::max<std::size_t>(1, candidate.stash_capacity() / 2);
    return candidate.stash_size() <= threshold;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
void TwoLeftHashMap<Key, Value, BucketSlots, Hash, Equal>::rebuild(
    std::size_t desired_bucket_count,
    RebuildGoal goal
) {
    desired_bucket_count = ceil_power_of_two(desired_bucket_count);

    for (;;) {
        constexpr int attempts_per_size = 4;

        for (int attempt = 0; attempt < attempts_per_size; ++attempt) {
            fixed_table_type fresh(make_config(desired_bucket_count), hash_, equal_);
            bool ok = true;

            table_.for_each_entry([&](const value_type& entry) {
                if (!ok) {
                    return;
                }

                const auto status = fresh.try_insert(entry);
                if (status == InsertStatus::full) {
                    ok = false;
                }
            });

            if (ok && satisfies_rebuild_goal(fresh, goal)) {
                table_ = std::move(fresh);
                return;
            }
        }

        desired_bucket_count *= 2;
    }
}

} // namespace twoleft
