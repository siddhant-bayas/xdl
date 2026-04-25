#include "xdl/pager.h"
#include <stdexcept>

namespace xdl {

Pager::Pager(StorageEngine& storage, IndexManager& index, size_t cache_capacity)
    : storage_(storage), index_(index),
      cache_(cache_capacity, [this](Page& p){ this->on_cache_evict(p); })
{}

// ─────────────────────────────────────────────────────────────────────────────
// recover()  — called once after the storage file is opened.
//
// Scans every page sequentially and rebuilds the in-memory index.
// Also populates page_offsets_ so we know where each page lives on disk.
// ─────────────────────────────────────────────────────────────────────────────

void Pager::recover() {
    DBHeader dbhdr = storage_.read_db_header();
    uint32_t disk_page_count = dbhdr.page_count;

    uint64_t offset = sizeof(DBHeader);
    uint64_t file_sz = storage_.file_size();

    uint32_t pages_seen = 0;

    while (offset + sizeof(PageHeader) <= file_sz && pages_seen < disk_page_count) {
        PageHeader hdr{};
        std::vector<char> blob;
        storage_.read_page_raw(offset, hdr, blob);

        // Decompress to read row metadata for index rebuild
        CompressionType ct = static_cast<CompressionType>(hdr.compression_type);
        std::vector<char> uncompressed(hdr.uncompressed_size);
        CompressionEngine::decompress(ct,
            blob.data(), blob.size(),
            uncompressed.data(), uncompressed.size());

        Page page(hdr.page_id);
        page.data      = std::move(uncompressed);
        page.row_count = hdr.row_count;

        // Rebuild index entries for every row in this page
        for (uint32_t slot = 0; slot < page.row_count; ++slot) {
            Row r = page.get_row(slot);
            IndexEntry entry{ hdr.page_id, offset, slot };
            try {
                index_.insert(r.id, entry);
            } catch (DuplicateKeyError&) {
                // On-disk corruption — skip duplicate; could log here
            }
        }

        page_offsets_[hdr.page_id] = offset;
        offset += sizeof(PageHeader) + hdr.compressed_size;
        next_page_id_ = std::max(next_page_id_, hdr.page_id + 1);
        ++pages_seen;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// get_page()
// ─────────────────────────────────────────────────────────────────────────────

Page& Pager::get_page(uint32_t page_id) {
    Page* cached = cache_.get(page_id);
    if (cached) return *cached;

    // Not in cache — load from disk
    Page page = load_from_disk(page_id);
    cache_.put(std::move(page));
    return *cache_.get(page_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// new_page()
// ─────────────────────────────────────────────────────────────────────────────

Page& Pager::new_page() {
    uint32_t pid = next_page_id_++;
    Page page(pid);
    page.dirty = true;
    cache_.put(std::move(page));
    return *cache_.get(pid);
}

void Pager::mark_dirty(uint32_t page_id) {
    Page* p = cache_.get(page_id);
    if (p) p->dirty = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// flush_page()
// ─────────────────────────────────────────────────────────────────────────────

void Pager::flush_page(uint32_t page_id) {
    Page* p = cache_.get(page_id);
    if (!p || !p->dirty) return;
    uint64_t new_offset = write_page_to_disk(*p);

    // Update all index entries pointing to this page with new offset
    index_.for_each([&](uint32_t id, const IndexEntry& e){
        if (e.page_id == page_id) {
            const_cast<IndexEntry&>(e).page_offset = new_offset;
        }
    });

    p->dirty = false;
}

void Pager::flush_all() {
    cache_.for_each_dirty([this](Page& p){
        if (p.dirty) {
            uint64_t new_offset = write_page_to_disk(p);
            index_.for_each([&](uint32_t id, const IndexEntry& e){
                if (e.page_id == p.id) {
                    const_cast<IndexEntry&>(e).page_offset = new_offset;
                }
            });
            p.dirty = false;
        }
    });

    // Update DB header with current page count
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

    uint64_t offset;
    auto it = page_offsets_.find(page.id);
    if (it != page_offsets_.end()) {
        // Overwrite only if compressed size fits (otherwise append and update offset)
        PageHeader old_hdr{};
        std::vector<char> dummy;
        storage_.read_page_raw(it->second, old_hdr, dummy);

        if (comp_size <= old_hdr.compressed_size) {
            // Pad to original size to maintain file layout
            compressed.resize(old_hdr.compressed_size, 0);
            hdr.compressed_size = old_hdr.compressed_size;
            storage_.overwrite_page_raw(it->second, hdr, compressed);
            offset = it->second;
        } else {
            offset = storage_.append_page_raw(hdr, compressed);
            page_offsets_[page.id] = offset;
        }
    } else {
        offset = storage_.append_page_raw(hdr, compressed);
        page_offsets_[page.id] = offset;
    }

    return offset;
}

void Pager::on_cache_evict(Page& page) {
    if (page.dirty) {
        write_page_to_disk(page);
        page.dirty = false;
    }
}

} // namespace xdl