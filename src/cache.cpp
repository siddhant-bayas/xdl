#include "xdl/cache.h"

namespace xdl {

PageCache::PageCache(size_t capacity, EvictionCb on_evict)
    : capacity_(capacity), on_evict_(std::move(on_evict))
{
    if (capacity_ == 0) capacity_ = 1;
}

Page* PageCache::get(uint32_t page_id) {
    auto it = map_.find(page_id);
    if (it == map_.end()) return nullptr;

    // Move to front (most recently used)
    lru_.splice(lru_.begin(), lru_, it->second);
    return &(*it->second);
}

void PageCache::put(Page page) {
    uint32_t pid = page.id;

    // If already cached, update in place and move to front
    auto it = map_.find(pid);
    if (it != map_.end()) {
        it->second->data      = std::move(page.data);
        it->second->row_count = page.row_count;
        it->second->dirty     = page.dirty;
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }

    // Evict LRU if at capacity
    if (lru_.size() >= capacity_) evict_lru();

    lru_.push_front(std::move(page));
    map_[pid] = lru_.begin();
}

void PageCache::evict(uint32_t page_id) {
    auto it = map_.find(page_id);
    if (it == map_.end()) return;
    if (on_evict_ && it->second->dirty) on_evict_(*it->second);
    lru_.erase(it->second);
    map_.erase(it);
}

void PageCache::flush_all() {
    if (on_evict_) {
        for (auto& page : lru_) {
            if (page.dirty) on_evict_(page);
        }
    }
    lru_.clear();
    map_.clear();
}

void PageCache::evict_lru() {
    if (lru_.empty()) return;
    Page& victim = lru_.back();
    if (on_evict_ && victim.dirty) on_evict_(victim);
    map_.erase(victim.id);
    lru_.pop_back();
}

} // namespace xdl