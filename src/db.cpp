#include "xdl/db.h"
#include <stdexcept>
#include <algorithm>
#include <cstring>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// ScanFilter
// ─────────────────────────────────────────────────────────────────────────────

// Helper: skip past field at position `src` according to `field_type`.
// Returns pointer past the end of that field's data.
static const char* skip_field(const char* src, FieldType ft) {
    switch (ft) {
    case FieldType::UINT32:  return src + 4;
    case FieldType::INT64:   return src + 8;
    case FieldType::FLOAT32: return src + 4;
    case FieldType::BOOL:    return src + 1;
    case FieldType::STRING: {
        uint16_t len = serialise::read_u16(src);
        return src + 2 + len;
    }
    }
    return src;
}

bool ScanFilter::matches_raw(const char* row_data, const Schema& schema) const {
    if (constraints.empty()) return true;

    // Row layout: [total_len:4][row_id:4][fields...]
    const char* src = row_data + 8; // skip length header + row id

    for (size_t fi = 0; fi < schema.fields.size(); ++fi) {
        const auto& fd = schema.fields[fi];

        // Check if there's a constraint on this field
        auto it = constraints.find(fd.name);
        if (it != constraints.end()) {
            const auto& c = it->second;
            switch (fd.type) {
            case FieldType::UINT32: {
                uint32_t v = serialise::read_u32(src);
                if (c.eq_uint  && v != *c.eq_uint)  return false;
                if (c.min_uint && v <  *c.min_uint)  return false;
                if (c.max_uint && v >  *c.max_uint)  return false;
                break;
            }
            case FieldType::INT64: {
                uint64_t u = serialise::read_u64(src);
                int64_t v; std::memcpy(&v, &u, 8);
                if (c.eq_int64  && v != *c.eq_int64) return false;
                if (c.min_int64 && v <  *c.min_int64) return false;
                if (c.max_int64 && v >  *c.max_int64) return false;
                break;
            }
            case FieldType::FLOAT32: {
                float v; serialise::read_f32(src, v);
                if (c.eq_float  && v != *c.eq_float) return false;
                if (c.min_float && v <  *c.min_float) return false;
                if (c.max_float && v >  *c.max_float) return false;
                break;
            }
            case FieldType::BOOL: {
                bool v = serialise::read_u8(src) != 0;
                if (c.eq_bool && v != *c.eq_bool) return false;
                break;
            }
            case FieldType::STRING: {
                uint16_t len = serialise::read_u16(src);
                std::string_view sv(src + 2, len);
                if (c.eq_str && sv != *c.eq_str) return false;
                if (c.prefix) {
                    if (sv.size() < c.prefix->size()) return false;
                    if (sv.compare(0, c.prefix->size(), *c.prefix) != 0) return false;
                }
                break;
            }
            }
        }

        src = skip_field(src, fd.type);
    }
    return true;
}

