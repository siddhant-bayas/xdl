#include "xdl/db.h"
#include <stdexcept>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// ScanFilter
// ─────────────────────────────────────────────────────────────────────────────

bool ScanFilter::matches(const Row& r) const {
    if (min_age && r.age < *min_age) return false;
    if (max_age && r.age > *max_age) return false;
    if (name_prefix) {
        std::string n = r.name_str();
        if (n.size() < name_prefix->size()) return false;
        if (n.compare(0, name_prefix->size(), *name_prefix) != 0) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// DB
// ─────────────────────────────────────────────────────────────────────────────

DB::DB(const std::string& path, CompressionType compression, size_t cache_cap)
    : path_(path), compression_(compression), cache_cap_(cache_cap) {}

DB::~DB() {
    if (open_) {
        try { close(); } catch (...) {}
    }
}

void DB::open() {
    if (open_) return;

    storage_ = std::make_unique<StorageEngine>(path_, compression_);
    storage_->open();

    index_ = std::make_unique<IndexManager>();

    pager_ = std::make_unique<Pager>(*storage_, *index_, cache_cap_);
    pager_->recover();

    open_ = true;
}

void DB::close() {
    if (!open_) return;
    pager_->flush_all();
    storage_->close();
    open_ = false;
}

bool DB::is_open() const { return open_; }

// ─────────────────────────────────────────────────────────────────────────────
// Insert
// ─────────────────────────────────────────────────────────────────────────────

void DB::insert(const Row& row) {
    if (!open_) throw XDLError("DB not open");

    // DuplicateKeyError is thrown here if id exists
    if (index_->find(row.id))
        throw DuplicateKeyError("Duplicate key: " + std::to_string(row.id));

    Page& page = get_or_create_writable_page();
    uint32_t slot = page.row_count;
    page.append_row(row);

    // We need the file offset to store in the index, but the page may not be
    // flushed yet.  We store UINT64_MAX as a sentinel and fill it in on flush.
    // The Pager's flush_page / on_cache_evict will update the IndexEntry.
    // For now we use page_offsets_ which is updated after any write.
    // Actually, for reads before flush we resolve via Pager::get_page(page_id)
    // which returns the in-memory dirty page — so offset doesn't matter yet.
    uint64_t offset = pager_->page_offset(page.id);  // UINT64_MAX if not yet on disk
    index_->insert(row.id, { page.id, offset, slot });
}

// ─────────────────────────────────────────────────────────────────────────────
// Get
// ─────────────────────────────────────────────────────────────────────────────

Row DB::get(uint32_t id) const {
    if (!open_) throw XDLError("DB not open");

    const IndexEntry* entry = index_->find(id);
    if (!entry)
        throw NotFoundError("Key not found: " + std::to_string(id));

    Page& page = const_cast<Pager&>(*pager_).get_page(entry->page_id);
    return page.get_row(entry->row_slot);
}

bool DB::try_get(uint32_t id, Row& out) const {
    try {
        out = get(id);
        return true;
    } catch (const NotFoundError&) {
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Scan
// ─────────────────────────────────────────────────────────────────────────────

void DB::scan(std::function<void(const Row&)> visitor, const ScanFilter& filter) const {
    if (!open_) throw XDLError("DB not open");

    uint32_t total_pages = pager_->page_count();
    for (uint32_t pid = 0; pid < total_pages; ++pid) {
        Page& page = const_cast<Pager&>(*pager_).get_page(pid);
        for (uint32_t slot = 0; slot < page.row_count; ++slot) {
            Row r = page.get_row(slot);
            if (filter.matches(r)) visitor(r);
        }
    }
}

std::vector<Row> DB::scan_all(const ScanFilter& filter) const {
    std::vector<Row> results;
    scan([&](const Row& r){ results.push_back(r); }, filter);
    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// Maintenance
// ─────────────────────────────────────────────────────────────────────────────

void DB::checkpoint() {
    if (!open_) throw XDLError("DB not open");
    pager_->flush_all();
}

Stats DB::stats() const {
    return Stats{
        index_->size(),
        pager_->page_count(),
        0,               // cache_.size() — expose via Pager if needed
        cache_cap_,
        storage_->file_size()
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// Private
// ─────────────────────────────────────────────────────────────────────────────

Page& DB::get_or_create_writable_page() {
    // Walk pages from the last one downwards looking for capacity
    uint32_t count = pager_->page_count();
    if (count > 0) {
        Page& last = pager_->get_page(count - 1);
        if (last.has_capacity()) return last;
    }
    return pager_->new_page();
}

} // namespace xdl