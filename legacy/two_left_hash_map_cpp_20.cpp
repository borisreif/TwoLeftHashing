// two_left_hash_map.cpp
// C++20 single-file implementation of a dynamically managed 2-left hash map.
//
// Architecture:
//   TwoLeftHashMap      - public dynamic container, owns rebuild/growth policy
//   FixedTwoLeftTable   - fixed-capacity table image: | table 1 | table 2 | stash |
//   NoLookupFilter      - extension point for future Bloom/quotient filters
//
// Build:
//   g++ -std=c++20 -O2 -Wall -Wextra -pedantic two_left_hash_map.cpp && ./a.out

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
#include <type_traits>
#include <utility>
#include <vector>

namespace tlh {

// -----------------------------------------------------------------------------
// Utilities
// -----------------------------------------------------------------------------

inline std::size_t ceil_power_of_two(std::size_t n) {
    if (n <= 1) {
        return 1;
    }
    return std::bit_ceil(n);
}

inline std::uint64_t make_seed() {
    static std::random_device rd;
    const auto a = static_cast<std::uint64_t>(rd());
    const auto b = static_cast<std::uint64_t>(rd());
    return (a << 32) ^ b;
}

inline std::uint64_t mix64(std::uint64_t x) noexcept {
    // SplitMix64-style finalizer. Useful because we later mask with bucket_count - 1.
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

// -----------------------------------------------------------------------------
// Lookup-filter extension point
// -----------------------------------------------------------------------------

struct NoLookupFilter {
    template <class Key>
    bool maybe_contains(const Key&) const noexcept {
        return true;
    }

    template <class Key>
    void add(const Key&) noexcept {}

    void clear() noexcept {}
};

// -----------------------------------------------------------------------------
// Fixed table config and result types
// -----------------------------------------------------------------------------

struct TwoLeftConfig {
    std::size_t bucket_count = 16;     // power of two
    std::size_t stash_capacity = 16;   // power of two
    std::uint64_t seed1 = 0;
    std::uint64_t seed2 = 0;
};

enum class InsertStatus {
    inserted_main_table,
    inserted_stash,
    already_present,
    full
};

// -----------------------------------------------------------------------------
// FixedTwoLeftTable
// -----------------------------------------------------------------------------
//
// This class never resizes. It owns one contiguous raw slot array laid out as:
//
//   | table 1 buckets | table 2 buckets | stash |
//
// Each bucket contains BucketSlots physical slots. The occupied part of a bucket
// is always compact: [0, load). Therefore erase can remove an item by moving the
// last occupied slot in that bucket into the removed position.
//
// This implementation is intentionally exact and simple. It does not know about
// max load factor, growth policy, or rebuild policy.

template <
    class Key,
    class Mapped,
    std::size_t BucketSlots = 4,
    class Hash = std::hash<Key>,
    class Eq = std::equal_to<Key>
>
class FixedTwoLeftTable {
    static_assert(BucketSlots > 0, "BucketSlots must be positive");
    static_assert(BucketSlots <= 255, "BucketSlots must fit in std::uint8_t loads");

public:
    using key_type = Key;
    using mapped_type = Mapped;
    using value_type = std::pair<Key, Mapped>;

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
    FixedTwoLeftTable(TwoLeftConfig config, Hash hash = {}, Eq eq = {})
        : config_(normalize_and_validate(config)),
          hash_(std::move(hash)),
          eq_(std::move(eq)),
          slots_(total_slot_count()),
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

        // If the selected bucket is full, the other candidate bucket must also
        // be full under the 2-left load-choice rule.
        if (stash_size_ < config_.stash_capacity) {
            construct_in_stash(stash_size_, entry);
            ++stash_size_;
            ++size_;
            return InsertStatus::inserted_stash;
        }

        return InsertStatus::full;
    }

    Mapped* find(const Key& key) {
        if (auto* p = find_value(key)) {
            return &p->second;
        }
        return nullptr;
    }

    const Mapped* find(const Key& key) const {
        if (auto* p = find_value(key)) {
            return &p->second;
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

    std::size_t stash_capacity() const noexcept {
        return config_.stash_capacity;
    }

    std::size_t stash_size() const noexcept {
        return stash_size_;
    }

    std::size_t main_capacity() const noexcept {
        return 2 * config_.bucket_count * BucketSlots;
    }

    std::size_t total_capacity() const noexcept {
        return main_capacity() + config_.stash_capacity;
    }

    double load_factor() const noexcept {
        if (main_capacity() == 0) {
            return 0.0;
        }
        return static_cast<double>(size_) / static_cast<double>(main_capacity());
    }

    const TwoLeftConfig& config() const noexcept {
        return config_;
    }

private:
    TwoLeftConfig config_{};
    Hash hash_{};
    Eq eq_{};

    std::vector<RawSlot> slots_{};
    std::vector<std::uint8_t> load1_{};
    std::vector<std::uint8_t> load2_{};

    std::size_t size_ = 0;
    std::size_t stash_size_ = 0;

    static TwoLeftConfig normalize_and_validate(TwoLeftConfig cfg) {
        cfg.bucket_count = ceil_power_of_two(cfg.bucket_count);
        cfg.stash_capacity = ceil_power_of_two(cfg.stash_capacity);

        if (!std::has_single_bit(cfg.bucket_count)) {
            throw std::invalid_argument("bucket_count must be a power of two");
        }

        if (!std::has_single_bit(cfg.stash_capacity)) {
            throw std::invalid_argument("stash_capacity must be a power of two");
        }

        if (cfg.seed1 == 0) {
            cfg.seed1 = make_seed();
        }
        if (cfg.seed2 == 0) {
            cfg.seed2 = make_seed();
        }
        if (cfg.seed1 == cfg.seed2) {
            cfg.seed2 = mix64(cfg.seed2 + 1);
        }

        return cfg;
    }

    std::size_t total_slot_count() const noexcept {
        return 2 * config_.bucket_count * BucketSlots + config_.stash_capacity;
    }

    std::uint64_t mixed_hash(const Key& key, std::uint64_t seed) const {
        const auto h = static_cast<std::uint64_t>(hash_(key));
        return mix64(h ^ seed);
    }

    std::size_t bucket1(const Key& key) const {
        return static_cast<std::size_t>(mixed_hash(key, config_.seed1)) &
               (config_.bucket_count - 1);
    }

    std::size_t bucket2(const Key& key) const {
        return static_cast<std::size_t>(mixed_hash(key, config_.seed2)) &
               (config_.bucket_count - 1);
    }

    std::size_t table_offset(int table_index) const noexcept {
        return table_index == 0 ? 0 : config_.bucket_count * BucketSlots;
    }

    std::size_t bucket_offset(int table_index, std::size_t bucket) const noexcept {
        return table_offset(table_index) + bucket * BucketSlots;
    }

    std::size_t stash_offset() const noexcept {
        return 2 * config_.bucket_count * BucketSlots;
    }

    value_type* bucket_slot(int table_index, std::size_t bucket, std::size_t index) {
        return slots_[bucket_offset(table_index, bucket) + index].ptr();
    }

    const value_type* bucket_slot(int table_index, std::size_t bucket, std::size_t index) const {
        return slots_[bucket_offset(table_index, bucket) + index].ptr();
    }

    value_type* stash_slot(std::size_t index) {
        return slots_[stash_offset() + index].ptr();
    }

    const value_type* stash_slot(std::size_t index) const {
        return slots_[stash_offset() + index].ptr();
    }

    void construct_in_bucket(
        int table_index,
        std::size_t bucket,
        std::size_t index,
        const value_type& entry
    ) {
        std::construct_at(bucket_slot(table_index, bucket, index), entry);
    }

    void construct_in_stash(std::size_t index, const value_type& entry) {
        std::construct_at(stash_slot(index), entry);
    }

    value_type* find_value(const Key& key) {
        const auto b1 = bucket1(key);
        for (std::size_t i = 0; i < load1_[b1]; ++i) {
            auto* p = bucket_slot(0, b1, i);
            if (eq_(p->first, key)) {
                return p;
            }
        }

        const auto b2 = bucket2(key);
        for (std::size_t i = 0; i < load2_[b2]; ++i) {
            auto* p = bucket_slot(1, b2, i);
            if (eq_(p->first, key)) {
                return p;
            }
        }

        for (std::size_t i = 0; i < stash_size_; ++i) {
            auto* p = stash_slot(i);
            if (eq_(p->first, key)) {
                return p;
            }
        }

        return nullptr;
    }

    const value_type* find_value(const Key& key) const {
        return const_cast<FixedTwoLeftTable*>(this)->find_value(key);
    }

    bool erase_from_bucket(int table_index, std::size_t bucket, const Key& key) {
        auto& load = table_index == 0 ? load1_[bucket] : load2_[bucket];

        for (std::size_t i = 0; i < load; ++i) {
            auto* current = bucket_slot(table_index, bucket, i);
            if (!eq_(current->first, key)) {
                continue;
            }

            const auto last = static_cast<std::size_t>(load - 1);
            if (i != last) {
                auto* last_ptr = bucket_slot(table_index, bucket, last);
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
            if (!eq_(current->first, key)) {
                continue;
            }

            const auto last = stash_size_ - 1;
            if (i != last) {
                auto* last_ptr = stash_slot(last);
                *current = std::move(*last_ptr);
                std::destroy_at(last_ptr);
            } else {
                std::destroy_at(current);
            }

            --stash_size_;
            --size_;
            return true;
        }

        return false;
    }

    void promote_from_stash_while_possible() {
        while (try_promote_one_from_stash()) {
            // Keep promoting until no stashed item fits into either candidate bucket.
        }
    }

    bool try_promote_one_from_stash() {
        for (std::size_t i = 0; i < stash_size_; ++i) {
            auto* entry = stash_slot(i);
            const auto b1 = bucket1(entry->first);
            const auto b2 = bucket2(entry->first);

            const bool can_use_table1 = load1_[b1] < BucketSlots;
            const bool can_use_table2 = load2_[b2] < BucketSlots;

            if (!can_use_table1 && !can_use_table2) {
                continue;
            }

            if (can_use_table1 && (!can_use_table2 || load1_[b1] <= load2_[b2])) {
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

        std::destroy_at(current);

        if (index != last) {
            auto* last_ptr = stash_slot(last);
            std::construct_at(current, std::move(*last_ptr));
            std::destroy_at(last_ptr);
        }

        --stash_size_;
    }

    void destroy_all() noexcept {
        if (slots_.empty() || config_.bucket_count == 0) {
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
        eq_ = std::move(other.eq_);
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

// -----------------------------------------------------------------------------
// TwoLeftHashMap
// -----------------------------------------------------------------------------
//
// Public dynamic wrapper. This class owns resizing/rebuilding policy. The fixed
// table below it never reallocates itself.

template <
    class Key,
    class Mapped,
    std::size_t BucketSlots = 4,
    class Hash = std::hash<Key>,
    class Eq = std::equal_to<Key>,
    class LookupFilter = NoLookupFilter
>
class TwoLeftHashMap {
public:
    using key_type = Key;
    using mapped_type = Mapped;
    using value_type = std::pair<Key, Mapped>;
    using table_type = FixedTwoLeftTable<Key, Mapped, BucketSlots, Hash, Eq>;

    explicit TwoLeftHashMap(
        std::size_t initial_bucket_count = 16,
        std::size_t minimum_stash_capacity = 16,
        double max_load_factor = 0.80,
        Hash hash = {},
        Eq eq = {},
        LookupFilter filter = {}
    )
        : hash_(std::move(hash)),
          eq_(std::move(eq)),
          minimum_stash_capacity_(ceil_power_of_two(minimum_stash_capacity)),
          max_load_factor_(max_load_factor),
          table_(make_config(ceil_power_of_two(initial_bucket_count)), hash_, eq_),
          filter_(std::move(filter)) {
        if (!(max_load_factor_ > 0.0 && max_load_factor_ < 1.0)) {
            throw std::invalid_argument("max_load_factor must be in (0, 1)");
        }
    }

    bool insert(Key key, Mapped value) {
        value_type entry{std::move(key), std::move(value)};

        for (;;) {
            if (should_grow_before_insert()) {
                rebuild(table_.bucket_count() * 2, RebuildGoal::normal);
            }

            const auto status = table_.try_insert(entry);

            switch (status) {
                case InsertStatus::inserted_main_table:
                    filter_.add(entry.first);
                    return true;

                case InsertStatus::inserted_stash:
                    filter_.add(entry.first);
                    if (stash_pressure_is_high()) {
                        // Same size, new seeds first. If that does not reduce
                        // stash pressure, rebuild() will grow.
                        rebuild(table_.bucket_count(), RebuildGoal::reduce_stash_pressure);
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

    bool insert_or_assign(Key key, Mapped value) {
        if (auto* p = find(key)) {
            *p = std::move(value);
            return false;
        }
        return insert(std::move(key), std::move(value));
    }

    Mapped* find(const Key& key) {
        if (!filter_.maybe_contains(key)) {
            return nullptr;
        }
        return table_.find(key);
    }

    const Mapped* find(const Key& key) const {
        if (!filter_.maybe_contains(key)) {
            return nullptr;
        }
        return table_.find(key);
    }

    bool contains(const Key& key) const {
        return find(key) != nullptr;
    }

    bool erase(const Key& key) {
        // NoLookupFilter does nothing. A future Bloom filter may choose not to
        // support exact deletion and simply accumulate stale positives until the
        // next rebuild. Correctness is still preserved because the fixed table is
        // the source of truth.
        const bool removed = table_.erase(key);
        if (removed && erased_since_filter_rebuild_ < static_cast<std::size_t>(-1)) {
            ++erased_since_filter_rebuild_;
        }

        if (removed && should_rebuild_filter_after_erases()) {
            rebuild_filter_from_table();
        }

        return removed;
    }

    Mapped& at(const Key& key) {
        if (auto* p = find(key)) {
            return *p;
        }
        throw std::out_of_range("TwoLeftHashMap::at: key not found");
    }

    const Mapped& at(const Key& key) const {
        if (auto* p = find(key)) {
            return *p;
        }
        throw std::out_of_range("TwoLeftHashMap::at: key not found");
    }

    void clear() {
        table_.clear();
        filter_.clear();
        erased_since_filter_rebuild_ = 0;
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
        return BucketSlots;
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
        reduce_stash_pressure
    };

    Hash hash_{};
    Eq eq_{};
    std::size_t minimum_stash_capacity_ = 16;
    double max_load_factor_ = 0.80;
    table_type table_;
    LookupFilter filter_{};
    std::size_t erased_since_filter_rebuild_ = 0;

    TwoLeftConfig make_config(std::size_t bucket_count) const {
        bucket_count = ceil_power_of_two(bucket_count);

        return TwoLeftConfig{
            .bucket_count = bucket_count,
            .stash_capacity = choose_stash_capacity(bucket_count),
            .seed1 = make_seed(),
            .seed2 = make_seed()
        };
    }

    std::size_t choose_stash_capacity(std::size_t bucket_count) const {
        // Keep stash small, but let it scale gently. This is policy, not table logic.
        const auto scaled = std::max<std::size_t>(8, bucket_count / 16);
        return ceil_power_of_two(std::max(minimum_stash_capacity_, scaled));
    }

    bool should_grow_before_insert() const noexcept {
        const auto projected_size = static_cast<double>(table_.size() + 1);
        const auto threshold = max_load_factor_ * static_cast<double>(table_.main_capacity());
        return projected_size > threshold;
    }

    std::size_t stash_pressure_threshold() const noexcept {
        return std::max<std::size_t>(1, table_.stash_capacity() / 2);
    }

    bool stash_pressure_is_high() const noexcept {
        return table_.stash_size() > stash_pressure_threshold();
    }

    bool should_rebuild_filter_after_erases() const noexcept {
        // This only matters for approximate filters that cannot delete exactly.
        // For NoLookupFilter, rebuild_filter_from_table() is a no-op.
        return erased_since_filter_rebuild_ > 0 &&
               erased_since_filter_rebuild_ > table_.size() / 2;
    }

    bool table_satisfies_goal(const table_type& candidate, RebuildGoal goal) const noexcept {
        if (goal == RebuildGoal::normal) {
            return true;
        }

        const auto threshold = std::max<std::size_t>(1, candidate.stash_capacity() / 2);
        return candidate.stash_size() <= threshold;
    }

    void rebuild(std::size_t desired_bucket_count, RebuildGoal goal) {
        desired_bucket_count = ceil_power_of_two(std::max<std::size_t>(1, desired_bucket_count));

        for (;;) {
            constexpr int attempts_per_size = 4;

            for (int attempt = 0; attempt < attempts_per_size; ++attempt) {
                auto fresh = table_type(make_config(desired_bucket_count), hash_, eq_);
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

                if (ok && table_satisfies_goal(fresh, goal)) {
                    table_ = std::move(fresh);
                    rebuild_filter_from_table();
                    return;
                }
            }

            // Several new seed pairs failed at this size. Grow and try again.
            desired_bucket_count *= 2;
        }
    }

    void rebuild_filter_from_table() {
        filter_.clear();
        table_.for_each_entry([&](const value_type& entry) {
            filter_.add(entry.first);
        });
        erased_since_filter_rebuild_ = 0;
    }
};

} // namespace tlh

// -----------------------------------------------------------------------------
// Demo
// -----------------------------------------------------------------------------

int main() {
    using tlh::TwoLeftHashMap;

    std::cout << "int -> std::string map\n";

    TwoLeftHashMap<int, std::string> numbers;
    numbers.insert(1, "one");
    numbers.insert(2, "two");
    numbers.insert(3, "three");
    numbers.insert_or_assign(2, "TWO");

    if (auto* value = numbers.find(2)) {
        std::cout << "2 = " << *value << '\n';
    }

    std::cout << "contains 4? " << std::boolalpha << numbers.contains(4) << '\n';
    std::cout << "size = " << numbers.size()
              << ", buckets = " << numbers.bucket_count()
              << ", main_capacity = " << numbers.main_capacity()
              << ", load_factor = " << numbers.load_factor()
              << ", stash = " << numbers.stash_size() << '/' << numbers.stash_capacity()
              << "\n\n";

    std::cout << "std::string -> int map\n";

    TwoLeftHashMap<std::string, int> words;
    words.insert("alpha", 1);
    words.insert("beta", 2);
    words.insert("gamma", 3);
    words.insert("delta", 4);

    words.erase("beta");

    for (const auto& key : {std::string("alpha"), std::string("beta"), std::string("gamma")}) {
        if (auto* value = words.find(key)) {
            std::cout << key << " = " << *value << '\n';
        } else {
            std::cout << key << " not found\n";
        }
    }

    std::cout << "\nall entries:\n";
    words.for_each_entry([](const auto& entry) {
        std::cout << "  " << entry.first << " -> " << entry.second << '\n';
    });

    std::cout << "\nsize = " << words.size()
              << ", buckets = " << words.bucket_count()
              << ", main_capacity = " << words.main_capacity()
              << ", load_factor = " << words.load_factor()
              << ", stash = " << words.stash_size() << '/' << words.stash_capacity()
              << '\n';

    return 0;
}
