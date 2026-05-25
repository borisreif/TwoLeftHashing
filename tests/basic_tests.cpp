#include <cassert>
#include <string>

#include <twoleft/two_left_hash_map.hpp>

int main() {
    twoleft::TwoLeftHashMap<int, std::string> map;

    assert(map.insert(1, "one"));
    assert(!map.insert(1, "duplicate"));
    assert(map.contains(1));
    assert(map.at(1) == "one");

    assert(!map.insert_or_assign(1, "ONE"));
    assert(map.at(1) == "ONE");

    assert(map.insert(2, "two"));
    assert(map.erase(1));
    assert(!map.contains(1));
    assert(map.contains(2));
    assert(!map.erase(99));

    for (int i = 0; i < 10'000; ++i) {
        map.insert_or_assign(i, std::to_string(i));
    }

    for (int i = 0; i < 10'000; ++i) {
        assert(map.contains(i));
        assert(map.at(i) == std::to_string(i));
    }

    return 0;
}
