#pragma once

// In-memory B+ tree with bidirectional iterators and large-file save/load.
// C++17; built-in/trivially-copyable types, std::string, std::wstring, and
// types with a BPlusTreeCodec specialization can be used as keys and values.

#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#  define _FILE_OFFSET_BITS 64
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#  include <io.h>
#else
#  include <sys/types.h>
static_assert(sizeof(off_t) >= 8, "BPlusTree requires 64-bit POSIX file offsets");
#endif

// Serialization customization point. Specialize BPlusTreeCodec<T> for custom
// types; encode returns their byte payload and decode reconstructs the value.
template<class T, class Enable = void>
struct BPlusTreeCodec {
    static_assert(std::is_trivially_copyable<T>::value,
                  "Provide BPlusTreeCodec<T> for non-trivially-copyable types");
    static std::vector<unsigned char> encode(const T& value) {
        std::vector<unsigned char> bytes(sizeof(T));
        std::memcpy(bytes.data(), &value, sizeof(T));
        return bytes;
    }
    static T decode(const unsigned char* bytes, std::size_t size) {
        if (size != sizeof(T)) throw std::runtime_error("invalid fixed-size B+ tree value");
        T value{};
        std::memcpy(&value, bytes, sizeof(T));
        return value;
    }
};

template<>
struct BPlusTreeCodec<std::string> {
    static std::vector<unsigned char> encode(const std::string& value) {
        return std::vector<unsigned char>(value.begin(), value.end());
    }
    static std::string decode(const unsigned char* bytes, std::size_t size) {
        if (size == 0) return {};
        return std::string(reinterpret_cast<const char*>(bytes), size);
    }
};

template<>
struct BPlusTreeCodec<std::wstring> {
    static std::vector<unsigned char> encode(const std::wstring& value) {
        std::vector<unsigned char> bytes(value.size() * sizeof(wchar_t));
        if (!bytes.empty()) std::memcpy(bytes.data(), value.data(), bytes.size());
        return bytes;
    }
    static std::wstring decode(const unsigned char* bytes, std::size_t size) {
        if (size % sizeof(wchar_t) != 0)
            throw std::runtime_error("invalid std::wstring payload in B+ tree file");
        std::wstring value(size / sizeof(wchar_t), L'\0');
        if (size) std::memcpy(&value[0], bytes, size);
        return value;
    }
};

template<class Key, class Value, class Compare = std::less<Key>>
class BPlusTree {
    struct Node {
        bool leaf = true;
        std::vector<Key> separators;
        std::vector<std::pair<Key, Value>> entries;
        std::vector<std::unique_ptr<Node>> children;
        Node* next = nullptr;
        Node* previous = nullptr;
    };
    struct Split {
        std::unique_ptr<Node> right;
        Key separator{};
        bool inserted = false;
    };
    struct FileHeader {
        char magic[8];
        std::uint32_t version;
        std::uint32_t fanout;
        std::uint64_t count;
        std::uint64_t data_offset;
        std::uint64_t record_format;
    };

    static constexpr std::uint64_t kHeaderBytes = 64;
    static constexpr std::uint32_t kFileVersion = 2;
    static constexpr char kMagic[8] = {'B','P','T','R','E','E','0','2'};

public:
    using value_type = std::pair<Key, Value>;

    // These read-only bidirectional iterators traverse the linked leaf pages.
    // Insertion can split leaves and invalidates all existing iterators.
    class const_iterator {
        friend class BPlusTree;
        const BPlusTree* tree_ = nullptr;
        const Node* leaf_ = nullptr;
        std::size_t index_ = 0;
        const_iterator(const BPlusTree* tree, const Node* leaf, std::size_t index)
            : tree_(tree), leaf_(leaf), index_(index) {}
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = std::pair<Key, Value>;
        using difference_type = std::ptrdiff_t;
        using pointer = const value_type*;
        using reference = const value_type&;

        const_iterator() = default;
        reference operator*() const { return leaf_->entries[index_]; }
        pointer operator->() const { return &leaf_->entries[index_]; }
        const_iterator& operator++() {
            if (!leaf_) return *this;
            if (index_ + 1 < leaf_->entries.size()) ++index_;
            else { leaf_ = leaf_->next; index_ = 0; }
            return *this;
        }
        const_iterator operator++(int) { auto copy = *this; ++*this; return copy; }
        const_iterator& operator--() {
            if (!tree_) return *this;
            if (!leaf_) {
                leaf_ = tree_->tail_;
                if (leaf_) index_ = leaf_->entries.size() - 1;
            } else if (index_) --index_;
            else if (leaf_->previous) {
                leaf_ = leaf_->previous;
                index_ = leaf_->entries.size() - 1;
            }
            return *this;
        }
        const_iterator operator--(int) { auto copy = *this; --*this; return copy; }
        friend bool operator==(const const_iterator& a, const const_iterator& b) {
            return a.tree_ == b.tree_ && a.leaf_ == b.leaf_ &&
                   (!a.leaf_ || a.index_ == b.index_);
        }
        friend bool operator!=(const const_iterator& a, const const_iterator& b) {
            return !(a == b);
        }
    };

