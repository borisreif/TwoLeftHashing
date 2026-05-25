// two_left_hash_map_simple.cpp
// C++20 implementation of a 2-left hash map using two architectural layers:
//
//   detail::FixedTwoLeftTable   - fixed-capacity table image
//   TwoLeftHashMap              - dynamic wrapper that owns growth/rebuild policy
//
// Storage layout inside FixedTwoLeftTable:
//
//   | table 1 buckets | table 2 buckets | stash |
//
// Build:
//   g++ -std=c++20 -O2 -Wall -Wextra -pedantic two_left_hash_map_simple.cpp && ./a.out

#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace twoleft {

// -----------------------------------------------------------------------------
// Small utilities
// -----------------------------------------------------------------------------

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
    // SplitMix64-style finalizer. We use it before masking with bucket_count - 1.
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

struct TableConfig {
    std::size_t bucket_count = 16;   // power of two
    std::size_t stash_capacity = 16; // power of two
    std::uint64_t seed1 = 0;
    std::uint64_t seed2 = 0;
};

enum class InsertStatus {
    inserted_main_table,
    inserted_stash,
    already_present,
    full
};

namespace detail {

// -----------------------------------------------------------------------------
// FixedTwoLeftTable
// -----------------------------------------------------------------------------
//
// This class never resizes. It only knows how to place, find, and erase entries
// inside one fixed table image. The outer TwoLeftHashMap decides when to rebuild.

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

public:
    explicit FixedTwoLeftTable(TableConfig config, Hash hash = {}, Equal equal = {})
        : config_(normalize_config(config)),
          hash_(std::move(hash)),
          equal_(std::move(equal)),
          slots_(total_slots_for(config_)),
          load1_(config_.bucket_count, 0),
          load2_(config_.bucket_count, 0) {}

    ~FixedTwoLeftTable() {
        destroy_all();
    }

    FixedTwoLeftTable(const FixedTwoLeftTable&) = delete;
    FixedTwoLeftTable& operator=(const FixedTwoLeftTable&) = delete;

    FixedTwoLeftTable(FixedTwoLeftTable&& other) noexcept {
        move_from(std::move(other));
    }

    FixedTwoLeftTable& operator=(FixedTwoLeftTable&& other) noexcept {
        if (this != &other) {
            destroy_all();
            move_from(std::move(other));
        }
        return *this;
    }

