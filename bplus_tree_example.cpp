#include "bplus_tree.hpp"

#include <cstdint>
#include <iostream>

int main() {
    BPlusTree<std::uint64_t, std::uint64_t> tree(8); // maximum 8 child links
    tree.insert(42, 4200);
    tree.insert(7, 700);
    tree.insert(99, 9900);

    std::cout << "Forward range [7, 42]:\n";
    const auto forward = tree.range(7, 42);
    for (auto it = forward.first; it != forward.second; ++it)
        std::cout << it->first << " -> " << it->second << '\n';

    std::cout << "Reverse range [7, 99]:\n";
    const auto reverse = tree.reverse_range(7, 99);
    for (auto it = reverse.first; it != reverse.second; ++it)
        std::cout << it->first << " -> " << it->second << '\n';

    tree.save("numbers.bpt");
    auto restored = BPlusTree<std::uint64_t, std::uint64_t>::load("numbers.bpt");
    std::cout << "Restored " << restored.size() << " records; fanout="
              << restored.fanout() << '\n';

    BPlusTree<std::string, std::wstring> labels(5);
    labels.insert("alpha", L"first label");
    labels.insert("beta", L"a longer label with variable-sized text");
    labels.insert("empty", L"");
    labels.save("labels.bpt");
    auto restored_labels = BPlusTree<std::string, std::wstring>::load("labels.bpt");
    std::wstring wide_value;
    if (restored_labels.find("beta", wide_value))
        std::wcout << L"Restored wide value: " << wide_value << L'\n';
}