    using iterator = const_iterator; // Keys remain immutable through iterators.
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    explicit BPlusTree(std::uint32_t fanout, Compare compare = Compare())
        : fanout_(fanout), compare_(std::move(compare)), root_(std::make_unique<Node>()) {
        if (fanout_ < 5) throw std::invalid_argument("B+ tree fanout must be at least 5");
        if (fanout_ > 1000000) throw std::invalid_argument("B+ tree fanout is unreasonably large");
        head_ = tail_ = root_.get();
    }

    BPlusTree(const BPlusTree&) = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;
    BPlusTree(BPlusTree&&) noexcept = default;
    BPlusTree& operator=(BPlusTree&&) noexcept = default;

    std::uint32_t fanout() const noexcept { return fanout_; }
    std::uint64_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }

    iterator begin() noexcept { return iterator(this, count_ ? head_ : nullptr, 0); }
    iterator end() noexcept { return iterator(this, nullptr, 0); }
    const_iterator begin() const noexcept { return cbegin(); }
    const_iterator end() const noexcept { return cend(); }
    const_iterator cbegin() const noexcept { return const_iterator(this, count_ ? head_ : nullptr, 0); }
    const_iterator cend() const noexcept { return const_iterator(this, nullptr, 0); }
    reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(cend()); }
    const_reverse_iterator rend() const noexcept { return const_reverse_iterator(cbegin()); }
    const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }
    const_reverse_iterator crend() const noexcept { return const_reverse_iterator(cbegin()); }

    // Insert a key/value or replace the value for a key already present.
    void insert(const Key& key, const Value& value) {
        Split split = insert_into(root_.get(), key, value);
        if (split.inserted) ++count_;
        if (split.right) {
            auto new_root = std::make_unique<Node>();
            new_root->leaf = false;
            new_root->children.push_back(std::move(root_));
            new_root->children.push_back(std::move(split.right));
            rebuild_separators(new_root.get());
            root_ = std::move(new_root);
        }
    }

    bool find(const Key& key, Value& out) const {
        const Node* node = descend(key);
        auto it = lower_entry(node, key);
        if (it == node->entries.end() || !equal(it->first, key)) return false;
        out = it->second;
        return true;
    }

    iterator lower_bound(const Key& key) {
        const Node* leaf = descend(key);
        auto it = std::lower_bound(leaf->entries.begin(), leaf->entries.end(), key,
            [this](const value_type& entry, const Key& k) { return compare_(entry.first, k); });
        if (it == leaf->entries.end()) return iterator(this, leaf->next, 0);
        return iterator(this, leaf, static_cast<std::size_t>(it - leaf->entries.begin()));
    }
    const_iterator lower_bound(const Key& key) const {
        const Node* leaf = descend(key);
        auto it = std::lower_bound(leaf->entries.begin(), leaf->entries.end(), key,
            [this](const value_type& entry, const Key& k) { return compare_(entry.first, k); });
        if (it == leaf->entries.end()) return const_iterator(this, leaf->next, 0);
        return const_iterator(this, leaf, static_cast<std::size_t>(it - leaf->entries.begin()));
    }
    iterator upper_bound(const Key& key) {
        const Node* leaf = descend(key);
        auto it = std::upper_bound(leaf->entries.begin(), leaf->entries.end(), key,
            [this](const Key& k, const value_type& entry) { return compare_(k, entry.first); });
        if (it == leaf->entries.end()) return iterator(this, leaf->next, 0);
        return iterator(this, leaf, static_cast<std::size_t>(it - leaf->entries.begin()));
    }
    const_iterator upper_bound(const Key& key) const {
        const Node* leaf = descend(key);
        auto it = std::upper_bound(leaf->entries.begin(), leaf->entries.end(), key,
            [this](const Key& k, const value_type& entry) { return compare_(k, entry.first); });
        if (it == leaf->entries.end()) return const_iterator(this, leaf->next, 0);
        return const_iterator(this, leaf, static_cast<std::size_t>(it - leaf->entries.begin()));
    }

    // Inclusive forward range: iterate [first, second) while key <= high.
    std::pair<iterator, iterator> range(const Key& low, const Key& high) {
        if (compare_(high, low)) return {end(), end()};
        return {lower_bound(low), upper_bound(high)};
    }
    std::pair<const_iterator, const_iterator> range(const Key& low, const Key& high) const {
        if (compare_(high, low)) return {cend(), cend()};
        return {lower_bound(low), upper_bound(high)};
    }

    // Inclusive reverse range, in descending key order.
    std::pair<reverse_iterator, reverse_iterator> reverse_range(const Key& low, const Key& high) {
        if (compare_(high, low)) return {rend(), rend()};
        return {reverse_iterator(upper_bound(high)), reverse_iterator(lower_bound(low))};
    }
    std::pair<const_reverse_iterator, const_reverse_iterator>
    reverse_range(const Key& low, const Key& high) const {
        if (compare_(high, low)) return {crend(), crend()};
        return {const_reverse_iterator(upper_bound(high)), const_reverse_iterator(lower_bound(low))};
    }

    // Writes sorted, length-prefixed key/value payloads. The 64-bit lengths,
    // counts, and file offsets allow variable-size records and large files.
    void save(const std::string& path) const {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        if (!file) throw file_error("cannot create save file");
        try {
            FileHeader header{};
            std::memcpy(header.magic, kMagic, sizeof(kMagic));
            header.version = kFileVersion;
            header.fanout = fanout_;
            header.count = count_;
            header.data_offset = kHeaderBytes;
            header.record_format = 1; // uint64 key length, key bytes, uint64 value length, value bytes
            static_assert(sizeof(FileHeader) <= kHeaderBytes, "file header too large");
            unsigned char block[kHeaderBytes]{};
            std::memcpy(block, &header, sizeof(header));
            write_exact(file, block, sizeof(block));
            for (auto it = cbegin(); it != cend(); ++it) {
                write_value(file, it->first);
                write_value(file, it->second);
            }
            if (std::fflush(file) != 0) throw file_error("save flush failed");
            const int close_result = std::fclose(file);
            file = nullptr;
            if (close_result != 0) throw file_error("save close failed");
        } catch (...) {
            if (file) std::fclose(file);
            throw;
        }
    }

    static BPlusTree load(const std::string& path, Compare compare = Compare()) {
        std::FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) throw file_error("cannot open save file");
        try {
            unsigned char block[kHeaderBytes]{};
            read_exact(file, block, sizeof(block));
            FileHeader header{};
            std::memcpy(&header, block, sizeof(header));
            if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 ||
                header.version != kFileVersion || header.fanout < 5 ||
                header.data_offset != kHeaderBytes || header.record_format != 1)
                throw std::runtime_error("unsupported or incompatible B+ tree save file");
            seek64(file, 0, SEEK_END);
            const std::uint64_t actual_size = tell64(file);
            if (actual_size < header.data_offset || header.count > (actual_size - header.data_offset) / 16)
                throw std::runtime_error("invalid B+ tree record count or file size");
            seek64(file, header.data_offset, SEEK_SET);
            BPlusTree tree(header.fanout, std::move(compare));
            std::uint64_t remaining = actual_size - header.data_offset;
            std::optional<Key> previous_key;
            for (std::uint64_t i = 0; i < header.count; ++i) {
                Key key = read_value<Key>(file, remaining);
                Value value = read_value<Value>(file, remaining);
                if (previous_key && !tree.compare_(*previous_key, key)) {
                    throw std::runtime_error("keys in B+ tree save file are not strictly ordered");
                }
                tree.insert(key, value);
                previous_key = key;
            }
            if (remaining != 0) throw std::runtime_error("trailing data in B+ tree save file");
            std::fclose(file);
            file = nullptr;
            return tree;
        } catch (...) {
            if (file) std::fclose(file);
            throw;
        }
    }

