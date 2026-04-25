# XDL API Reference

Complete reference for the public C++ API.  Include `<xdl.h>` (or `<xdl/db.h>`) to access everything below.

---

## Namespace

All symbols live in the `xdl` namespace.

---

## Constants  (`include/xdl/types.h`)

| Constant | Type | Value | Description |
|----------|------|-------|-------------|
| `PAGE_SIZE` | `uint32_t` | `8192` | Default page size in bytes |
| `MAX_ROWS_PER_PAGE` | `uint32_t` | `64` | Maximum rows a single page can hold |
| `NAME_MAX_LEN` | `uint32_t` | `64` | Maximum name string length (bytes, incl. null) |
| `DEFAULT_CACHE_SIZE` | `uint32_t` | `256` | Default LRU cache capacity in pages |
| `INVALID_PAGE_ID` | `uint32_t` | `UINT32_MAX` | Sentinel for "no page" |
| `XDL_MAGIC` | `uint32_t` | `0x58444C31` | File magic `"XDL1"` |
| `XDL_VERSION` | `uint32_t` | `1` | On-disk format version |

---

## Enum: `CompressionType`

```cpp
enum class CompressionType : uint8_t {
    None = 0,
    LZ4  = 1,
};
```

Controls how pages are compressed before writing to disk.

---

## Struct: `Row`

```cpp
struct Row {
    uint32_t id;
    uint32_t age;
    uint16_t name_len;
    char     name[NAME_MAX_LEN];   // not necessarily null-terminated beyond name_len

    Row();
    Row(uint32_t id, uint32_t age, const std::string& name);

    std::string name_str() const;
    static constexpr size_t serialized_size();  // 74 bytes
};
```

### Members

| Member | Description |
|--------|-------------|
| `id` | Primary key — must be unique within a database |
| `age` | Unsigned age value |
| `name_len` | Actual length of `name` in bytes (≤ 63) |
| `name[64]` | Fixed-width name buffer |

### Methods

#### `std::string name_str() const`
Returns `name` as a `std::string` of length `name_len`.

#### `static constexpr size_t serialized_size()`
Returns `74` — the number of bytes each row occupies on disk.  Stable across all platforms.

---

## Struct: `ScanFilter`

```cpp
struct ScanFilter {
    std::optional<uint32_t>    min_age;
    std::optional<uint32_t>    max_age;
    std::optional<std::string> name_prefix;

    bool matches(const Row& r) const;
};
```

All fields are optional.  When set, rows must satisfy **all** specified conditions to be included in scan results.

| Field | Condition |
|-------|-----------|
| `min_age` | `row.age >= *min_age` |
| `max_age` | `row.age <= *max_age` |
| `name_prefix` | `row.name_str()` starts with `*name_prefix` |

---

## Struct: `Stats`

```cpp
struct Stats {
    size_t   row_count;
    uint32_t page_count;
    size_t   cache_size;
    size_t   cache_capacity;
    uint64_t file_size_bytes;
};
```

Returned by `DB::stats()`.  A lightweight snapshot — no disk I/O required.

---

## Class: `DB`

The primary public interface.  One `DB` instance per file.

```cpp
class DB {
public:
    explicit DB(const std::string& path,
                CompressionType    compression = CompressionType::LZ4,
                size_t             cache_cap   = DEFAULT_CACHE_SIZE);
    ~DB();  // calls close() automatically
```

### Constructor parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `path` | — | Filesystem path to the `.xdl` database file.  Created if absent. |
| `compression` | `LZ4` | Compression to apply to new pages. |
| `cache_cap` | `256` | Number of pages to hold in the LRU buffer pool. |

---

### Lifecycle

#### `void open()`

Opens (or creates) the database file.  If the file already exists, reads the `DBHeader`, validates magic and version, then scans all pages to rebuild the in-memory index.

- **Throws** `IOError` if the file cannot be opened.
- **Throws** `CorruptionError` if the file magic or version does not match.
- Idempotent: calling `open()` on an already-open `DB` is a no-op.

#### `void close()`

Flushes all dirty pages, updates the `DBHeader` page count, and closes the file handle.

