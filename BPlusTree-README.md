# In-memory B+ tree

`bplus_tree.hpp` provides a header-only C++17 `BPlusTree<Key, Value>`.
The complete tree stays in memory. It supports built-in fixed-size C++ types,
`std::string`, `std::wstring`, and custom types with a `BPlusTreeCodec` specialization.
Fanout is configurable when constructing
the tree, and must be at least 5; it is the maximum number of child links in an
internal node. Leaves hold up to `fanout - 1` key/value records and are linked
in both directions.

## Build

```sh
c++ -std=c++17 -Wall -Wextra -Wpedantic bplus_tree_example.cpp -o bpt_example
./bpt_example
```

## Iterators and ranges

```cpp
BPlusTree<std::uint64_t, std::uint64_t> tree(16);
tree.insert(10, 100);
tree.insert(20, 200);

for (auto it = tree.begin(); it != tree.end(); ++it) { /* ascending */ }
for (auto it = tree.rbegin(); it != tree.rend(); ++it) { /* descending */ }

auto [first, last] = tree.range(10, 20); // inclusive key bounds, ascending
for (auto it = first; it != last; ++it) { /* ... */ }

auto [rfirst, rlast] = tree.reverse_range(10, 20); // inclusive, descending
for (auto it = rfirst; it != rlast; ++it) { /* ... */ }

BPlusTree<std::string, std::wstring> text_tree(8);
text_tree.insert("greeting", L"Hello");
text_tree.save("text.bpt");
auto text_copy = BPlusTree<std::string, std::wstring>::load("text.bpt");
```

Iterators are read-only and bidirectional. Insertions can split leaves, so
discard existing iterators after inserting. `find(key, value_out)` performs
exact lookup; inserting an existing key replaces its value.

## Persistence

```cpp
tree.save("records.bpt");
auto restored = BPlusTree<std::uint64_t, std::uint64_t>::load("records.bpt");
```

The version 2 save file records the fanout, record count, and data offset
using 64-bit fields. Each key and value is stored as a 64-bit byte length
followed by its payload, so strings and other variable-size codecs are
supported. `save` writes sorted records; `load` rebuilds the complete
in-memory tree. File access uses `_fseeki64` on Windows and 64-bit
`fseeko`/`ftello` on POSIX for large-file support. Files beyond 2 GiB also
require a filesystem and C runtime configured for large files.

Built-in and other trivially-copyable values use their native byte
representation, so reopening those files requires compatible endianness and
ABI. `std::string` stores its exact bytes. `std::wstring` stores native
`wchar_t` code units, so it requires a compatible `wchar_t` representation.
For a custom type, specialize `BPlusTreeCodec<T>` with static `encode(value)`
returning `std::vector<unsigned char>` and `decode(bytes, size)` returning
`T`. This implementation has no erase operation, concurrent-writer locking,
transactions, or crash-recovery log.
