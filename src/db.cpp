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

// Field order is part of the row wire format, so compare positionally.
static bool schemas_match(const Schema& a, const Schema& b) {
    if (a.fields.size() != b.fields.size()) return false;
    for (size_t i = 0; i < a.fields.size(); ++i) {
        if (a.fields[i].name != b.fields[i].name) return false;
        if (a.fields[i].type != b.fields[i].type) return false;
    }
    return true;
}

DB::DB(const std::string& path, Schema schema,
       CompressionType compression, size_t cache_cap,
       SchemaMigrate migration)
    : path_(path), schema_(std::move(schema)),
      compression_(compression), cache_cap_(cache_cap),
      migration_(std::move(migration))
{
    schema_explicit_ = !schema_.fields.empty();
    if (!schema_explicit_) {
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

    // Adopt the on-disk schema when the caller did not supply one.  Writing
    // the constructor schema over an existing file would silently reinterpret
    // every stored row (and can shift data_start_offset()), so an explicit
    // incompatible schema is rejected instead.
    DBHeader dbhdr = storage_->read_db_header();
    bool have_stored_schema = false;
    Schema stored;
    if (storage_->file_size() > sizeof(DBHeader) + 4) {
        try {
            stored = storage_->read_schema();
            have_stored_schema = true;
        } catch (const XDLError&) {
            have_stored_schema = false;
        }
    }

    if (have_stored_schema) {
        if (schema_explicit_) {
            if (!schemas_match(stored, schema_)) {
                storage_->close();
                storage_.reset();
                throw MigrationError(
                    "On-disk schema does not match the requested schema for " +
                    path_ + ". Open without an explicit schema to adopt the "
                    "stored schema, or provide a matching schema.");
            }
            // Keep the stored version/metadata so the on-disk layout is stable.
            schema_ = stored;
        } else {
            schema_ = stored;
        }
    } else {
        if (dbhdr.page_count > 0) {
            storage_->close();
            storage_.reset();
            throw CorruptionError(
                "Database " + path_ + " has data pages but no stored schema; "
                "refusing to overwrite it");
        }
        storage_->write_schema(schema_);
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
    WriteLock lock(rwlock_);
    if (!open_) return;
    // WAL checkpoint: flush all dirty pages, then clear WAL
    pager_->flush_all();
    wal_->log_checkpoint();
    wal_->clear();
    wal_->close();
    storage_->close();
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
    insert_unlocked(row, true);
}

size_t DB::insert_batch(const std::vector<Row>& rows) {
    WriteLock lock(rwlock_);

    if (!open_) throw XDLError("DB not open");
    if (rows.empty()) return 0;

    // Validate everything up front so a bad row cannot leave a half-applied
    // batch.  serialised_row_size also enforces the string/page limits.
    std::unordered_set<uint32_t> seen;
    seen.reserve(rows.size() * 2);
    for (const auto& row : rows) {
        if (row.fields.size() != schema_.fields.size())
            throw XDLError("Row field count (" + std::to_string(row.fields.size()) +
                           ") does not match schema field count (" +
                           std::to_string(schema_.fields.size()) + ")");
        if (index_->find(row.id) || !seen.insert(row.id).second)
            throw DuplicateKeyError("Duplicate key: " + std::to_string(row.id));
        uint32_t need = serialised_row_size(row, schema_);
        if (need > PAGE_SIZE)
            throw XDLError("Row too large for a page: " + std::to_string(need) +
                           " bytes (page limit " + std::to_string(PAGE_SIZE) + ")");
    }

    // One fsync for the whole batch instead of one per row.
    for (const auto& row : rows)
        insert_unlocked(row, false);
    wal_->sync();

    return rows.size();
}

void DB::insert_unlocked(const Row& row, bool sync_now) {
    if (row.fields.size() != schema_.fields.size())
        throw XDLError("Row field count (" + std::to_string(row.fields.size()) +
                       ") does not match schema field count (" +
                       std::to_string(schema_.fields.size()) + ")");

    if (index_->find(row.id))
        throw DuplicateKeyError("Duplicate key: " + std::to_string(row.id));

    Page& page = get_or_create_writable_page(row);
    uint32_t slot = page.row_count;
    page.append_row(row, schema_);

    // Zero-copy WAL: write directly from page's data buffer
    const char* row_ptr = page.row_data_ptr(slot);
    uint32_t row_len    = page.row_data_len(slot);

    wal_->log_insert_direct(row.id, page.id, slot, row_ptr, row_len, sync_now);

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
// Delete / update
// ─────────────────────────────────────────────────────────────────────────────

void DB::erase(uint32_t id) {
    WriteLock lock(rwlock_);

    if (!open_) throw XDLError("DB not open");
    if (!erase_unlocked(id))
        throw NotFoundError("Key not found: " + std::to_string(id));
    wal_->sync();
}

size_t DB::erase_batch(const std::vector<uint32_t>& ids) {
    WriteLock lock(rwlock_);

    if (!open_) throw XDLError("DB not open");
    if (ids.empty()) return 0;

    // Validate the whole batch first: unknown or repeated ids must not leave
    // the database half-modified.
    std::unordered_set<uint32_t> seen;
    seen.reserve(ids.size() * 2);
    for (uint32_t id : ids) {
        if (!seen.insert(id).second)
            throw DuplicateKeyError("Duplicate key in erase batch: " +
                                    std::to_string(id));
        if (!index_->find(id))
            throw NotFoundError("Key not found: " + std::to_string(id));
    }

    size_t deleted = 0;
    for (uint32_t id : ids) {
        if (erase_unlocked(id)) ++deleted;
    }
    wal_->sync();
    return deleted;
}

void DB::update(const Row& row) {
    WriteLock lock(rwlock_);

    if (!open_) throw XDLError("DB not open");
    if (!index_->find(row.id))
        throw NotFoundError("Key not found: " + std::to_string(row.id));

    if (row.fields.size() != schema_.fields.size())
        throw XDLError("Row field count (" + std::to_string(row.fields.size()) +
                       ") does not match schema field count (" +
                       std::to_string(schema_.fields.size()) + ")");

    // Validate the replacement before removing the old row.
    uint32_t need = serialised_row_size(row, schema_);
    if (need > PAGE_SIZE)
        throw XDLError("Row too large for a page: " + std::to_string(need) +
                       " bytes (page limit " + std::to_string(PAGE_SIZE) + ")");

    // Delete + insert is deliberately used instead of in-place overwrite:
    // it handles size changes and keeps both index layers correct.  Both WAL
    // records are flushed together at the end.
    if (!erase_unlocked(row.id))
        throw NotFoundError("Key not found: " + std::to_string(row.id));
    insert_unlocked(row, false);
    wal_->sync();
}

size_t DB::update_batch(const std::vector<Row>& rows) {
    WriteLock lock(rwlock_);

    if (!open_) throw XDLError("DB not open");
    if (rows.empty()) return 0;

    std::unordered_set<uint32_t> seen;
    seen.reserve(rows.size() * 2);
    for (const auto& row : rows) {
        if (row.fields.size() != schema_.fields.size())
            throw XDLError("Row field count (" + std::to_string(row.fields.size()) +
                           ") does not match schema field count (" +
                           std::to_string(schema_.fields.size()) + ")");
        if (!seen.insert(row.id).second)
            throw DuplicateKeyError("Duplicate key in update batch: " +
                                    std::to_string(row.id));
        if (!index_->find(row.id))
            throw NotFoundError("Key not found: " + std::to_string(row.id));

        uint32_t need = serialised_row_size(row, schema_);
        if (need > PAGE_SIZE)
            throw XDLError("Row too large for a page: " + std::to_string(need) +
                           " bytes (page limit " + std::to_string(PAGE_SIZE) + ")");
    }

    for (const auto& row : rows) {
        erase_unlocked(row.id);
        insert_unlocked(row, false);
    }
    wal_->sync();
    return rows.size();
}

bool DB::erase_unlocked(uint32_t id) {
    const IndexEntry* entry = index_->find(id);
    if (!entry) return false;

    const uint32_t page_id = entry->page_id;
    Page& page = pager_->get_mutable_page(page_id);

    int slot = -1;
    if (entry->row_slot < page.row_count &&
        page.row_id_at(entry->row_slot) == id) {
        slot = static_cast<int>(entry->row_slot);
    } else {
        slot = page.find_row(id, schema_);
    }
    if (slot < 0) return false;

    // Decode before compaction so secondary-index keys can be removed.
    Row old = page.get_row(static_cast<uint32_t>(slot), schema_);

    wal_->log_delete(id, page_id, static_cast<uint32_t>(slot), false);

    page.erase_row(static_cast<uint32_t>(slot));
    index_->remove(id);
    repair_slots_after_erase(page_id);

    for (auto& [field_name, bptree] : indexes_) {
        int fi = schema_.field_index(field_name);
        if (fi >= 0) {
            bptree->erase(old.fields[static_cast<size_t>(fi)],
                          schema_.fields[static_cast<size_t>(fi)].type,
                          id);
        }
    }
    return true;
}

void DB::repair_slots_after_erase(uint32_t page_id) {
    const auto* ids = index_->ids_for_page(page_id);
    if (!ids) return;

    Page& page = pager_->get_mutable_page(page_id);
    for (uint32_t rid : *ids) {
        int slot = page.find_row(rid, schema_);
        if (slot >= 0)
            index_->update_slot(rid, static_cast<uint32_t>(slot));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Get
// ─────────────────────────────────────────────────────────────────────────────

Row DB::get(uint32_t id) const {
    ReadLock lock(rwlock_);

    if (!open_) throw XDLError("DB not open");
    return get_unlocked(id);
}

Row DB::get_unlocked(uint32_t id) const {
    const IndexEntry* entry = index_->find(id);
    if (!entry)
        throw NotFoundError("Key not found: " + std::to_string(id));

    auto page = pager_->get_page(entry->page_id);
    return page->get_row(entry->row_slot, schema_);
}

bool DB::try_get(uint32_t id, Row& out) const {
    ReadLock lock(rwlock_);

    if (!open_) throw XDLError("DB not open");
    try {
        out = get_unlocked(id);
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
    scan_unlocked(std::move(visitor), filter);
}

void DB::scan_unlocked(std::function<void(const Row&)> visitor,
                       const ScanFilter& filter) const {
    uint32_t total_pages = pager_->page_count();
    for (uint32_t pid = 0; pid < total_pages; ++pid) {
        auto page = pager_->get_page(pid);
        for (uint32_t slot = 0; slot < page->row_count; ++slot) {
            // Zero-copy filter: check against raw bytes without allocating a Row
            if (!filter.matches_raw(page->row_data_ptr(slot), schema_)) continue;
            Row r = page->get_row(slot, schema_);
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
        auto page = pager_->get_page(pid);
        for (uint32_t slot = 0; slot < page->row_count; ++slot) {
            Row r = page->get_row(slot, schema_);
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

// Compare two field values using the same order-preserving encoding as the
// secondary index, so the no-index fallback matches index semantics exactly.
static int compare_field_values(const FieldValue& a, const FieldValue& b,
                                FieldType ft) {
    auto ka = bptree_key_encode_vec(a, ft);
    auto kb = bptree_key_encode_vec(b, ft);
    return bptree_key_compare(ka.data(), ka.size(), kb.data(), kb.size());
}

std::vector<Row> DB::index_range_scan(
    const std::string& field,
    const std::optional<FieldValue>& lo, bool lo_inclusive,
    const std::optional<FieldValue>& hi, bool hi_inclusive) const
{
    ReadLock lock(rwlock_);

    int fi = schema_.field_index(field);
    if (fi < 0) throw XDLError("Unknown field: " + field);

    FieldType ft = schema_.fields[static_cast<size_t>(fi)].type;

    // Documented behaviour: fall back to a full scan when no index exists.
    auto it = indexes_.find(field);
    if (it == indexes_.end()) {
        std::vector<Row> results;
        scan_unlocked([&](const Row& r) {
            const FieldValue& v = r.fields[static_cast<size_t>(fi)];
            if (lo) {
                int c = compare_field_values(v, *lo, ft);
                if (c < 0 || (c == 0 && !lo_inclusive)) return;
            }
            if (hi) {
                int c = compare_field_values(v, *hi, ft);
                if (c > 0 || (c == 0 && !hi_inclusive)) return;
            }
            results.push_back(r);
        }, {});
        return results;
    }

    // Collect locations first, then load each matching page exactly once.
    // The previous implementation copied the full page for every row.
    std::vector<std::pair<uint32_t, uint32_t>> locs; // page_id, row_slot
    it->second->range_scan(
        lo, ft, lo_inclusive, hi, ft, hi_inclusive,
        [&](const std::vector<char>& /*key*/, const std::vector<uint32_t>& ids) {
            for (uint32_t id : ids) {
                const IndexEntry* entry = index_->find(id);
                if (entry) locs.emplace_back(entry->page_id, entry->row_slot);
            }
        });

    std::vector<Row> results(locs.size());
    std::unordered_map<uint32_t, std::vector<std::pair<size_t, uint32_t>>> by_page;
    by_page.reserve(locs.size() / 8 + 1);
    for (size_t i = 0; i < locs.size(); ++i)
        by_page[locs[i].first].emplace_back(i, locs[i].second);

    for (auto& [pid, items] : by_page) {
        auto page = pager_->get_page(pid);
        for (auto& [out_idx, slot] : items)
            results[out_idx] = page->get_row(slot, schema_);
    }
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
        s.cache_size   = pager_->cache_size();
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

Page& DB::get_or_create_writable_page(const Row& row) {
    uint32_t need = serialised_row_size(row, schema_);
    if (need > PAGE_SIZE)
        throw XDLError("Row too large for a page: " + std::to_string(need) +
                       " bytes (page limit " + std::to_string(PAGE_SIZE) + ")");

    uint32_t count = pager_->page_count();
    if (count > 0) {
        Page& last = pager_->get_mutable_page(count - 1);
        if (last.has_capacity() && last.data.size() + need <= PAGE_SIZE)
            return last;
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
                // We need to deserialise the row_bytes into a Row and append it
                // But the WAL stores raw serialised page data (same format as page.data).
                // We can create a temporary page and parse the row from it.
                Page tmp_page(INVALID_PAGE_ID);
                tmp_page.data = std::move(rec.row_data);
                tmp_page.row_count = 1;
                // Build row_offsets manually
                if (!tmp_page.data.empty()) {
                    tmp_page.row_offsets.push_back(0);
                }

                Row r = tmp_page.get_row(0, schema_);
                Page& page = get_or_create_writable_page(r);
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
        } else if (rec.type == WAL::WALRecord::DELETE) {
            // Remove the row from both the page and the primary index.  The
            // page is compacted in memory and marked dirty so the next
            // checkpoint persists the deletion.
            const IndexEntry* entry = index_->find(rec.row_id);
            if (!entry) continue;

            const uint32_t page_id = entry->page_id;
            try {
                Page& page = pager_->get_mutable_page(page_id);
                int slot = -1;
                if (entry->row_slot < page.row_count &&
                    page.row_id_at(entry->row_slot) == rec.row_id) {
                    slot = static_cast<int>(entry->row_slot);
                } else {
                    slot = page.find_row(rec.row_id, schema_);
                }
                if (slot >= 0) {
                    page.erase_row(static_cast<uint32_t>(slot));
                    page.dirty = true;
                }
                index_->remove(rec.row_id);
                repair_slots_after_erase(page_id);
            } catch (...) {
                // Skip corrupt WAL entries
            }
        }
    }
}

} // namespace xdl