    InsertStatus try_insert(const value_type& entry) {
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

    Value* find(const Key& key) {
        if (auto* entry = find_entry(key)) {
            return &entry->second;
        }
        return nullptr;
    }

    const Value* find(const Key& key) const {
        if (auto* entry = find_entry(key)) {
            return &entry->second;
        }
        return nullptr;
    }

    bool contains(const Key& key) const {
        return find(key) != nullptr;
    }

    bool erase(const Key& key) {
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

    void clear() noexcept {
        destroy_all();
    }

    template <class F>
    void for_each_entry(F&& f) {
        for_each_entry_impl(*this, std::forward<F>(f));
    }

    template <class F>
    void for_each_entry(F&& f) const {
        for_each_entry_impl(*this, std::forward<F>(f));
    }

    std::size_t size() const noexcept {
        return size_;
    }

    bool empty() const noexcept {
        return size_ == 0;
    }

    std::size_t bucket_count() const noexcept {
        return config_.bucket_count;
    }

    std::size_t bucket_slots() const noexcept {
        return BucketSlots;
    }

    std::size_t main_capacity() const noexcept {
        return 2 * config_.bucket_count * BucketSlots;
    }

    std::size_t stash_capacity() const noexcept {
        return config_.stash_capacity;
    }

    std::size_t stash_size() const noexcept {
        return stash_size_;
    }

    double load_factor() const noexcept {
        return main_capacity() == 0
            ? 0.0
            : static_cast<double>(size_) / static_cast<double>(main_capacity());
    }

private:
    TableConfig config_{};
    Hash hash_{};
    Equal equal_{};
    std::vector<RawSlot> slots_{};
    std::vector<std::uint8_t> load1_{};
    std::vector<std::uint8_t> load2_{};
    std::size_t size_ = 0;
    std::size_t stash_size_ = 0;

    static TableConfig normalize_config(TableConfig config) {
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

    static std::size_t total_slots_for(const TableConfig& config) noexcept {
        return 2 * config.bucket_count * BucketSlots + config.stash_capacity;
    }

    std::uint64_t hash_with_seed(const Key& key, std::uint64_t seed) const {
        return mix64(static_cast<std::uint64_t>(hash_(key)) ^ seed);
    }

    std::size_t bucket1(const Key& key) const {
        return static_cast<std::size_t>(hash_with_seed(key, config_.seed1)) &
               (config_.bucket_count - 1);
    }

    std::size_t bucket2(const Key& key) const {
        return static_cast<std::size_t>(hash_with_seed(key, config_.seed2)) &
               (config_.bucket_count - 1);
    }

    std::size_t table_offset(int table) const noexcept {
        return table == 0 ? 0 : config_.bucket_count * BucketSlots;
    }

    std::size_t bucket_offset(int table, std::size_t bucket) const noexcept {
        return table_offset(table) + bucket * BucketSlots;
    }

    std::size_t stash_offset() const noexcept {
        return 2 * config_.bucket_count * BucketSlots;
    }

    value_type* bucket_slot(int table, std::size_t bucket, std::size_t index) {
        return slots_[bucket_offset(table, bucket) + index].ptr();
    }

    const value_type* bucket_slot(int table, std::size_t bucket, std::size_t index) const {
        return slots_[bucket_offset(table, bucket) + index].ptr();
    }

    value_type* stash_slot(std::size_t index) {
        return slots_[stash_offset() + index].ptr();
    }

    const value_type* stash_slot(std::size_t index) const {
        return slots_[stash_offset() + index].ptr();
    }

    void construct_in_bucket(int table, std::size_t bucket, std::size_t index, const value_type& entry) {
        std::construct_at(bucket_slot(table, bucket, index), entry);
    }

    void construct_in_stash(std::size_t index, const value_type& entry) {
        std::construct_at(stash_slot(index), entry);
    }

    value_type* find_entry(const Key& key) {
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

    const value_type* find_entry(const Key& key) const {
        return const_cast<FixedTwoLeftTable*>(this)->find_entry(key);
    }

    bool erase_from_bucket(int table, std::size_t bucket, const Key& key) {
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

    bool erase_from_stash(const Key& key) {
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

    void promote_from_stash_while_possible() {
        while (try_promote_one_from_stash()) {
            // Keep promoting until no stashed item fits in either of its two buckets.
        }
    }

    bool try_promote_one_from_stash() {
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

    void remove_stash_slot_without_changing_size(std::size_t index) {
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

    void destroy_all() noexcept {
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

    template <class Self, class F>
    static void for_each_entry_impl(Self& self, F&& f) {
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

    void move_from(FixedTwoLeftTable&& other) noexcept {
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
};

} // namespace detail

// -----------------------------------------------------------------------------
// TwoLeftHashMap
// -----------------------------------------------------------------------------
//
// This is the dynamic public wrapper. It owns the resize/rebuild policy. It
// replaces the fixed table with a freshly built one when necessary.

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

    bool insert(Key key, Value value) {
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

    bool insert_or_assign(Key key, Value value) {
        if (auto* existing = find(key)) {
            *existing = std::move(value);
            return false;
        }
        return insert(std::move(key), std::move(value));
    }

    Value* find(const Key& key) {
        return table_.find(key);
    }

    const Value* find(const Key& key) const {
        return table_.find(key);
    }

    bool contains(const Key& key) const {
        return table_.contains(key);
    }

    bool erase(const Key& key) {
        return table_.erase(key);
    }

    Value& at(const Key& key) {
        if (auto* value = find(key)) {
            return *value;
        }
        throw std::out_of_range("TwoLeftHashMap::at: key not found");
    }

    const Value& at(const Key& key) const {
        if (auto* value = find(key)) {
            return *value;
        }
        throw std::out_of_range("TwoLeftHashMap::at: key not found");
    }

    void clear() {
        table_.clear();
    }

    template <class F>
    void for_each_entry(F&& f) {
        table_.for_each_entry(std::forward<F>(f));
    }

    template <class F>
    void for_each_entry(F&& f) const {
        table_.for_each_entry(std::forward<F>(f));
    }

    std::size_t size() const noexcept {
        return table_.size();
    }

    bool empty() const noexcept {
        return table_.empty();
    }

    std::size_t bucket_count() const noexcept {
        return table_.bucket_count();
    }

    std::size_t bucket_slots() const noexcept {
        return table_.bucket_slots();
    }

    std::size_t main_capacity() const noexcept {
        return table_.main_capacity();
    }

    std::size_t stash_size() const noexcept {
        return table_.stash_size();
    }

    std::size_t stash_capacity() const noexcept {
        return table_.stash_capacity();
    }

    double load_factor() const noexcept {
        return table_.load_factor();
    }

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

    TableConfig make_config(std::size_t bucket_count) const {
        bucket_count = ceil_power_of_two(bucket_count);
        return TableConfig{
            .bucket_count = bucket_count,
            .stash_capacity = choose_stash_capacity(bucket_count),
            .seed1 = make_seed(),
            .seed2 = make_seed()
        };
    }

    std::size_t choose_stash_capacity(std::size_t bucket_count) const {
        // Tiny but scaling stash. This is wrapper policy, not fixed-table logic.
        const auto scaled = ceil_power_of_two(std::max<std::size_t>(8, bucket_count / 16));
        return std::max(min_stash_capacity_, scaled);
    }

    bool should_grow_before_insert() const noexcept {
        const auto projected = static_cast<double>(table_.size() + 1);
        const auto threshold = max_load_factor_ * static_cast<double>(table_.main_capacity());
        return projected > threshold;
    }

    std::size_t stash_pressure_threshold() const noexcept {
        return std::max<std::size_t>(1, table_.stash_capacity() / 2);
    }

    bool stash_pressure_is_high() const noexcept {
        return table_.stash_size() > stash_pressure_threshold();
    }

    bool satisfies_rebuild_goal(const fixed_table_type& candidate, RebuildGoal goal) const noexcept {
        if (goal == RebuildGoal::normal) {
            return true;
        }
        const auto threshold = std::max<std::size_t>(1, candidate.stash_capacity() / 2);
        return candidate.stash_size() <= threshold;
    }

    void rebuild(std::size_t desired_bucket_count, RebuildGoal goal) {
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
};

} // namespace twoleft

// -----------------------------------------------------------------------------
// Demo
// -----------------------------------------------------------------------------

int main() {
    using twoleft::TwoLeftHashMap;

    TwoLeftHashMap<int, std::string> numbers;
    numbers.insert(1, "one");
    numbers.insert(2, "two");
    numbers.insert(3, "three");
    numbers.insert_or_assign(2, "TWO");

    std::cout << "int -> string\n";
    for (int key : {1, 2, 3, 4}) {
        if (auto* value = numbers.find(key)) {
            std::cout << key << " -> " << *value << '\n';
        } else {
            std::cout << key << " not found\n";
        }
    }

    std::cout << "size=" << numbers.size()
              << " buckets=" << numbers.bucket_count()
              << " capacity=" << numbers.main_capacity()
              << " load=" << numbers.load_factor()
              << " stash=" << numbers.stash_size() << '/' << numbers.stash_capacity()
              << "\n\n";

    TwoLeftHashMap<std::string, int> words;
    words.insert("alpha", 1);
    words.insert("beta", 2);
    words.insert("gamma", 3);
    words.insert("delta", 4);
    words.erase("beta");

    std::cout << "string -> int\n";
    for (const auto& key : {std::string("alpha"), std::string("beta"), std::string("gamma")}) {
        if (auto* value = words.find(key)) {
            std::cout << key << " -> " << *value << '\n';
        } else {
            std::cout << key << " not found\n";
        }
    }

    std::cout << "\nall word entries:\n";
    words.for_each_entry([](const auto& entry) {
        std::cout << "  " << entry.first << " -> " << entry.second << '\n';
    });

    std::cout << "size=" << words.size()
              << " buckets=" << words.bucket_count()
              << " capacity=" << words.main_capacity()
              << " load=" << words.load_factor()
              << " stash=" << words.stash_size() << '/' << words.stash_capacity()
              << '\n';

    return 0;
}
