#pragma once

namespace twoleft::detail {

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::FixedTwoLeftTable(
    TableConfig config,
    Hash hash,
    Equal equal
)
    : config_(normalize_config(config)),
      hash_(std::move(hash)),
      equal_(std::move(equal)),
      slots_(total_slots_for(config_)),
      load1_(config_.bucket_count, 0),
      load2_(config_.bucket_count, 0) {}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::~FixedTwoLeftTable() {
    destroy_all();
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::FixedTwoLeftTable(
    FixedTwoLeftTable&& other
) noexcept {
    move_from(std::move(other));
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
auto FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::operator=(
    FixedTwoLeftTable&& other
) noexcept -> FixedTwoLeftTable& {
    if (this != &other) {
        destroy_all();
        move_from(std::move(other));
    }
    return *this;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
InsertStatus FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::try_insert(
    const value_type& entry
) {
    if (find(entry.first) != nullptr) {
        return InsertStatus::already_present;
    }

    const auto b1 = bucket1(entry.first);
    const auto b2 = bucket2(entry.first);

    // 2-left choice: choose the less-loaded candidate bucket; ties go left.
    if (load1_[b1] <= load2_[b2]) {
        if (load1_[b1] < BucketSlots) {
            construct_in_bucket(0, b1, load1_[b1], entry);
            ++load1_[b1];
            ++size_;
            return InsertStatus::inserted_main_table;
        }
    } else {
        if (load2_[b2] < BucketSlots) {
            construct_in_bucket(1, b2, load2_[b2], entry);
            ++load2_[b2];
            ++size_;
            return InsertStatus::inserted_main_table;
        }
    }

    if (stash_size_ < config_.stash_capacity) {
        construct_in_stash(stash_size_, entry);
        ++stash_size_;
        ++size_;
        return InsertStatus::inserted_stash;
    }

    return InsertStatus::full;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
Value* FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::find(const Key& key) {
    if (auto* entry = find_entry(key)) {
        return &entry->second;
    }
    return nullptr;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
const Value* FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::find(const Key& key) const {
    if (auto* entry = find_entry(key)) {
        return &entry->second;
    }
    return nullptr;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
bool FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::contains(const Key& key) const {
    return find(key) != nullptr;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
bool FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::erase(const Key& key) {
    const auto b1 = bucket1(key);
    if (erase_from_bucket(0, b1, key)) {
        promote_from_stash_while_possible();
        return true;
    }

    const auto b2 = bucket2(key);
    if (erase_from_bucket(1, b2, key)) {
        promote_from_stash_while_possible();
        return true;
    }

    return erase_from_stash(key);
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
void FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::clear() noexcept {
    destroy_all();
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
template <class F>
void FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::for_each_entry(F&& f) {
    for_each_entry_impl(*this, std::forward<F>(f));
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
template <class F>
void FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::for_each_entry(F&& f) const {
    for_each_entry_impl(*this, std::forward<F>(f));
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::size() const noexcept {
    return size_;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
bool FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::empty() const noexcept {
    return size_ == 0;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::bucket_count() const noexcept {
    return config_.bucket_count;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::bucket_slots() const noexcept {
    return BucketSlots;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::main_capacity() const noexcept {
    return 2 * config_.bucket_count * BucketSlots;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::stash_capacity() const noexcept {
    return config_.stash_capacity;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::stash_size() const noexcept {
    return stash_size_;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
double FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::load_factor() const noexcept {
    return main_capacity() == 0
        ? 0.0
        : static_cast<double>(size_) / static_cast<double>(main_capacity());
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
TableConfig FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::normalize_config(
    TableConfig config
) {
    config.bucket_count = ceil_power_of_two(config.bucket_count);
    config.stash_capacity = ceil_power_of_two(config.stash_capacity);

    if (config.seed1 == 0) {
        config.seed1 = make_seed();
    }
    if (config.seed2 == 0) {
        config.seed2 = make_seed();
    }
    if (config.seed1 == config.seed2) {
        config.seed2 = mix64(config.seed2 + 1);
    }

    return config;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::total_slots_for(
    const TableConfig& config
) noexcept {
    return 2 * config.bucket_count * BucketSlots + config.stash_capacity;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::uint64_t FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::hash_with_seed(
    const Key& key,
    std::uint64_t seed
) const {
    return mix64(static_cast<std::uint64_t>(hash_(key)) ^ seed);
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::bucket1(const Key& key) const {
    return static_cast<std::size_t>(hash_with_seed(key, config_.seed1)) &
           (config_.bucket_count - 1);
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::bucket2(const Key& key) const {
    return static_cast<std::size_t>(hash_with_seed(key, config_.seed2)) &
           (config_.bucket_count - 1);
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::table_offset(
    int table
) const noexcept {
    return table == 0 ? 0 : config_.bucket_count * BucketSlots;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::bucket_offset(
    int table,
    std::size_t bucket
) const noexcept {
    return table_offset(table) + bucket * BucketSlots;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
std::size_t FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::stash_offset() const noexcept {
    return 2 * config_.bucket_count * BucketSlots;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
auto FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::bucket_slot(
    int table,
    std::size_t bucket,
    std::size_t index
) -> value_type* {
    return slots_[bucket_offset(table, bucket) + index].ptr();
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
auto FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::bucket_slot(
    int table,
    std::size_t bucket,
    std::size_t index
) const -> const value_type* {
    return slots_[bucket_offset(table, bucket) + index].ptr();
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
auto FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::stash_slot(
    std::size_t index
) -> value_type* {
    return slots_[stash_offset() + index].ptr();
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
auto FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::stash_slot(
    std::size_t index
) const -> const value_type* {
    return slots_[stash_offset() + index].ptr();
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
void FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::construct_in_bucket(
    int table,
    std::size_t bucket,
    std::size_t index,
    const value_type& entry
) {
    std::construct_at(bucket_slot(table, bucket, index), entry);
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
void FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::construct_in_stash(
    std::size_t index,
    const value_type& entry
) {
    std::construct_at(stash_slot(index), entry);
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
auto FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::find_entry(
    const Key& key
) -> value_type* {
    const auto b1 = bucket1(key);
    for (std::size_t i = 0; i < load1_[b1]; ++i) {
        auto* entry = bucket_slot(0, b1, i);
        if (equal_(entry->first, key)) {
            return entry;
        }
    }

    const auto b2 = bucket2(key);
    for (std::size_t i = 0; i < load2_[b2]; ++i) {
        auto* entry = bucket_slot(1, b2, i);
        if (equal_(entry->first, key)) {
            return entry;
        }
    }

    for (std::size_t i = 0; i < stash_size_; ++i) {
        auto* entry = stash_slot(i);
        if (equal_(entry->first, key)) {
            return entry;
        }
    }

    return nullptr;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
auto FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::find_entry(
    const Key& key
) const -> const value_type* {
    return const_cast<FixedTwoLeftTable*>(this)->find_entry(key);
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
bool FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::erase_from_bucket(
    int table,
    std::size_t bucket,
    const Key& key
) {
    auto& load = table == 0 ? load1_[bucket] : load2_[bucket];

    for (std::size_t i = 0; i < load; ++i) {
        auto* current = bucket_slot(table, bucket, i);
        if (!equal_(current->first, key)) {
            continue;
        }

        const auto last = static_cast<std::size_t>(load - 1);
        if (i != last) {
            auto* last_ptr = bucket_slot(table, bucket, last);
            *current = std::move(*last_ptr);
            std::destroy_at(last_ptr);
        } else {
            std::destroy_at(current);
        }

        --load;
        --size_;
        return true;
    }

    return false;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
bool FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::erase_from_stash(
    const Key& key
) {
    for (std::size_t i = 0; i < stash_size_; ++i) {
        auto* current = stash_slot(i);
        if (!equal_(current->first, key)) {
            continue;
        }

        remove_stash_slot_without_changing_size(i);
        --size_;
        return true;
    }

    return false;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
void FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::promote_from_stash_while_possible() {
    while (try_promote_one_from_stash()) {
        // Keep promoting until no stashed item fits in either of its two buckets.
    }
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
bool FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::try_promote_one_from_stash() {
    for (std::size_t i = 0; i < stash_size_; ++i) {
        auto* entry = stash_slot(i);
        const auto b1 = bucket1(entry->first);
        const auto b2 = bucket2(entry->first);

        const bool can_use_1 = load1_[b1] < BucketSlots;
        const bool can_use_2 = load2_[b2] < BucketSlots;

        if (!can_use_1 && !can_use_2) {
            continue;
        }

        if (can_use_1 && (!can_use_2 || load1_[b1] <= load2_[b2])) {
            std::construct_at(bucket_slot(0, b1, load1_[b1]), std::move(*entry));
            ++load1_[b1];
        } else {
            std::construct_at(bucket_slot(1, b2, load2_[b2]), std::move(*entry));
            ++load2_[b2];
        }

        remove_stash_slot_without_changing_size(i);
        return true;
    }

    return false;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
void FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::remove_stash_slot_without_changing_size(
    std::size_t index
) {
    auto* current = stash_slot(index);
    const auto last = stash_size_ - 1;

    if (index != last) {
        auto* last_ptr = stash_slot(last);
        *current = std::move(*last_ptr);
        std::destroy_at(last_ptr);
    } else {
        std::destroy_at(current);
    }

    --stash_size_;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
void FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::destroy_all() noexcept {
    if (slots_.empty()) {
        size_ = 0;
        stash_size_ = 0;
        return;
    }

    for (std::size_t bucket = 0; bucket < config_.bucket_count; ++bucket) {
        for (std::size_t i = 0; i < load1_[bucket]; ++i) {
            std::destroy_at(bucket_slot(0, bucket, i));
        }
        for (std::size_t i = 0; i < load2_[bucket]; ++i) {
            std::destroy_at(bucket_slot(1, bucket, i));
        }
        load1_[bucket] = 0;
        load2_[bucket] = 0;
    }

    for (std::size_t i = 0; i < stash_size_; ++i) {
        std::destroy_at(stash_slot(i));
    }

    size_ = 0;
    stash_size_ = 0;
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
template <class Self, class F>
void FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::for_each_entry_impl(
    Self& self,
    F&& f
) {
    for (std::size_t bucket = 0; bucket < self.config_.bucket_count; ++bucket) {
        for (std::size_t i = 0; i < self.load1_[bucket]; ++i) {
            f(*self.bucket_slot(0, bucket, i));
        }
        for (std::size_t i = 0; i < self.load2_[bucket]; ++i) {
            f(*self.bucket_slot(1, bucket, i));
        }
    }

    for (std::size_t i = 0; i < self.stash_size_; ++i) {
        f(*self.stash_slot(i));
    }
}

template <class Key, class Value, std::size_t BucketSlots, class Hash, class Equal>
void FixedTwoLeftTable<Key, Value, BucketSlots, Hash, Equal>::move_from(
    FixedTwoLeftTable&& other
) noexcept {
    config_ = other.config_;
    hash_ = std::move(other.hash_);
    equal_ = std::move(other.equal_);
    slots_ = std::move(other.slots_);
    load1_ = std::move(other.load1_);
    load2_ = std::move(other.load2_);
    size_ = other.size_;
    stash_size_ = other.stash_size_;

    other.config_ = {};
    other.size_ = 0;
    other.stash_size_ = 0;
}

} // namespace twoleft::detail
