#pragma once

#include "types.h"
#include <vector>
#include <algorithm>
#include <functional>
#include <optional>
#include <cstring>
#include <cstdint>
#include <array>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// Stack-allocated key buffer for BPTree operations.
// Avoids heap allocation for every encode/find/insert.
// ─────────────────────────────────────────────────────────────────────────────

struct KeyBuffer {
    std::array<char, 32> buf{};
    uint8_t len = 0;

    const char* data() const { return buf.data(); }
    char*       data()       { return buf.data(); }
    size_t      size() const { return len; }

    bool operator<(const KeyBuffer& o) const {
        size_t min_len = std::min<size_t>(len, o.len);
        int cmp = std::memcmp(buf.data(), o.buf.data(), min_len);
        if (cmp != 0) return cmp < 0;
        return len < o.len;
    }

    bool operator==(const KeyBuffer& o) const {
        return len == o.len && std::memcmp(buf.data(), o.buf.data(), len) == 0;
    }

    bool operator!=(const KeyBuffer& o) const { return !(*this == o); }

    bool operator>(const KeyBuffer& o) const  { return o < *this; }
    bool operator<=(const KeyBuffer& o) const { return !(o < *this); }
    bool operator>=(const KeyBuffer& o) const { return !(*this < o); }
};

// Encode a FieldValue into an order-preserving KeyBuffer (stack-allocated).
KeyBuffer bptree_key_encode(const FieldValue& fv, FieldType type);

// Also provide a vector version for APIs that need dynamic sizing (strings > 32 bytes).
std::vector<char> bptree_key_encode_vec(const FieldValue& fv, FieldType type);

// Compare two encoded keys. Returns <0, 0, or >0.
int bptree_key_compare(const char* a, size_t a_len, const char* b, size_t b_len);

class BPTree {
public:
    BPTree() = default;

    void insert(const FieldValue& key, FieldType key_type, uint32_t row_id);
    void erase(const FieldValue& key, FieldType key_type, uint32_t row_id);

    const std::vector<uint32_t>* find(const FieldValue& key, FieldType key_type) const;

    void range_scan(
        const std::optional<FieldValue>& lo, FieldType lo_type, bool lo_inclusive,
        const std::optional<FieldValue>& hi, FieldType hi_type, bool hi_inclusive,
        std::function<void(const std::vector<char>& key_bytes,
                           const std::vector<uint32_t>& ids)> visitor) const;

    bool empty() const { return entries_.empty(); }
    size_t size() const { return entries_.size(); }

    template<typename Fn>
    void for_each(Fn&& fn) const {
        for (const auto& e : entries_) fn(e.key_data, e.ids);
    }

private:
    struct Entry {
        std::vector<char>   key_data;  // heap-allocated key bytes
        std::vector<uint32_t> ids;

        bool operator<(const Entry& o) const {
            return bptree_key_compare(key_data.data(), key_data.size(),
                                      o.key_data.data(), o.key_data.size()) < 0;
        }
    };

    // Sorted vector of entries (flat, cache-friendly)
    std::vector<Entry> entries_;

    // Binary search on the sorted vector
    const Entry* find_entry(const char* key, size_t key_len) const;
    typename std::vector<Entry>::iterator lower_bound(const char* key, size_t key_len);
    typename std::vector<Entry>::const_iterator lower_bound(const char* key, size_t key_len) const;
};

} // namespace xdl
