#include "xdl/index.h"

namespace xdl {

void IndexManager::insert(uint32_t id, const IndexEntry& entry) {
    if (index_.count(id))
        throw DuplicateKeyError("Duplicate key: " + std::to_string(id));
    index_[id] = entry;
}

const IndexEntry* IndexManager::find(uint32_t id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &it->second;
}

bool IndexManager::remove(uint32_t id) {
    return index_.erase(id) > 0;
}

void IndexManager::update_offset(uint32_t id, uint64_t new_offset) {
    auto it = index_.find(id);
    if (it != index_.end()) it->second.page_offset = new_offset;
}

} // namespace xdl