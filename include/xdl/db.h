#pragma once

#include "types.h"
#include "storage.h"
#include "index.h"
#include "pager.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <optional>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// ScanFilter — predicate for scan operations
// ─────────────────────────────────────────────────────────────────────────────

struct ScanFilter {
    std::optional<uint32_t> min_age;
    std::optional<uint32_t> max_age;
    std::optional<std::string> name_prefix;

    bool matches(const Row& r) const;
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
};

// ─────────────────────────────────────────────────────────────────────────────
// DB  — the single public entry-point for the entire storage engine
//
//  Usage:
//    DB db("mydb.xdl");
//    db.open();
//    db.insert({1, 25, "alice"});
//    auto row = db.get(1);
//    db.scan([](const Row& r){ ... });
//    db.close();
// ─────────────────────────────────────────────────────────────────────────────

class DB {
public:
    // path        — file path for the database (created if absent)
    // compression — default compression for new pages
    // cache_cap   — number of pages to keep in the LRU buffer pool
    explicit DB(const std::string& path,
                CompressionType    compression = CompressionType::LZ4,
                size_t             cache_cap   = DEFAULT_CACHE_SIZE);
    ~DB();

    // Non-copyable
    DB(const DB&)            = delete;
    DB& operator=(const DB&) = delete;

    // ── Lifecycle ────────────────────────────────────────────────────────────

    void open();
    void close();
    bool is_open() const;

    // ── Write operations ─────────────────────────────────────────────────────

    // Insert a row.  Throws DuplicateKeyError if id already exists.
    void insert(const Row& row);

    // Convenience overload
    void insert(uint32_t id, uint32_t age, const std::string& name) {
        insert(Row{id, age, name});
    }

    // ── Read operations ──────────────────────────────────────────────────────

    // Point lookup; throws NotFoundError if absent.
    Row get(uint32_t id) const;

    // Returns false if not found (no-throw variant)
    bool try_get(uint32_t id, Row& out) const;

    // Full scan — visits every row in storage order; optionally filtered
    void scan(std::function<void(const Row&)> visitor,
              const ScanFilter& filter = {}) const;

    // Collect into a vector (convenience wrapper around scan)
    std::vector<Row> scan_all(const ScanFilter& filter = {}) const;

    // ── Maintenance ──────────────────────────────────────────────────────────

    // Flush all dirty pages to disk without closing
    void checkpoint();

    // Diagnostic snapshot
    Stats stats() const;

private:
    // Find or create a page with capacity for one more row
    Page& get_or_create_writable_page();

    std::string             path_;
    CompressionType         compression_;
    size_t                  cache_cap_;

    std::unique_ptr<StorageEngine> storage_;
    std::unique_ptr<IndexManager>  index_;
    std::unique_ptr<Pager>         pager_;

    bool open_ = false;
};

} // namespace xdl