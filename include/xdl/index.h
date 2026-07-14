#pragma once

#include "types.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// IndexEntry  — everything the index needs to know to fetch a row
// ─────────────────────────────────────────────────────────────────────────────

struct IndexEntry {
    uint32_t page_id;
    uint64_t page_offset;   // file byte offset of the page header
    uint32_t row_slot;      // 0-based position within the page
};

// ─────────────────────────────────────────────────────────────────────────────
// IndexManager  — flat sorted vector for cache-friendly lookups
//
// Uses a sorted vector of (id, IndexEntry) pairs with binary search.
// Bulk-loaded at recover() time via prepare(), then kept sorted.
// For insert-heavy workloads we buffer new entries and merge periodically.
// ─────────────────────────────────────────────────────────────────────────────

class IndexManager {
public:
    // Insert a new entry.  Throws DuplicateKeyError if id already present.
    void insert(uint32_t id, const IndexEntry& entry) {
        if (prepared_) {
            if (buf_.size() >= MERGE_THRESHOLD) merge();
            auto it = lower_bound(id);
            if (it != entries_.end() && it->id == id)
                throw DuplicateKeyError("Duplicate key: " + std::to_string(id));
        }
        buf_.push_back({id, entry});
        page_to_ids_[entry.page_id].insert(id);
    }

    // Lookup — returns nullptr if absent
    const IndexEntry* find(uint32_t id) const {
        if (prepared_) {
            auto it = lower_bound(id);
            if (it != entries_.end() && it->id == id)
                return &it->entry;
        }
        for (auto& p : buf_) {
            if (p.id == id) return &p.entry;
        }
        return nullptr;
    }

    // Remove (needed for future DELETE support)
    bool remove(uint32_t id) {
        if (prepared_) {
            auto it = lower_bound(id);
            if (it != entries_.end() && it->id == id) {
                page_to_ids_[it->entry.page_id].erase(id);
                entries_.erase(it);
                return true;
            }
        }
        for (auto it = buf_.begin(); it != buf_.end(); ++it) {
            if (it->id == id) {
                page_to_ids_[it->entry.page_id].erase(id);
                buf_.erase(it);
                return true;
            }
        }
        return false;
    }

    // Update file offset for an existing key
    void update_offset(uint32_t id, uint64_t new_offset) {
        if (prepared_) {
            auto it = lower_bound(id);
            if (it != entries_.end() && it->id == id) {
                it->entry.page_offset = new_offset;
                return;
            }
        }
        for (auto& p : buf_) {
            if (p.id == id) { p.entry.page_offset = new_offset; return; }
        }
    }

    // Batch update offsets for all entries on a given page
    // Uses reverse mapping for O(k) where k = entries on this page
    void update_offsets_for_page(uint32_t page_id, uint64_t new_offset) {
        auto pit = page_to_ids_.find(page_id);
        if (pit == page_to_ids_.end()) return;
        for (uint32_t id : pit->second) {
            update_offset(id, new_offset);
        }
    }

    bool     empty() const { return entries_.empty() && buf_.empty(); }
    size_t   size()  const { return entries_.size() + buf_.size(); }

    // Sort and merge the buffer into the sorted portion
    void prepare() {
        if (!buf_.empty()) merge();
        prepared_ = true;
    }

    // Iterate all entries (used during rebuild / compaction)
    template<typename Fn>
    void for_each(Fn&& fn) const {
        for (auto& p : entries_) fn(p.id, p.entry);
        for (auto& p : buf_) fn(p.id, p.entry);
    }

private:
    struct Pair {
        uint32_t    id;
        IndexEntry  entry;
        bool operator<(const Pair& o) const { return id < o.id; }
    };

    std::vector<Pair> entries_;
    std::vector<Pair> buf_;
    static constexpr size_t MERGE_THRESHOLD = 1024;
    bool prepared_ = false;

    // Reverse mapping: page_id → set of row IDs on that page
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> page_to_ids_;

    std::vector<Pair>::iterator lower_bound(uint32_t id) {
        return std::lower_bound(entries_.begin(), entries_.end(), Pair{id, {}},
            [](const Pair& a, const Pair& b) { return a.id < b.id; });
    }

    std::vector<Pair>::const_iterator lower_bound(uint32_t id) const {
        return std::lower_bound(entries_.begin(), entries_.end(), Pair{id, {}},
            [](const Pair& a, const Pair& b) { return a.id < b.id; });
    }

    void merge() {
        if (buf_.empty()) return;
        std::sort(buf_.begin(), buf_.end());
        std::vector<Pair> merged;
        merged.reserve(entries_.size() + buf_.size());
        std::merge(entries_.begin(), entries_.end(), buf_.begin(), buf_.end(),
                   std::back_inserter(merged));
        entries_ = std::move(merged);
        buf_.clear();
    }
};

} // namespace xdl
