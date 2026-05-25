#include <iostream>
#include <string>

#include <twoleft/two_left_hash_map.hpp>

int main() {
    twoleft::TwoLeftHashMap<int, std::string> numbers;
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

    twoleft::TwoLeftHashMap<std::string, int> words;
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
