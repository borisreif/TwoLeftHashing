#pragma once

/**
 * @file table_config.hpp
 * @brief Configuration and small result types shared by the two table layers.
 */

#include <cstddef>
#include <cstdint>

namespace twoleft {

/**
 * @brief Configuration for one fixed table image.
 *
 * A `FixedTwoLeftTable` is one immutable-capacity storage image with this logical
 * layout:
 *
 * @code{.text}
 * | table 1 buckets | table 2 buckets | stash |
 * @endcode
 *
 * The dynamic wrapper creates new `TableConfig` values whenever it rebuilds the
 * table. `bucket_count` and `stash_capacity` are normalized to powers of two by
 * the fixed table constructor.
 */
struct TableConfig {
    /** @brief Number of buckets in each of the two main tables. */
    std::size_t bucket_count = 16;

    /** @brief Number of fallback slots in the stash. */
    std::size_t stash_capacity = 16;

    /** @brief Seed used to derive the first table's bucket index. */
    std::uint64_t seed1 = 0;

    /** @brief Seed used to derive the second table's bucket index. */
    std::uint64_t seed2 = 0;
};

/**
 * @brief Result of attempting to insert into one fixed-capacity table.
 *
 * The lower table never resizes itself. Instead, it reports what happened and the
 * outer `TwoLeftHashMap` decides whether to keep going, rehash with new seeds, or
 * grow to a larger fixed table.
 */
enum class InsertStatus {
    /** @brief The key/value pair was inserted into one of the two main tables. */
    inserted_main_table,

    /** @brief Both candidate buckets were full, so the entry was inserted into the stash. */
    inserted_stash,

    /** @brief The key already existed and was not inserted again. */
    already_present,

    /** @brief The fixed table could not accept the entry because the stash was full. */
    full
};

} // namespace twoleft
