#pragma once

#include "types.h"
#include "storage.h"
#include "index.h"
#include "pager.h"
#include "wal.h"
#include "bptree.h"
#include "migrate.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// ScanFilter — predicate for scan operations
//
// Filters are expressed as field-name → value constraints.  All constraints
// must be satisfied for a row to pass (logical AND).
//
// Supported per-field predicates:
//   eq   — field value equals the given value (any type)
//   min  — field value >= given (numeric types)
//   max  — field value <= given (numeric types)
//   prefix — string field starts with given prefix
// ─────────────────────────────────────────────────────────────────────────────

struct FieldConstraint {
    std::optional<uint32_t>    eq_uint;
    std::optional<std::string> eq_str;
    std::optional<float>       eq_float;
    std::optional<bool>        eq_bool;
    std::optional<int64_t>     eq_int64;
    std::optional<uint32_t>    min_uint;
    std::optional<uint32_t>    max_uint;
    std::optional<int64_t>     min_int64;
    std::optional<int64_t>     max_int64;
    std::optional<float>       min_float;
    std::optional<float>       max_float;
    std::optional<std::string> prefix;
};

struct ScanFilter {
    // field name → constraint
    std::unordered_map<std::string, FieldConstraint> constraints;

    // Convenience setters
    ScanFilter& eq    (const std::string& field, uint32_t v)    { constraints[field].eq_uint  = v; return *this; }
    ScanFilter& eq    (const std::string& field, std::string v) { constraints[field].eq_str   = std::move(v); return *this; }
    ScanFilter& eq    (const std::string& field, float v)       { constraints[field].eq_float = v; return *this; }
    ScanFilter& eq    (const std::string& field, bool v)        { constraints[field].eq_bool  = v; return *this; }
    ScanFilter& eq    (const std::string& field, int64_t v)     { constraints[field].eq_int64 = v; return *this; }
    ScanFilter& min   (const std::string& field, uint32_t v)    { constraints[field].min_uint = v; return *this; }
    ScanFilter& max   (const std::string& field, uint32_t v)    { constraints[field].max_uint = v; return *this; }
    ScanFilter& min   (const std::string& field, int64_t v)     { constraints[field].min_int64 = v; return *this; }
    ScanFilter& max   (const std::string& field, int64_t v)     { constraints[field].max_int64 = v; return *this; }
    ScanFilter& min   (const std::string& field, float v)       { constraints[field].min_float = v; return *this; }
    ScanFilter& max   (const std::string& field, float v)       { constraints[field].max_float = v; return *this; }
    ScanFilter& starts(const std::string& field, std::string v) { constraints[field].prefix   = std::move(v); return *this; }

    bool matches(const Row& row, const Schema& schema) const;

    // Zero-copy version: checks filter directly against serialised row bytes
    // without allocating a Row object.
    bool matches_raw(const char* row_data, const Schema& schema) const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Stats — diagnostic snapshot
// ─────────────────────────────────────────────────────────────────────────────

struct Stats {
    size_t   row_count;
    uint32_t page_count;
    size_t   cache_size;
    size_t   cache_capacity;
    uint64_t file_size_bytes;
    uint32_t schema_version;
    bool     wal_active;
    size_t   secondary_index_count;
};

// ─────────────────────────────────────────────────────────────────────────────
// DB  — the single public entry-point for the entire storage engine
//
// Thread-safe: multiple concurrent readers are allowed.  Writers acquire
// an exclusive lock.  Internally uses a reader-writer lock (RWLock).
//
// WAL: every insert is logged to a WAL file before the page is modified.
// On open(), any pending WAL records are replayed to recover from crashes.
//
// Secondary indexes: call create_index(field_name) to build a B+ tree index
// on a field.  Scans on indexed fields use the index for range queries.
//
// Schema migrations: if the on-disk schema version differs from the requested
// schema, a migration is applied (if compatible) during open().
// ─────────────────────────────────────────────────────────────────────────────

class DB {
public:
    // path        — file path for the database (created if absent)
    // schema      — field definitions; must not change across open/close
    //             (unless a migration is provided)
    // compression — default compression for new pages
    // cache_cap   — number of pages to keep in the LRU buffer pool
    // migration   — optional schema migration to apply on version mismatch
    explicit DB(const std::string& path,
                Schema             schema      = {},
                CompressionType    compression = CompressionType::LZ4,
                size_t             cache_cap   = DEFAULT_CACHE_SIZE,
                SchemaMigrate      migration   = {});
    ~DB();

    // Non-copyable
    DB(const DB&)            = delete;
    DB& operator=(const DB&) = delete;

    // ── Schema access ────────────────────────────────────────────────────────

    const Schema& schema() const { return schema_; }

    // ── Lifecycle ────────────────────────────────────────────────────────────

    void open();
    void close();
    bool is_open() const;

    // ── Write operations ─────────────────────────────────────────────────────

    // Insert a row.  Throws DuplicateKeyError if id already exists.
    // Thread-safe: acquires write lock internally.
    void insert(const Row& row);

    // Convenience builder: constructs a Row from id and ordered field values.
    void insert(uint32_t id, std::vector<FieldValue> field_values) {
        insert(Row{id, std::move(field_values)});
    }

    // ── Read operations ──────────────────────────────────────────────────────

    // Point lookup; throws NotFoundError if absent.
    // Thread-safe: acquires read lock internally.
    Row get(uint32_t id) const;

    // Returns false if not found (no-throw variant).
    bool try_get(uint32_t id, Row& out) const;

    // Full scan — visits every row in storage order; optionally filtered.
    // Thread-safe: acquires read lock internally.
    void scan(std::function<void(const Row&)> visitor,
              const ScanFilter& filter = {}) const;

    // Collect into a vector (convenience wrapper around scan).
    std::vector<Row> scan_all(const ScanFilter& filter = {}) const;

    // ── Secondary indexes ────────────────────────────────────────────────────

    // Create a B+ tree secondary index on the named field.
    // Existing rows are indexed immediately.  Inserts after this call
    // update the index automatically.
    void create_index(const std::string& field_name);

    // Drop a secondary index.
    void drop_index(const std::string& field_name);

    // Check if an index exists for a field.
    bool has_index(const std::string& field_name) const;

    // Range scan using a secondary index (if available).
    // Falls back to full scan if no index exists for the field.
    std::vector<Row> index_range_scan(
        const std::string& field,
        const std::optional<FieldValue>& lo, bool lo_inclusive,
        const std::optional<FieldValue>& hi, bool hi_inclusive) const;

    // ── Maintenance ──────────────────────────────────────────────────────────

    // Flush all dirty pages to disk without closing.
    void checkpoint();

    // Diagnostic snapshot.
    Stats stats() const;

private:
    Page& get_or_create_writable_page();

    // Rebuild secondary indexes from disk (called during open).
    void rebuild_indexes();

    // Apply WAL replay after a crash.
    void replay_wal();

    std::string             path_;
    Schema                  schema_;
    CompressionType         compression_;
    size_t                  cache_cap_;
    SchemaMigrate           migration_;

    std::unique_ptr<StorageEngine> storage_;
    std::unique_ptr<IndexManager>  index_;
    std::unique_ptr<Pager>         pager_;
    std::unique_ptr<WAL>           wal_;

    // Secondary indexes: field_name → BPTree
    std::unordered_map<std::string, std::unique_ptr<BPTree>> indexes_;

    // Reader-writer lock for concurrent access
    mutable RWLock rwlock_;

    bool open_ = false;
};

} // namespace xdl
