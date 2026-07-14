# XDL API Reference

Complete reference for the public C++ API. Include `<xdl.h>` (or `<xdl/db.h>`) to access everything below.

---

## Namespace

All symbols live in the `xdl` namespace.

---

## Constants  (`include/xdl/types.h`)

| Constant | Type | Value | Description |
|----------|------|-------|-------------|
| `PAGE_SIZE` | `uint32_t` | `8192` | Page size in bytes |
| `MAX_ROWS_PER_PAGE` | `uint32_t` | `64` | Maximum rows per page |
| `DEFAULT_CACHE_SIZE` | `uint32_t` | `256` | Default LRU cache capacity (pages) |
| `INVALID_PAGE_ID` | `uint32_t` | `UINT32_MAX` | Sentinel for "no page" |
| `XDL_MAGIC` | `uint32_t` | `0x58444C31` | File magic `"XDL1"` |
| `XDL_VERSION` | `uint32_t` | `1` | On-disk format version |
| `BPLUS_TREE_ORDER` | `uint32_t` | `32` | B+ tree node order (max keys) |

---

## Enum: `FieldType`

```cpp
enum class FieldType : uint8_t {
    UINT32  = 0,   // 32-bit unsigned integer  (4 bytes)
    STRING  = 1,   // variable-length UTF-8     (2-byte length + data)
    FLOAT32 = 2,   // IEEE 754 float            (4 bytes)
    BOOL    = 3,   // boolean                   (1 byte)
    INT64   = 4,   // 64-bit signed integer     (8 bytes)
};
```

Helper functions:
- `uint32_t field_type_width(FieldType)` — wire size (0 for variable-length)
- `const char* field_type_name(FieldType)` — human-readable name
- `FieldType parse_field_type(const std::string&)` — parse from string (throws `std::invalid_argument`)

---

## Enum: `CompressionType`

```cpp
enum class CompressionType : uint8_t {
    None = 0,
    LZ4  = 1,
};
```

---

## Struct: `Schema`

```cpp
struct Schema {
    std::vector<FieldDef> fields;
    SchemaVersion         ver;    // default version = 1

    int  field_index(const std::string& name) const;
    void validate() const;
};
```

---

## Struct: `FieldDef`

```cpp
struct FieldDef {
    std::string name;
    FieldType   type;
    FieldDef() = default;
    FieldDef(std::string n, FieldType t);
};
```

---

## Type: `FieldValue`

```cpp
using FieldValue = std::variant<uint32_t, std::string, float, bool, int64_t>;
```

---

## Struct: `Row`

```cpp
struct Row {
    uint32_t                id;
    std::vector<FieldValue> fields;

    Row();
    Row(uint32_t id, std::vector<FieldValue> fv);

    // Positional accessors (fast, no name lookup)
    uint32_t           get_uint32 (size_t idx) const;
    const std::string& get_string (size_t idx) const;
    float              get_float32(size_t idx) const;
    bool               get_bool   (size_t idx) const;
    int64_t            get_int64  (size_t idx) const;

    // Named accessors (require Schema, slower)
    uint32_t           get_uint32 (const Schema&, const std::string& name) const;
    const std::string& get_string (const Schema&, const std::string& name) const;
    float              get_float32(const Schema&, const std::string& name) const;
    bool               get_bool   (const Schema&, const std::string& name) const;
    int64_t            get_int64  (const Schema&, const std::string& name) const;

    void set(size_t idx, FieldValue v);
};
```

---

## Struct: `ScanFilter`

```cpp
struct ScanFilter {
    std::unordered_map<std::string, FieldConstraint> constraints;

    ScanFilter& eq    (const std::string& field, uint32_t v);
    ScanFilter& eq    (const std::string& field, std::string v);
    ScanFilter& eq    (const std::string& field, float v);
    ScanFilter& eq    (const std::string& field, bool v);
    ScanFilter& eq    (const std::string& field, int64_t v);
    ScanFilter& min   (const std::string& field, uint32_t v);
    ScanFilter& max   (const std::string& field, uint32_t v);
    ScanFilter& min   (const std::string& field, int64_t v);
    ScanFilter& max   (const std::string& field, int64_t v);
    ScanFilter& min   (const std::string& field, float v);
    ScanFilter& max   (const std::string& field, float v);
    ScanFilter& starts(const std::string& field, std::string v);

    bool matches(const Row& row, const Schema& schema) const;
    bool matches_raw(const char* row_data, const Schema& schema) const;
};
```

