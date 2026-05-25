#pragma once

#include <cstddef>
#include <cstdint>

namespace twoleft {

struct TableConfig {
    std::size_t bucket_count = 16;   // normalized to power of two
    std::size_t stash_capacity = 16; // normalized to power of two
    std::uint64_t seed1 = 0;
    std::uint64_t seed2 = 0;
};

enum class InsertStatus {
    inserted_main_table,
    inserted_stash,
    already_present,
    full
};

} // namespace twoleft