bool ScanFilter::matches(const Row& row, const Schema& schema) const {
    for (const auto& [field_name, c] : constraints) {
        int idx = schema.field_index(field_name);
        if (idx < 0) continue; // unknown field — ignore constraint

        const FieldValue& fv = row.fields.at(static_cast<size_t>(idx));
        const FieldType ft   = schema.fields[static_cast<size_t>(idx)].type;

        switch (ft) {
        case FieldType::UINT32: {
            uint32_t v = std::get<uint32_t>(fv);
            if (c.eq_uint   && v != *c.eq_uint  ) return false;
            if (c.min_uint  && v <  *c.min_uint  ) return false;
            if (c.max_uint  && v >  *c.max_uint  ) return false;
            break;
        }
        case FieldType::INT64: {
            int64_t v = std::get<int64_t>(fv);
            if (c.eq_int64  && v != *c.eq_int64 ) return false;
            if (c.min_int64 && v <  *c.min_int64) return false;
            if (c.max_int64 && v >  *c.max_int64) return false;
            break;
        }
        case FieldType::FLOAT32: {
            float v = std::get<float>(fv);
            if (c.eq_float  && v != *c.eq_float ) return false;
            if (c.min_float && v <  *c.min_float) return false;
            if (c.max_float && v >  *c.max_float) return false;
            break;
        }
        case FieldType::BOOL: {
            bool v = std::get<bool>(fv);
            if (c.eq_bool && v != *c.eq_bool) return false;
            break;
        }
        case FieldType::STRING: {
            const std::string& s = std::get<std::string>(fv);
            if (c.eq_str && s != *c.eq_str) return false;
            if (c.prefix) {
                if (s.size() < c.prefix->size()) return false;
                if (s.compare(0, c.prefix->size(), *c.prefix) != 0) return false;
            }
            break;
        }
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// DB
// ─────────────────────────────────────────────────────────────────────────────

DB::DB(const std::string& path, Schema schema,
       CompressionType compression, size_t cache_cap,
       SchemaMigrate migration)
    : path_(path), schema_(std::move(schema)),
      compression_(compression), cache_cap_(cache_cap),
      migration_(std::move(migration))
{
    if (schema_.fields.empty()) {
        schema_.fields = {
            FieldDef{"age",  FieldType::UINT32},
            FieldDef{"name", FieldType::STRING},
        };
    }
    schema_.validate();
}

DB::~DB() {
    if (open_) {
        try { close(); } catch (...) {}
    }
}

void DB::open() {
    if (open_) return;

    storage_ = std::make_unique<StorageEngine>(path_, compression_);
    storage_->open();

    // Write schema to the file BEFORE the pager starts allocating pages.
    // For new files this is the first schema write; for existing files
    // this ensures the schema is up-to-date.
    try {
        storage_->write_schema(schema_);
    } catch (...) {
        // If schema write fails, continue anyway
    }

    index_ = std::make_unique<IndexManager>();

    // Open WAL before pager so pager can use it
    wal_ = std::make_unique<WAL>(path_ + ".wal");
    wal_->open();

    pager_ = std::make_unique<Pager>(*storage_, *index_, *wal_, schema_, cache_cap_);
    pager_->recover();

    // Check and replay WAL
    replay_wal();

    open_ = true;
}

void DB::close() {
    if (!open_) return;
    {
        WriteLock lock(rwlock_);
        // WAL checkpoint: flush all dirty pages, then clear WAL
        pager_->flush_all();
        wal_->log_checkpoint();
        wal_->clear();
        wal_->close();
        storage_->close();
    }
    indexes_.clear();
    open_ = false;
}

bool DB::is_open() const { return open_; }

// ─────────────────────────────────────────────────────────────────────────────
// Insert
// ─────────────────────────────────────────────────────────────────────────────

void DB::insert(const Row& row) {
    WriteLock lock(rwlock_);

    if (!open_) throw XDLError("DB not open");

    if (row.fields.size() != schema_.fields.size())
        throw XDLError("Row field count (" + std::to_string(row.fields.size()) +
                       ") does not match schema field count (" +
                       std::to_string(schema_.fields.size()) + ")");

    if (index_->find(row.id))
        throw DuplicateKeyError("Duplicate key: " + std::to_string(row.id));

    Page& page = get_or_create_writable_page();
    uint32_t slot = page.row_count;
    page.append_row(row, schema_);

    // Zero-copy WAL: write directly from page's data buffer
    const char* row_ptr = page.row_data_ptr(slot);
    uint32_t row_len    = page.row_data_len(slot);

    wal_->log_insert_direct(row.id, page.id, slot, row_ptr, row_len);

    uint64_t offset = pager_->page_offset(page.id);
    index_->insert(row.id, { page.id, offset, slot });

    // Update secondary indexes
    for (auto& [field_name, bptree] : indexes_) {
        int fi = schema_.field_index(field_name);
        if (fi >= 0) {
            bptree->insert(row.fields[static_cast<size_t>(fi)],
                          schema_.fields[static_cast<size_t>(fi)].type,
                          row.id);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Get
// ─────────────────────────────────────────────────────────────────────────────

Row DB::get(uint32_t id) const {
    ReadLock lock(rwlock_);

    if (!open_) throw XDLError("DB not open");

    const IndexEntry* entry = index_->find(id);
    if (!entry)
        throw NotFoundError("Key not found: " + std::to_string(id));

    Page page = pager_->get_page(entry->page_id);
    return page.get_row(entry->row_slot, schema_);
}

bool DB::try_get(uint32_t id, Row& out) const {
    ReadLock lock(rwlock_);
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

void DB::scan(std::function<void(const Row&)> visitor,
              const ScanFilter& filter) const {
    ReadLock lock(rwlock_);

    if (!open_) throw XDLError("DB not open");

    uint32_t total_pages = pager_->page_count();
    for (uint32_t pid = 0; pid < total_pages; ++pid) {
        Page page = pager_->get_page(pid);
        for (uint32_t slot = 0; slot < page.row_count; ++slot) {
            // Zero-copy filter: check against raw bytes without allocating a Row
            if (!filter.matches_raw(page.row_data_ptr(slot), schema_)) continue;
            Row r = page.get_row(slot, schema_);
            visitor(r);
        }
    }
}

std::vector<Row> DB::scan_all(const ScanFilter& filter) const {
    std::vector<Row> results;
    scan([&](const Row& r){ results.push_back(r); }, filter);
    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// Secondary indexes
// ─────────────────────────────────────────────────────────────────────────────

void DB::create_index(const std::string& field_name) {
    WriteLock lock(rwlock_);

    int fi = schema_.field_index(field_name);
    if (fi < 0)
        throw XDLError("Cannot create index on unknown field: " + field_name);

    if (indexes_.count(field_name)) return; // already exists

    auto tree = std::make_unique<BPTree>();

    // Build index from existing data
    uint32_t total_pages = pager_->page_count();
    for (uint32_t pid = 0; pid < total_pages; ++pid) {
        Page page = pager_->get_page(pid);
        for (uint32_t slot = 0; slot < page.row_count; ++slot) {
            Row r = page.get_row(slot, schema_);
            tree->insert(r.fields[static_cast<size_t>(fi)],
                        schema_.fields[static_cast<size_t>(fi)].type,
                        r.id);
        }
    }

    indexes_[field_name] = std::move(tree);
}

void DB::drop_index(const std::string& field_name) {
    WriteLock lock(rwlock_);
    indexes_.erase(field_name);
}

bool DB::has_index(const std::string& field_name) const {
    ReadLock lock(rwlock_);
    return indexes_.count(field_name) > 0;
}

std::vector<Row> DB::index_range_scan(
    const std::string& field,
    const std::optional<FieldValue>& lo, bool lo_inclusive,
    const std::optional<FieldValue>& hi, bool hi_inclusive) const
{
    ReadLock lock(rwlock_);

    auto it = indexes_.find(field);
    if (it == indexes_.end()) {
        throw XDLError("No index on field: " + field);
    }

    int fi = schema_.field_index(field);
    if (fi < 0) throw XDLError("Unknown field: " + field);

    FieldType ft = schema_.fields[static_cast<size_t>(fi)].type;
    std::vector<Row> results;

    it->second->range_scan(
        lo, ft, lo_inclusive, hi, ft, hi_inclusive,
        [&](const std::vector<char>& /*key*/, const std::vector<uint32_t>& ids) {
            for (uint32_t id : ids) {
                const IndexEntry* entry = index_->find(id);
                if (entry) {
                    Page page = pager_->get_page(entry->page_id);
                    results.push_back(page.get_row(entry->row_slot, schema_));
                }
            }
        });

    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// Maintenance
// ─────────────────────────────────────────────────────────────────────────────

void DB::checkpoint() {
    WriteLock lock(rwlock_);
    if (!open_) throw XDLError("DB not open");
    pager_->flush_all();
    wal_->log_checkpoint();
    wal_->clear();
}

Stats DB::stats() const {
    ReadLock lock(rwlock_);
    Stats s{};
    if (open_) {
        s.row_count    = index_->size();
        s.page_count   = pager_->page_count();
        s.cache_size   = 0;  // not tracked in current cache
        s.cache_capacity = cache_cap_;
        s.file_size_bytes = storage_->file_size();
        s.schema_version  = schema_.ver.version;
        s.wal_active      = wal_->is_open();
        s.secondary_index_count = indexes_.size();
    }
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// Private
// ─────────────────────────────────────────────────────────────────────────────

Page& DB::get_or_create_writable_page() {
    uint32_t count = pager_->page_count();
    if (count > 0) {
        Page& last = pager_->get_mutable_page(count - 1);
        if (last.has_capacity()) return last;
    }
    return pager_->new_page();
}

void DB::replay_wal() {
    auto records = wal_->replay();

    for (const auto& rec : records) {
        if (rec.type == WAL::WALRecord::COMMIT || rec.type == WAL::WALRecord::CHECKPOINT) {
            // These are markers — nothing to replay
            continue;
        }

        if (rec.type == WAL::WALRecord::INSERT) {
            // Check if the row already exists (idempotent replay)
            if (index_->find(rec.row_id)) continue;

            // Reconstruct the row and insert it
            // The serialised row data goes into a new page
            try {
                Page& page = get_or_create_writable_page();

                // We need to deserialise the row_bytes into a Row and append it
                // But the WAL stores raw serialised page data (same format as page.data).
                // We can create a temporary page and parse the row from it.
                Page tmp_page(page.id);
                tmp_page.data = std::move(rec.row_data);
                tmp_page.row_count = 1;
                // Build row_offsets manually
                if (!tmp_page.data.empty()) {
                    tmp_page.row_offsets.push_back(0);
                }

                Row r = tmp_page.get_row(0, schema_);
                uint32_t slot = page.row_count;
                page.append_row(r, schema_);

                uint64_t offset = pager_->page_offset(page.id);
                index_->insert(r.id, { page.id, offset, slot });

                // Update secondary indexes
                for (auto& [field_name, bptree] : indexes_) {
                    int fi = schema_.field_index(field_name);
                    if (fi >= 0) {
                        bptree->insert(r.fields[static_cast<size_t>(fi)],
                                      schema_.fields[static_cast<size_t>(fi)].type,
                                      r.id);
                    }
                }
            } catch (...) {
                // Skip corrupt WAL entries
            }
        }
    }
}

} // namespace xdl