- Safe to call multiple times.
- Called automatically by the destructor.

#### `bool is_open() const`

Returns `true` if the database file is currently open.

---

### Write Operations

#### `void insert(const Row& row)`

Insert a new row.

- **Throws** `DuplicateKeyError` if `row.id` already exists.
- **Throws** `XDLError` if the database is not open.

The row is written to the last page that has capacity, or a new page is allocated.  The data remains in the LRU cache as a dirty page until eviction or `checkpoint()`.

#### `void insert(uint32_t id, uint32_t age, const std::string& name)`

Convenience overload.  Equivalent to `insert(Row{id, age, name})`.

Names longer than `NAME_MAX_LEN - 1` bytes are silently truncated.

---

### Read Operations

#### `Row get(uint32_t id) const`

Retrieve a row by primary key.

- **Throws** `NotFoundError` if `id` does not exist.
- **Throws** `XDLError` if the database is not open.
- **Complexity**: O(1) index lookup + O(1) cache probe (or one page decompression on cache miss).

#### `bool try_get(uint32_t id, Row& out) const`

Non-throwing variant.  Writes to `out` and returns `true` on success; returns `false` if not found.

---

#### `void scan(std::function<void(const Row&)> visitor, const ScanFilter& filter = {})`

Visit every row in storage order (page 0 → page N, slot 0 → slot M within each page), calling `visitor` for each row that satisfies `filter`.

- Sequential page access is cache-friendly.
- The visitor is called inline; do not call `insert()` or `close()` from within it.

#### `std::vector<Row> scan_all(const ScanFilter& filter = {})`

Collect all matching rows into a vector.  Convenience wrapper around `scan`.

---

### Maintenance

#### `void checkpoint()`

Flush all dirty pages to disk and update the on-disk `DBHeader.page_count`.

After a crash (without WAL), any insertions since the last `checkpoint()` or `close()` will be lost.  Call `checkpoint()` periodically to bound potential data loss.

#### `Stats stats() const`

Return a diagnostic `Stats` snapshot.  No disk I/O.

---

## Exceptions

All exceptions inherit from `xdl::XDLError` → `std::runtime_error`.

| Exception | When thrown |
|-----------|-------------|
| `XDLError` | Generic engine error (DB not open, etc.) |
| `CorruptionError` | Bad file magic, unsupported version |
| `DuplicateKeyError` | `insert()` with an existing primary key |
| `NotFoundError` | `get()` for a key that does not exist |
| `IOError` | File read/write failure |
| `CompressionError` | LZ4 compress/decompress failure |

---

## Lower-level APIs (advanced)

These are public but considered **internal**.  Prefer `DB` for all normal usage.

### `CompressionEngine`

```cpp
static size_t compress(CompressionType, const char* src, size_t src_size,
                        std::vector<char>& dst);

static void decompress(CompressionType, const char* src, size_t src_size,
                       char* dst, size_t expected_size);

static size_t max_compressed_size(CompressionType, size_t src_size);
static const char* name(CompressionType);
```

### `PageCache`

```cpp
PageCache(size_t capacity, EvictionCb on_evict = nullptr);
Page*  get(uint32_t page_id);
void   put(Page page);
void   evict(uint32_t page_id);
void   flush_all();
```

### `IndexManager`

```cpp
void              insert(uint32_t id, const IndexEntry& entry);
const IndexEntry* find(uint32_t id) const;
bool              remove(uint32_t id);
void              update_offset(uint32_t id, uint64_t new_offset);
size_t            size() const;
```

### `Pager`

```cpp
Pager(StorageEngine&, IndexManager&, size_t cache_capacity);
void   recover();
Page&  get_page(uint32_t page_id);
Page&  new_page();
void   mark_dirty(uint32_t page_id);
void   flush_page(uint32_t page_id);
void   flush_all();
uint32_t page_count() const;
uint64_t page_offset(uint32_t page_id) const;
```

---

## On-disk Format Version History

| Version | Changes |
|---------|---------|
| 1 | Initial release: DBHeader, PageHeader, fixed-width Row v1 |

Files with a version greater than `XDL_VERSION` will throw `CorruptionError` on open.