private:
    std::uint32_t fanout_;
    Compare compare_;
    std::unique_ptr<Node> root_;
    Node* head_ = nullptr;
    Node* tail_ = nullptr;
    std::uint64_t count_ = 0;

    static std::runtime_error file_error(const char* message) {
        return std::runtime_error(std::string("BPlusTree: ") + message);
    }
    static void write_exact(std::FILE* file, const void* data, std::size_t bytes) {
        if (std::fwrite(data, 1, bytes, file) != bytes) throw file_error("write failed");
    }
    static void read_exact(std::FILE* file, void* data, std::size_t bytes) {
        if (std::fread(data, 1, bytes, file) != bytes) throw file_error("short read");
    }
    template<class T>
    static void write_value(std::FILE* file, const T& value) {
        const auto bytes = BPlusTreeCodec<T>::encode(value);
        const std::uint64_t length = static_cast<std::uint64_t>(bytes.size());
        write_exact(file, &length, sizeof(length));
        if (!bytes.empty()) write_exact(file, bytes.data(), bytes.size());
    }
    template<class T>
    static T read_value(std::FILE* file, std::uint64_t& remaining) {
        if (remaining < sizeof(std::uint64_t)) throw file_error("truncated variable-size record");
        std::uint64_t length = 0;
        read_exact(file, &length, sizeof(length));
        remaining -= sizeof(length);
        if (length > remaining || length > std::numeric_limits<std::size_t>::max())
            throw file_error("invalid variable-size record length");
        std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
        if (!bytes.empty()) read_exact(file, bytes.data(), bytes.size());
        remaining -= length;
        return BPlusTreeCodec<T>::decode(bytes.data(), bytes.size());
    }
    static void seek64(std::FILE* file, std::uint64_t offset, int origin) {
#ifdef _WIN32
        if (_fseeki64(file, static_cast<__int64>(offset), origin) != 0)
#else
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
            fseeko(file, static_cast<off_t>(offset), origin) != 0)
