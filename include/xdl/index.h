#pragma once

#include "types.h"
#include <unordered_map>
#include <vector>
#include <functional>

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
// IndexManager  — pure in-memory, never compressed, O(1) lookup
//
// Persistence: the index is rebuilt by scanning all pages at open() time.
// For very large databases a WAL-style persisted index can be added later.
// ─────────────────────────────────────────────────────────────────────────────

class IndexManager {
public:
    // Insert a new entry.  Throws DuplicateKeyError if id already present.
    void insert(uint32_t id, const IndexEntry& entry);

    // Lookup — returns nullptr if absent
    const IndexEntry* find(uint32_t id) const;

    // Remove (needed for future DELETE support)
    bool remove(uint32_t id);

    // Update file offset for an existing key (page re-written to new location)
    void update_offset(uint32_t id, uint64_t new_offset);

    bool     empty() const { return index_.empty(); }
    size_t   size()  const { return index_.size();  }

    // Iterate all entries (used during rebuild / compaction)
    template<typename Fn>
    void for_each(Fn&& fn) const {
        for (const auto& [k, v] : index_) fn(k, v);
    }

private:
    std::unordered_map<uint32_t, IndexEntry> index_;
};

} // namespace xdl