#include "xdl/pager.h"
#include <stdexcept>
#include <algorithm>

namespace xdl {

// Fixed slot size for each page: max uncompressed data + header.
static constexpr uint64_t PAGE_SLOT_SIZE = sizeof(PageHeader) + PAGE_SIZE;

static uint64_t page_file_offset(uint64_t data_start, uint32_t page_id) {
    return data_start + static_cast<uint64_t>(page_id) * PAGE_SLOT_SIZE;
}

Pager::Pager(StorageEngine& storage, IndexManager& index,
             WAL& wal, const Schema& schema, size_t cache_capacity)
    : storage_(storage), index_(index), wal_(wal), schema_(schema),
      cache_(cache_capacity, [this](Page& p){ this->on_cache_evict(p); })
{}

// ─────────────────────────────────────────────────────────────────────────────
// rebuild_row_offsets()
// ─────────────────────────────────────────────────────────────────────────────

void Pager::rebuild_row_offsets(Page& page) const {
    page.row_offsets.clear();
    page.row_offsets.reserve(page.row_count);
    uint32_t pos = 0;
    for (uint32_t i = 0; i < page.row_count; ++i) {
        if (pos + 4 > page.data.size())
            throw CorruptionError("Page data truncated while rebuilding row offsets");
        page.row_offsets.push_back(pos);
        uint32_t row_len = serialise::read_u32(page.data.data() + pos);
        pos += row_len;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// recover()
// ─────────────────────────────────────────────────────────────────────────────

void Pager::recover() {
    DBHeader dbhdr = storage_.read_db_header();
    uint32_t disk_page_count = dbhdr.page_count;
    uint64_t data_start = storage_.data_start_offset();
    uint64_t file_sz = storage_.file_size();

    for (uint32_t pid = 0; pid < disk_page_count; ++pid) {
        uint64_t offset = page_file_offset(data_start, pid);
        if (offset + sizeof(PageHeader) > file_sz) break;

        PageHeader hdr{};
        std::vector<char> blob;
        try {
            storage_.read_page_raw(offset, hdr, blob);
        } catch (...) { continue; }

        if (hdr.page_id != pid) continue;

        CompressionType ct = static_cast<CompressionType>(hdr.compression_type);
        std::vector<char> uncompressed(hdr.uncompressed_size);
        try {
            CompressionEngine::decompress(ct,
                blob.data(), blob.size(),
                uncompressed.data(), uncompressed.size());
        } catch (...) { continue; }

        Page page(hdr.page_id);
        page.data      = std::move(uncompressed);
        page.row_count = hdr.row_count;

        rebuild_row_offsets(page);

        for (uint32_t slot = 0; slot < page.row_count; ++slot) {
            Row r = page.get_row(slot, schema_);
            IndexEntry entry{ hdr.page_id, offset, slot };
            try {
                index_.insert(r.id, entry);
            } catch (DuplicateKeyError&) {
                // skip duplicate
            }
        }

        page_offsets_[hdr.page_id] = offset;
        next_page_id_ = std::max(next_page_id_, hdr.page_id + 1);
    }

    // Prepare index for fast lookups after bulk load
    index_.prepare();
}

// ─────────────────────────────────────────────────────────────────────────────
// get_page() — returns a copy, safe for concurrent readers
// ─────────────────────────────────────────────────────────────────────────────

Page Pager::get_page(uint32_t page_id) {
    auto cached = cache_.get(page_id);
    if (cached) return *cached;

    Page page = load_from_disk(page_id);
    Page result = page;               // copy before moving into cache
    cache_.put(std::move(page));
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// get_mutable_page() — returns reference into cache, caller must hold
// exclusive lock
// ─────────────────────────────────────────────────────────────────────────────

Page& Pager::get_mutable_page(uint32_t page_id) {
    Page* cached = cache_.get_mut(page_id);
    if (cached) return *cached;

    Page page = load_from_disk(page_id);
    cache_.put(std::move(page));
    return *cache_.get_mut(page_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// new_page()
// ─────────────────────────────────────────────────────────────────────────────

Page& Pager::new_page() {
    uint32_t pid = next_page_id_++;
    Page page(pid);
    page.dirty = true;
    cache_.put(std::move(page));
    return *cache_.get_mut(pid);
}

void Pager::mark_dirty(uint32_t page_id) {
    Page* p = cache_.get_mut(page_id);
    if (p) p->dirty = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// flush_page()
// ─────────────────────────────────────────────────────────────────────────────

void Pager::flush_page(uint32_t page_id) {
    Page* p = cache_.get_mut(page_id);
    if (!p || !p->dirty) return;
    uint64_t new_offset = write_page_to_disk(*p);

    // Batch update: update all index entries for this page at once
    index_.update_offsets_for_page(page_id, new_offset);

    p->dirty = false;
}

void Pager::flush_all() {
    // Collect all dirty pages first, then flush
    std::vector<uint32_t> dirty_pages;
    cache_.for_each_dirty([this, &dirty_pages](Page& p) {
        if (p.dirty) dirty_pages.push_back(p.id);
    });

    for (uint32_t pid : dirty_pages) {
        Page* p = cache_.get_mut(pid);
        if (!p || !p->dirty) continue;
        uint64_t new_offset = write_page_to_disk(*p);
        index_.update_offsets_for_page(pid, new_offset);
        p->dirty = false;
    }

    DBHeader dbhdr = storage_.read_db_header();
    dbhdr.page_count = next_page_id_;
    storage_.write_db_header(dbhdr);
}

uint64_t Pager::page_offset(uint32_t page_id) const {
    auto it = page_offsets_.find(page_id);
    return it == page_offsets_.end() ? UINT64_MAX : it->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────────────────────

Page Pager::load_from_disk(uint32_t page_id) {
    auto it = page_offsets_.find(page_id);
    if (it == page_offsets_.end())
        throw NotFoundError("Page not on disk: " + std::to_string(page_id));

    PageHeader hdr{};
    std::vector<char> blob;
    storage_.read_page_raw(it->second, hdr, blob);

    CompressionType ct = static_cast<CompressionType>(hdr.compression_type);
    std::vector<char> uncompressed(hdr.uncompressed_size);
    CompressionEngine::decompress(ct,
        blob.data(), blob.size(),
        uncompressed.data(), uncompressed.size());

    Page page(page_id);
    page.data      = std::move(uncompressed);
    page.row_count = hdr.row_count;
    page.dirty     = false;

    rebuild_row_offsets(page);
    return page;
}

uint64_t Pager::write_page_to_disk(Page& page) {
    CompressionType ct = storage_.default_compression();
    std::vector<char> compressed;
    size_t comp_size = CompressionEngine::compress(
        ct, page.data.data(), page.data.size(), compressed);

    PageHeader hdr{};
    hdr.page_id           = page.id;
    hdr.compressed_size   = static_cast<uint32_t>(comp_size);
    hdr.uncompressed_size = static_cast<uint32_t>(page.data.size());
    hdr.row_count         = page.row_count;
    hdr.compression_type  = static_cast<uint8_t>(ct);

    uint64_t data_start = storage_.data_start_offset();
    uint64_t offset = page_file_offset(data_start, page.id);

    storage_.overwrite_page_raw(offset, hdr, compressed);

    page_offsets_[page.id] = offset;
    return offset;
}

void Pager::on_cache_evict(Page& page) {
    if (page.dirty) {
        write_page_to_disk(page);
        page.dirty = false;
    }
}

} // namespace xdl
