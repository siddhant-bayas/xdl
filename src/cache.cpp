#include "xdl/cache.h"

namespace xdl {

PageCache::PageCache(size_t capacity, EvictionCb on_evict)
    : capacity_(capacity), on_evict_(std::move(on_evict))
{
    if (capacity_ == 0) capacity_ = 1;
}

std::optional<Page> PageCache::get(uint32_t page_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = map_.find(page_id);
    if (it == map_.end()) return std::nullopt;

    lru_.splice(lru_.begin(), lru_, it->second);
    return *it->second;  // copy made under lock
}

Page* PageCache::get_mut(uint32_t page_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = map_.find(page_id);
    if (it == map_.end()) return nullptr;

    lru_.splice(lru_.begin(), lru_, it->second);
    return &(*it->second);
}

void PageCache::put(Page page) {
    std::lock_guard<std::mutex> lock(mtx_);
    uint32_t pid = page.id;

    // If already cached, update in place and move to front
    auto it = map_.find(pid);
    if (it != map_.end()) {
        it->second->data        = std::move(page.data);
        it->second->row_count   = page.row_count;
        it->second->dirty       = page.dirty;
        it->second->row_offsets = std::move(page.row_offsets);
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }

    // Evict LRU if at capacity
    if (lru_.size() >= capacity_) evict_lru();

    lru_.push_front(std::move(page));
    map_[pid] = lru_.begin();
}

void PageCache::evict(uint32_t page_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = map_.find(page_id);
    if (it == map_.end()) return;
    if (on_evict_ && it->second->dirty) on_evict_(*it->second);
    lru_.erase(it->second);
    map_.erase(it);
}

void PageCache::flush_all() {
    std::lock_guard<std::mutex> lock(mtx_);
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

size_t PageCache::size() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return map_.size();
}

} // namespace xdl
