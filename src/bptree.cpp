#include "xdl/bptree.h"
#include <algorithm>
#include <cstring>
#include <cmath>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// Key encoding — order-preserving byte representations
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
static void encode_int_be(char* out, T val) {
    for (int i = static_cast<int>(sizeof(T)) - 1; i >= 0; --i) {
        out[i] = static_cast<char>(val & 0xFF);
        val = static_cast<T>(val >> 8);
    }
}

template<typename T>
static void encode_signed_be(char* out, T val) {
    using U = typename std::make_unsigned<T>::type;
    U uval;
    std::memcpy(&uval, &val, sizeof(T));
    uval ^= (static_cast<U>(1) << (sizeof(T) * 8 - 1));
    encode_int_be(out, uval);
}

static void encode_float_ordered(char* out, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    if (bits & 0x80000000u)
        bits ^= 0xFFFFFFFFu;
    else
        bits ^= 0x80000000u;
    encode_int_be(out, bits);
}

KeyBuffer bptree_key_encode(const FieldValue& fv, FieldType type) {
    KeyBuffer kb;
    switch (type) {
    case FieldType::UINT32:
        kb.len = 4;
        encode_int_be(kb.data(), std::get<uint32_t>(fv));
        break;
    case FieldType::INT64:
        kb.len = 8;
        encode_signed_be(kb.data(), std::get<int64_t>(fv));
        break;
    case FieldType::FLOAT32:
        kb.len = 4;
        encode_float_ordered(kb.data(), std::get<float>(fv));
        break;
    case FieldType::BOOL:
        kb.len = 1;
        kb.buf[0] = static_cast<char>(std::get<bool>(fv) ? 1 : 0);
        break;
    case FieldType::STRING: {
        const std::string& s = std::get<std::string>(fv);
        if (s.size() <= 32) {
            kb.len = static_cast<uint8_t>(s.size());
            std::memcpy(kb.data(), s.data(), s.size());
        } else {
            kb.len = 32;
            std::memcpy(kb.data(), s.data(), 32);
        }
        break;
    }
    }
    return kb;
}

std::vector<char> bptree_key_encode_vec(const FieldValue& fv, FieldType type) {
    switch (type) {
    case FieldType::UINT32: {
        std::vector<char> v(4);
        encode_int_be(v.data(), std::get<uint32_t>(fv));
        return v;
    }
    case FieldType::INT64: {
        std::vector<char> v(8);
        encode_signed_be(v.data(), std::get<int64_t>(fv));
        return v;
    }
    case FieldType::FLOAT32: {
        std::vector<char> v(4);
        encode_float_ordered(v.data(), std::get<float>(fv));
        return v;
    }
    case FieldType::BOOL:
        return {static_cast<char>(std::get<bool>(fv) ? 1 : 0)};
    case FieldType::STRING: {
        const std::string& s = std::get<std::string>(fv);
        return std::vector<char>(s.begin(), s.end());
    }
    }
    return {};
}

int bptree_key_compare(const char* a, size_t a_len, const char* b, size_t b_len) {
    size_t min_len = std::min(a_len, b_len);
    int cmp = std::memcmp(a, b, min_len);
    if (cmp != 0) return cmp;
    if (a_len < b_len) return -1;
    if (a_len > b_len) return 1;
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// BPTree — flat sorted vector
// ─────────────────────────────────────────────────────────────────────────────

const BPTree::Entry* BPTree::find_entry(const char* key, size_t key_len) const {
    auto it = lower_bound(key, key_len);
    if (it == entries_.end()) return nullptr;
    if (bptree_key_compare(it->key_data.data(), it->key_data.size(), key, key_len) != 0)
        return nullptr;
    return &(*it);
}

std::vector<BPTree::Entry>::iterator BPTree::lower_bound(const char* key, size_t key_len) {
    return std::lower_bound(entries_.begin(), entries_.end(), key,
        [key, key_len](const Entry& e, const char* k) {
            return bptree_key_compare(e.key_data.data(), e.key_data.size(), k, key_len) < 0;
        });
}

std::vector<BPTree::Entry>::const_iterator BPTree::lower_bound(const char* key, size_t key_len) const {
    return std::lower_bound(entries_.begin(), entries_.end(), key,
        [key, key_len](const Entry& e, const char* k) {
            return bptree_key_compare(e.key_data.data(), e.key_data.size(), k, key_len) < 0;
        });
}

void BPTree::insert(const FieldValue& key, FieldType key_type, uint32_t row_id) {
    KeyBuffer kb = bptree_key_encode(key, key_type);
    auto it = lower_bound(kb.data(), kb.size());

    if (it != entries_.end() &&
        bptree_key_compare(it->key_data.data(), it->key_data.size(),
                           kb.data(), kb.size()) == 0) {
        auto& ids = it->ids;
        if (std::find(ids.begin(), ids.end(), row_id) == ids.end()) {
            ids.push_back(row_id);
        }
    } else {
        Entry e;
        e.key_data.assign(kb.data(), kb.data() + kb.size());
        e.ids.push_back(row_id);
        entries_.insert(it, std::move(e));
    }
}

void BPTree::erase(const FieldValue& key, FieldType key_type, uint32_t row_id) {
    KeyBuffer kb = bptree_key_encode(key, key_type);
    auto it = lower_bound(kb.data(), kb.size());
    if (it == entries_.end()) return;
    if (bptree_key_compare(it->key_data.data(), it->key_data.size(),
                           kb.data(), kb.size()) != 0) return;

    auto& ids = it->ids;
    ids.erase(std::remove(ids.begin(), ids.end(), row_id), ids.end());
    if (ids.empty()) {
        entries_.erase(it);
    }
}

const std::vector<uint32_t>* BPTree::find(const FieldValue& key, FieldType key_type) const {
    KeyBuffer kb = bptree_key_encode(key, key_type);
    const Entry* e = find_entry(kb.data(), kb.size());
    return e ? &e->ids : nullptr;
}

void BPTree::range_scan(
    const std::optional<FieldValue>& lo, FieldType lo_type, bool lo_inclusive,
    const std::optional<FieldValue>& hi, FieldType hi_type, bool hi_inclusive,
    std::function<void(const std::vector<char>&, const std::vector<uint32_t>&)> visitor) const
{
    KeyBuffer lo_kb, hi_kb;
    if (lo) lo_kb = bptree_key_encode(*lo, lo_type);
    if (hi) hi_kb = bptree_key_encode(*hi, hi_type);

    auto it = entries_.begin();

    if (lo) {
        it = lower_bound(lo_kb.data(), lo_kb.size());
        if (!lo_inclusive && it != entries_.end() &&
            bptree_key_compare(it->key_data.data(), it->key_data.size(),
                               lo_kb.data(), lo_kb.size()) == 0) {
            ++it;
        }
    }

    for (; it != entries_.end(); ++it) {
        if (hi) {
            int cmp = bptree_key_compare(it->key_data.data(), it->key_data.size(),
                                         hi_kb.data(), hi_kb.size());
            if (cmp > 0) break;
            if (cmp == 0 && !hi_inclusive) break;
        }
        visitor(it->key_data, it->ids);
    }
}

} // namespace xdl