#endif
            throw file_error("64-bit file seek failed");
    }
    static std::uint64_t tell64(std::FILE* file) {
#ifdef _WIN32
        const auto pos = _ftelli64(file);
#else
        const auto pos = ftello(file);
#endif
        if (pos < 0) throw file_error("64-bit file tell failed");
        return static_cast<std::uint64_t>(pos);
    }

    bool equal(const Key& a, const Key& b) const { return !compare_(a, b) && !compare_(b, a); }
    const Node* descend(const Key& key) const {
        const Node* node = root_.get();
        while (!node->leaf) {
            const auto it = std::upper_bound(node->separators.begin(), node->separators.end(), key, compare_);
            node = node->children[static_cast<std::size_t>(it - node->separators.begin())].get();
        }
        return node;
    }
    auto lower_entry(Node* leaf, const Key& key) const {
        return std::lower_bound(leaf->entries.begin(), leaf->entries.end(), key,
            [this](const value_type& entry, const Key& k) { return compare_(entry.first, k); });
    }
    auto lower_entry(const Node* leaf, const Key& key) const {
        return std::lower_bound(leaf->entries.begin(), leaf->entries.end(), key,
            [this](const value_type& entry, const Key& k) { return compare_(entry.first, k); });
    }

    Split insert_into(Node* node, const Key& key, const Value& value) {
        if (node->leaf) {
            auto it = lower_entry(node, key);
            if (it != node->entries.end() && equal(it->first, key)) {
                it->second = value;
                return Split{nullptr, Key{}, false};
            }
            node->entries.insert(it, value_type(key, value));
            if (node->entries.size() < fanout_) return Split{nullptr, Key{}, true};
            const std::size_t left_size = fanout_ / 2;
            auto right = std::make_unique<Node>();
            right->leaf = true;
            right->entries.assign(std::make_move_iterator(node->entries.begin() + static_cast<std::ptrdiff_t>(left_size)),
                                  std::make_move_iterator(node->entries.end()));
            node->entries.resize(left_size);
            right->next = node->next;
            right->previous = node;
            if (node->next) node->next->previous = right.get();
            else tail_ = right.get();
            node->next = right.get();
            return Split{std::move(right), node->next->entries.front().first, true};
        }

        const auto pos = std::upper_bound(node->separators.begin(), node->separators.end(), key, compare_);
        const std::size_t child = static_cast<std::size_t>(pos - node->separators.begin());
        Split child_split = insert_into(node->children[child].get(), key, value);
        if (child_split.right)
            node->children.insert(node->children.begin() + static_cast<std::ptrdiff_t>(child + 1),
                                  std::move(child_split.right));
        rebuild_separators(node);
        if (node->children.size() <= fanout_)
            return Split{nullptr, Key{}, child_split.inserted};

        const std::size_t left_children = (fanout_ + 1) / 2;
        auto right = std::make_unique<Node>();
        right->leaf = false;
        for (std::size_t i = left_children; i < node->children.size(); ++i)
            right->children.push_back(std::move(node->children[i]));
        node->children.resize(left_children);
        rebuild_separators(node);
        rebuild_separators(right.get());
        const Key separator = min_key(right.get());
        return Split{std::move(right), separator, child_split.inserted};
    }

    Key min_key(const Node* node) const {
        while (!node->leaf) node = node->children.front().get();
        return node->entries.front().first;
    }
    void rebuild_separators(Node* node) const {
        node->separators.clear();
        if (node->leaf) return;
        for (std::size_t i = 1; i < node->children.size(); ++i)
            node->separators.push_back(min_key(node->children[i].get()));
    }
};

template<class Key, class Value, class Compare>
constexpr char BPlusTree<Key, Value, Compare>::kMagic[8];