All constraints are ANDed.

`matches()` deserialises the row first. `matches_raw()` reads field values directly from the serialised byte buffer without allocating a `Row` object — used internally for zero-copy scanning.

---

## Struct: `Stats`

```cpp
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
```

---

## Class: `DB`

The primary public interface. Thread-safe via reader-writer lock.

### Constructor

```cpp
explicit DB(const std::string& path,
            Schema             schema      = {},
            CompressionType    compression = CompressionType::LZ4,
            size_t             cache_cap   = DEFAULT_CACHE_SIZE,
            SchemaMigrate      migration   = {});
~DB();
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `path` | — | Filesystem path to the `.xdl` database file |
| `schema` | `{age:uint32, name:string}` | Field definitions |
| `compression` | `LZ4` | Compression for new pages |
| `cache_cap` | `256` | LRU cache size in pages |
| `migration` | `{}` | Optional schema migration |

### Lifecycle

```cpp
void open();
void close();
bool is_open() const;
```

### Write Operations

```cpp
void insert(const Row& row);
void insert(uint32_t id, std::vector<FieldValue> field_values);
```

Throws `DuplicateKeyError` on duplicate id. Acquires write lock. Logs to WAL.

### Read Operations

```cpp
Row  get(uint32_t id) const;                        // throws NotFoundError
bool try_get(uint32_t id, Row& out) const;

void scan(std::function<void(const Row&)> visitor,
          const ScanFilter& filter = {}) const;
std::vector<Row> scan_all(const ScanFilter& filter = {}) const;
```

All reads acquire a shared lock. Multiple concurrent readers supported.

### Secondary Indexes

```cpp
void create_index(const std::string& field_name);
void drop_index(const std::string& field_name);
bool has_index(const std::string& field_name) const;

std::vector<Row> index_range_scan(
    const std::string& field,
    const std::optional<FieldValue>& lo, bool lo_inclusive,
    const std::optional<FieldValue>& hi, bool hi_inclusive) const;
```

### Maintenance

```cpp
void checkpoint();
Stats stats() const;
```

---

## Struct: `SchemaMigrate`

```cpp
enum class MigrateOp : uint8_t { ADD = 0, DROP = 1, RENAME = 2 };

struct MigrateEntry {
    MigrateOp   op;
    std::string field_name;    // source field name (ADD/DROP/RENAME)
    std::string new_name;      // new field name (RENAME only)
    FieldType   type;          // field type (ADD only)
    FieldValue  default_value; // default value for existing rows (ADD only)
};

struct SchemaMigrate {
    uint32_t                from_version;
    uint32_t                to_version;
    std::vector<MigrateEntry> ops;
};
```

---

## Class: `SchemaMigrator` (advanced)

```cpp
class SchemaMigrator {
public:
    static Row apply_to_row(const Row& old_row,
                            const Schema& old_schema,
                            const Schema& new_schema,
                            const SchemaMigrate& migration);

    static bool is_compatible(const Schema& old_schema,
                              const Schema& new_schema,
                              const SchemaMigrate& migration);
};
```

`apply_to_row()` maps fields by name from the old schema to the new schema, handles RENAME by copying from the old field, and fills in defaults for ADD columns.

---

## Class: `WAL` (advanced)

```cpp
class WAL {
public:
    explicit WAL(const std::string& wal_path);
    ~WAL();
    void open();
    void close();
    bool is_open() const;

    uint64_t log_insert(uint32_t row_id, uint32_t page_id, uint32_t row_slot,
                        const char* row_data, uint32_t row_data_len);

    // Zero-copy variant: writes directly from caller's buffer.
    uint64_t log_insert_direct(uint32_t row_id, uint32_t page_id, uint32_t row_slot,
                               const char* row_data, uint32_t row_data_len);

    void log_commit();
    void log_checkpoint();

    struct WALRecord {
        enum Type : uint8_t { INSERT = 0, COMMIT = 1, CHECKPOINT = 2, ROLLBACK = 3 };
        Type              type;
        uint32_t          row_id;
        uint32_t          page_id;
        uint32_t          row_slot;
        std::vector<char> row_data;
    };
    std::vector<WALRecord> replay();

