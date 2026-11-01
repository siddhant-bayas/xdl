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
    return *(*it->second);  // copy made under lock
}

std::shared_ptr<const Page> PageCache::get_shared(uint32_t page_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = map_.find(page_id);
    if (it == map_.end()) return nullptr;

    lru_.splice(lru_.begin(), lru_, it->second);
    return *it->second;
}

Page* PageCache::get_mut(uint32_t page_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = map_.find(page_id);
    if (it == map_.end()) return nullptr;

    lru_.splice(lru_.begin(), lru_, it->second);
    return (*it->second).get();
}

void PageCache::put(Page page) {
    std::lock_guard<std::mutex> lock(mtx_);
    uint32_t pid = page.id;

    // If already cached, replace the list's pointer rather than mutating the
    // Page in place: readers holding the old shared_ptr keep a consistent
    // snapshot instead of racing with the write.
    auto it = map_.find(pid);
    if (it != map_.end()) {
        if (on_evict_ && (*it->second)->dirty) on_evict_(*(*it->second));
        *it->second = std::make_shared<Page>(std::move(page));
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }

    if (lru_.size() >= capacity_) evict_lru();

    lru_.push_front(std::make_shared<Page>(std::move(page)));
    map_[pid] = lru_.begin();
}

std::shared_ptr<Page> PageCache::put_shared(std::shared_ptr<Page> page) {
    std::lock_guard<std::mutex> lock(mtx_);
    uint32_t pid = page->id;

    auto it = map_.find(pid);
    if (it != map_.end()) {
        // A racing reader loaded the same page; keep the resident copy and
        // hand it back so the caller never keeps a pointer to `page` after
        // this function releases it.
        lru_.splice(lru_.begin(), lru_, it->second);
        return *it->second;
    }

    if (lru_.size() >= capacity_) evict_lru();

    lru_.push_front(std::move(page));
    map_[pid] = lru_.begin();
    return *lru_.begin();
}

void PageCache::evict(uint32_t page_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = map_.find(page_id);
    if (it == map_.end()) return;
    if (on_evict_ && (*it->second)->dirty) on_evict_(*(*it->second));
    lru_.erase(it->second);
    map_.erase(it);
}

void PageCache::flush_all() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (on_evict_) {
        for (auto& page : lru_) {
            if (page->dirty) on_evict_(*page);
        }
    }
    lru_.clear();
    map_.clear();
}

void PageCache::evict_lru() {
    if (lru_.empty()) return;
    auto& victim = lru_.back();
    if (on_evict_ && victim->dirty) on_evict_(*victim);
    map_.erase(victim->id);
    lru_.pop_back();
}

size_t PageCache::size() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return map_.size();
}

} // namespace xdl
