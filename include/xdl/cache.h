#pragma once

#include "page.h"
#include <unordered_map>
#include <list>
#include <functional>
#include <optional>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// PageCache — LRU buffer pool
//
// On eviction of a dirty page, the eviction_callback is invoked so the Pager
// can flush it to disk before the memory is released.
// ─────────────────────────────────────────────────────────────────────────────

class PageCache {
public:
    using EvictionCb = std::function<void(Page&)>;

    explicit PageCache(size_t capacity, EvictionCb on_evict = nullptr);

    // Returns nullptr if not cached
    Page* get(uint32_t page_id);

    // Insert or replace.  May evict the LRU entry.
    void put(Page page);

    // Forcibly remove a page (e.g. after explicit flush)
    void evict(uint32_t page_id);

    // Flush all dirty pages via the eviction callback, then clear
    void flush_all();

    size_t size()     const { return map_.size(); }
    size_t capacity() const { return capacity_; }

    // Expose dirty pages for iteration (used during checkpoint / close)
    template<typename Fn>
    void for_each_dirty(Fn&& fn) {
        for (auto& [id, it] : map_) {
            if (it->dirty) fn(*it);
        }
    }

private:
    void evict_lru();

    size_t     capacity_;
    EvictionCb on_evict_;

    // LRU list: front = most recently used
    std::list<Page>                                          lru_;
    std::unordered_map<uint32_t, std::list<Page>::iterator> map_;
};

} // namespace xdl