    void clear();
    uint64_t file_size() const;
};
```

`log_insert_direct()` is the zero-copy variant used internally by `DB::insert()`. Both methods flush to disk (fsync) before returning.

---

## Class: `BPTree` (advanced)

```cpp
class BPTree {
public:
    void insert(const FieldValue& key, FieldType key_type, uint32_t row_id);
    void erase(const FieldValue& key, FieldType key_type, uint32_t row_id);
    const std::vector<uint32_t>* find(const FieldValue& key,
                                       FieldType key_type) const;
    void range_scan(
        const std::optional<FieldValue>& lo, FieldType lo_type, bool lo_inclusive,
        const std::optional<FieldValue>& hi, FieldType hi_type, bool hi_inclusive,
        std::function<void(const std::vector<char>&,
                           const std::vector<uint32_t>&)> visitor) const;
    bool empty() const;
    size_t size() const;
};
```

Backed by a flat sorted `std::vector<Entry>` with binary search for O(log n) lookups. Uses `KeyBuffer` (32-byte stack allocation) during traversal to avoid heap allocation.

---

## Class: `IndexManager` (advanced)

```cpp
class IndexManager {
public:
    void insert(uint32_t id, IndexEntry entry);
    bool remove(uint32_t id);
    const IndexEntry* find(uint32_t id) const;

    void update_offset(uint32_t id, uint64_t new_offset);
    size_t size() const;

    template<typename Fn> void for_each(Fn&& fn) const;

    // Reverse lookup: all row IDs on a given page
    const std::vector<uint32_t>* page_row_ids(uint32_t page_id) const;
};
```

---

## Class: `PageCache` (advanced)

```cpp
class PageCache {
public:
    using EvictionCb = std::function<void(Page&)>;

    explicit PageCache(size_t capacity, EvictionCb on_evict = nullptr);

    Page* get(uint32_t page_id);
    void  put(Page page);
    void  evict(uint32_t page_id);
    void  flush_all();

    size_t size()     const;
    size_t capacity() const;

    template<typename Fn>
    void for_each_dirty(Fn&& fn);
};
```

The eviction callback fires only for dirty pages.

---

## Struct: `Page` (advanced)

```cpp
struct Page {
    uint32_t              id = INVALID_PAGE_ID;
    uint32_t              row_count = 0;
    uint32_t              data_size = 0;
    bool                  dirty = false;
    std::vector<char>     data;
    std::vector<uint32_t> row_offsets;

    Page();
    explicit Page(uint32_t page_id);

    void   append_row(const Row& row, const Schema& schema);
    Row    get_row(uint32_t idx, const Schema& schema) const;
    int    find_row(uint32_t id, const Schema& schema) const;

    std::vector<char> row_data_copy(uint32_t idx, const Schema& schema) const;

    // Zero-copy access into serialised row bytes
    const char* row_data_ptr(uint32_t idx) const;
    uint32_t    row_data_len(uint32_t idx) const;
};
```

---

## Class: `MmapFile` (advanced)

```cpp
class MmapFile {
public:
    MmapFile();
    ~MmapFile();
    void open(const std::string& path, bool create = false,
              size_t initial_size = 0);
    void close();
    bool is_open() const;
    void read (uint64_t offset, void* dst, size_t len) const;
    void write(uint64_t offset, const void* src, size_t len);
    void ensure_size(size_t min_size);
    void flush();
    size_t   size() const;
    uint8_t* data() const;
    int      fd() const;
};
```

Uses `mmap`/`munmap` on Linux and `CreateFileMapping`/`MapViewOfFile` on Windows.

---

## Exceptions

All inherit from `xdl::XDLError` -> `std::runtime_error`.

| Exception | When thrown |
|-----------|-------------|
| `XDLError` | Generic engine error |
| `CorruptionError` | Bad file magic, CRC failure |
| `DuplicateKeyError` | Insert with existing primary key |
| `NotFoundError` | Get for absent key |
| `IOError` | File read/write failure |
| `CompressionError` | LZ4 failure |
| `MigrationError` | Incompatible schema migration |
| `WALError` | WAL corruption or I/O failure |
