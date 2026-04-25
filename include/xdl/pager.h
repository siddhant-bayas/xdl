#pragma once

#include "page.h"
#include "cache.h"
#include "storage.h"
#include "compression.h"
#include "index.h"
#include <memory>
#include <optional>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// Pager
//
// Central coordinator.  The Execution Engine talks *only* to the Pager.
//
//   • Manages page IDs and file offsets
//   • Consults cache before hitting disk
//   • Compresses/decompresses transparently
//   • Writes dirty pages on flush / close
// ─────────────────────────────────────────────────────────────────────────────

class Pager {
public:
    Pager(StorageEngine& storage,
          IndexManager&  index,
          size_t         cache_capacity = DEFAULT_CACHE_SIZE);

    // Called once after open(): scan file, rebuild index, populate metadata
    void recover();

    // Load a page by ID (from cache or disk).  Returns reference valid until
    // next mutation that could trigger cache eviction.
    Page& get_page(uint32_t page_id);

    // Allocate a brand-new empty page and add to cache
    Page& new_page();

    // Mark a page dirty; call after mutating its data
    void mark_dirty(uint32_t page_id);

    // Flush a single dirty page to disk
    void flush_page(uint32_t page_id);

    // Flush all dirty pages (called on close, or as a checkpoint)
    void flush_all();

    uint32_t page_count() const { return next_page_id_; }

    // Translate page_id → file offset (used by IndexManager rebuild)
    uint64_t page_offset(uint32_t page_id) const;

private:
    Page     load_from_disk(uint32_t page_id);
    uint64_t write_page_to_disk(Page& page);
    void     on_cache_evict(Page& page);

    StorageEngine& storage_;
    IndexManager&  index_;
    PageCache      cache_;

    uint32_t next_page_id_ = 0;

    // page_id → file offset for pages that are on disk
    std::unordered_map<uint32_t, uint64_t> page_offsets_;
};

} // namespace xdl