# XDL Architecture

## Overview

XDL is a layered, embedded storage engine built around these core ideas:

1. **Fixed-offset pages** — each page occupies a fixed-size slot on disk, enabling in-place updates regardless of compression ratio changes.
2. **Compression** — applied per page at the storage boundary; the rest of the engine always operates on plain bytes.
3. **In-memory primary index** — a hash map from primary key → (page_id, slot), enabling O(1) point lookups.
4. **Write-Ahead Log** — every mutation is logged to a WAL file before the page is modified; crash recovery replays the WAL on open.
5. **Reader-writer lock** — multiple concurrent readers are allowed; writers acquire an exclusive lock.
6. **Cross-platform** — full Windows and Linux support via `#ifdef` portability layer for POSIX I/O, mmap, and binary file flags.

---

## Component Map

```
┌──────────────────────────────────────────────────────────────┐
│                    Application / CLI                          │
└──────────────────────────┬───────────────────────────────────┘
                           │
┌──────────────────────────▼───────────────────────────────────┐
│                  DB  (db.h / db.cpp)                          │
│  Public entry-point. Owns all subsystems.                    │
│  Thread-safe via RWLock (shared reads, exclusive writes).    │
└───┬──────────┬───────────┬───────────┬──────────────────────┘
    │          │           │           │
    │     ┌────▼────┐  ┌───▼───┐  ┌───▼──────┐
    │     │  WAL    │  │ B+Tree│  │ IndexMgr │
    │     │ (wal.h) │  │(bp.h) │  │(index.h) │
    │     └─────────┘  └───────┘  └──────────┘
    │
┌───▼──────────────────────────────────────────────────────────┐
│                  Pager  (pager.h / pager.cpp)                 │
│  Coordinates cache, storage, compression, and WAL.           │
│  The ONLY component that touches both cache and disk.        │
├──────────────────┬───────────────────────────────────────────┤
│ PageCache (LRU)  │  StorageEngine  (storage.h)               │
│  In-memory pool  │  POSIX fd-based I/O (pread/pwrite)        │
│  dirty-on-evict  │  Thread-safe via mutex on Windows         │
└──────────────────┴──────────┬────────────────────────────────┘
                              │
                   ┌──────────▼──────────────┐
                   │  CompressionEngine       │
                   │  LZ4 (or None)           │
                   │  applied at page boundary│
                   └──────────┬──────────────┘
                              │
                           Disk
```

---

## Data Flow

### Insert

```
DB::insert(row)
  └─ WriteLock (exclusive)
  └─ WAL::log_insert(row.id, page_id, slot, row_data)   ← fsync
  └─ IndexManager::find(id)           ← guard: reject duplicates
  └─ DB::get_or_create_writable_page()
       └─ Pager::get_page(last_pid)   ← cache hit → return
          or Pager::new_page()         ← allocate fresh slot
  └─ Page::append_row(row)             ← serialise into page.data
  └─ IndexManager::insert(id, {page_id, offset, slot})
  └─ WAL::log_commit()                ← fsync
  └─ BPTree::insert() for each secondary index
```

### Get

```
DB::get(id)
  └─ ReadLock (shared)
  └─ IndexManager::find(id)   → IndexEntry{page_id, offset, slot}
  └─ Pager::get_page(page_id)
       └─ PageCache::get(page_id)  → Page* (cache hit)
          or StorageEngine::read_page_raw(offset)
             → CompressionEngine::decompress(blob)
             → PageCache::put(page)
  └─ Page::get_row(slot)
```

### Scan (zero-copy filter)

```
DB::scan(visitor, filter)
  └─ ReadLock (shared)
  for pid in [0, page_count):
    └─ Pager::get_page(pid)      ← cache-friendly sequential access
    for slot in [0, row_count):
      └─ ScanFilter::matches_raw(page.row_data_ptr(slot), schema)
         → visitor(row)           ← only deserialises on match
```

### Checkpoint

```
DB::checkpoint()
  └─ WriteLock (exclusive)
  └─ Pager::flush_all()
       for each dirty page in cache:
         └─ CompressionEngine::compress(page.data)
         └─ StorageEngine::overwrite_page_raw(offset, header, blob)
            (fixed-offset slot; ftruncate if growing)
         └─ IndexManager::update_offset(all rows on page)
       └─ StorageEngine::write_db_header(page_count)
  └─ WAL::log_checkpoint()
  └─ WAL::clear()               ← destroy and recreate WAL file
```

### Crash Recovery (on open)

```
DB::open()
  └─ StorageEngine::open()
  └─ StorageEngine::write_schema(schema)   ← persist schema
  └─ WAL::open()
  └─ Pager::recover()
       for pid in [0, page_count):
         └─ read page at fixed offset (data_start + pid * slot_size)
         └─ decompress, rebuild index entries
  └─ WAL::replay()
       for each INSERT record:
         └─ skip if row already exists (idempotent)
         └─ reconstruct row from serialised data
         └─ insert into page + index + secondary indexes
```

---

## On-Disk File Format

### Main database file (.xdl)

```
Offset 0       ┌─────────────────────┐  20 bytes
                │ DBHeader             │
                │  magic: 0x58444C31  │
                │  version: 1         │
                │  page_size: 8192    │
                │  page_count: N      │
                │  compression: 0/1   │
                └─────────────────────┘

Offset 20      ┌─────────────────────┐
                │ Schema blob          │  4 + schema_len bytes
                │  length: uint32     │
                │  data: serialised   │
                └─────────────────────┘

Offset schema_end (= data_start)
                ┌─────────────────────┐  slot_size = 20 + 8192 = 8212
                │ Page 0: header (20) │
                │   page_id, sizes,   │
                │   row_count, type   │
                ├─────────────────────┤
                │ Compressed data     │  up to 8192 bytes
                │ (zero-padded to     │
                │  fill the slot)     │
                └─────────────────────┘
                ┌─────────────────────┐
                │ Page 1: header      │
                ├─────────────────────┤
                │ Compressed data     │
                └─────────────────────┘
                ...
```

Each page slot is `sizeof(PageHeader) + PAGE_SIZE = 8212` bytes. Pages are always overwritten in place at their fixed offset, so there is never stale data from previous versions.

### WAL file (.xdl.wal)

```
Offset 0       ┌─────────────────────┐  32 bytes
                │ WAL header           │
                │  magic: 0x57414C31  │
                │  version: 1         │
                │  page_size: 8192    │
                │  checksum: CRC32    │
                │  padding: 12 bytes  │
                └─────────────────────┘

Offset 32      ┌─────────────────────┐
                │ Record               │
                │  CRC32: 4 bytes     │
                │  total_len: 4 bytes │
                │  type: 1 byte       │
                │    0=INSERT          │
                │    1=COMMIT          │
                │    2=CHECKPOINT      │
                │    3=ROLLBACK        │
                │  row_id: 4 bytes    │  (INSERT only)
                │  page_id: 4 bytes   │  (INSERT only)
                │  row_slot: 4 bytes  │  (INSERT only)
                │  row_data: N bytes  │  (INSERT only)
                └─────────────────────┘
                [more records...]
```

Each record is checksummed with CRC32. On replay, any trailing corrupted/truncated record is silently discarded.

---

## Row Serialisation (on-disk wire format)

Each row is stored as a variable-length record inside a page:

```
Offset  Size  Description
──────  ────  ───────────────────────────────────────────
0       4     total row byte length (includes this header)
4       4     row id (uint32, little-endian)
8       …     fields, in schema order:
                 UINT32  → 4 bytes (little-endian)
                 STRING  → 2-byte length N + N bytes UTF-8
                 FLOAT32 → 4 bytes (IEEE 754, memcpy)
                 BOOL    → 1 byte (0 or 1)
                 INT64   → 8 bytes (little-endian)
```

The `Page` struct maintains a `row_offsets[]` table so that `get_row(idx)` is O(1) without scanning the flat buffer. It also exposes `row_data_ptr(idx)` and `row_data_len(idx)` for zero-copy access to the serialised bytes.

---

## Index Design

### Primary Index

```cpp
struct IndexEntry {
    uint32_t page_id;
    uint64_t page_offset;  // file byte offset of the page slot
    uint32_t row_slot;     // 0-based position within the page
};
std::unordered_map<uint32_t, IndexEntry> index_;
std::unordered_map<uint32_t, std::vector<uint32_t>> page_to_ids_;
```

The primary index is rebuilt from disk on `open()` by scanning all pages. The `page_to_ids_` reverse mapping enables O(1) lookup of all row IDs on a given page (used during checkpoint).

### Secondary Indexes (B+ Tree)

```cpp
class BPTree {
    // In-memory flat sorted vector keyed by serialised field values.
    // Keys use order-preserving encoding so that memcmp ordering
    // matches the natural sort order of the field type.
    // Binary search gives O(log n) lookups; append + sort for bulk insert.
    struct Entry {
        std::vector<char>    key;
        std::vector<uint32_t> row_ids;
    };
    std::vector<Entry> entries_;   // sorted by key
};
```

- `insert(key, key_type, row_id)` — O(n) insert maintaining sorted order
- `erase(key, key_type, row_id)` — O(n) erase
- `find(key, key_type)` → `vector<uint32_t>*` — O(log n) binary search
- `range_scan(lo, lo_type, lo_inc, hi, hi_type, hi_inc, visitor)` — O(log n + k)

Key encoding is order-preserving:
- UINT32: big-endian 4 bytes
- INT64: sign-flipped big-endian 8 bytes
- FLOAT32: IEEE 754 with sign-bit flip for correct ordering
- BOOL: 0x00 or 0x01
- STRING: raw UTF-8 bytes (lexicographic)

A `KeyBuffer` (32-byte stack-allocated `std::array`) is used to avoid heap allocation during tree traversal.

---

## Cache (Buffer Pool)

`PageCache` is an LRU with `std::list<Page>` + `std::unordered_map<uint32_t, iterator>`. All methods are protected by `std::mutex` for thread safety.

- **Get**: O(1) lookup, splice to front.
- **Put**: O(1) insert at front; evict from back if over capacity. Copies `row_offsets` when updating an existing page.
- **Eviction callback**: before a dirty page leaves the cache, the Pager's `on_cache_evict` lambda compresses and writes it to disk.

The cache is self-flushing — explicit `checkpoint()` is optional but provides a stronger durability guarantee.

---

## Compression

`CompressionEngine` is a stateless utility class:

- Compresses a `char*` buffer → `std::vector<char>` (returns actual compressed size). Uses thread-local scratch buffer to avoid repeated allocations.
- Decompresses a `char*` blob → pre-allocated `char*` (caller knows `uncompressed_size` from the header).
- Falls back to a bundled minimal LZ4 implementation if the system library is absent.

The compression boundary is only at the storage layer. The Pager, Cache, and DB always work with uncompressed `Page::data`.

---

## WAL (Write-Ahead Log)

The WAL uses POSIX fd-based I/O (not `std::fstream`) for reliable `fsync`, with a full Windows portability layer (`_open`/`_write`/`_commit`, `O_BINARY` flag):

- **log_insert()** — writes INSERT record with CRC32, then `flush` + `fsync`
- **log_insert_direct()** — zero-copy variant: takes raw serialised row bytes directly
- **log_commit()** — writes COMMIT marker
- **log_checkpoint()** — writes CHECKPOINT marker + fsync
- **replay()** — reads all records, validates CRC32, returns vector of WALRecords
- **clear()** — unlinks and recreates the WAL file

Buffered writes: records are accumulated in a 256 KB write buffer and flushed as a batch. On Windows, `pread`/`pwrite` are emulated with `lseek` + `read`/`write` protected by a `std::mutex`.

On `DB::open()`, after the pager recovers pages from the main DB file, any pending WAL records are replayed to recover uncommitted insertions from a crash.

---

## Concurrency

`DB` uses a reader-writer lock (`std::shared_mutex` wrapper):

- `get()`, `scan()`, `stats()` → `ReadLock` (shared) — multiple concurrent readers
- `insert()`, `checkpoint()`, `create_index()`, `drop_index()` → `WriteLock` (exclusive)

The WAL file handle is held by the DB and shared with the Pager. WAL writes are serialized by the DB's write lock. The `PageCache` uses its own `std::mutex` for thread-safe access.

---

## Error Hierarchy

```
std::runtime_error
  └─ xdl::XDLError
       ├─ CorruptionError     — bad magic / version mismatch / CRC failure
       ├─ DuplicateKeyError   — insert on existing id
       ├─ NotFoundError       — get on absent id
       ├─ IOError             — file read/write failures
       ├─ CompressionError    — LZ4 compress/decompress failure
       ├─ MigrationError      — incompatible schema migration
       └─ WALError            — WAL corruption or I/O failure
```

---

## Schema Migration

Schemas are versioned. On `open()`, if the on-disk schema version differs from the requested schema, a migration is applied:

```cpp
xdl::SchemaMigrate migration;
migration.from_version = 1;
migration.to_version   = 2;
migration.ops.push_back({
    xdl::MigrateOp::ADD,
    "score",              // new column name
    "",                   // (not used for ADD)
    xdl::FieldType::FLOAT32,
    xdl::FieldValue{0.0f} // default value for existing rows
});
```

Supported operations: ADD column (with default), DROP column, RENAME column (copies data from old field name to new).

---

## Windows Portability

XDL supports full native Windows builds via MSVC. Key portability measures:

| Area | Linux | Windows |
|------|-------|---------|
| File I/O | `open`/`read`/`write`/`fsync` | `_open`/`_read`/`_write`/`_commit` with `O_BINARY` |
| mmap | `mmap`/`munmap`/`madvise` | `CreateFileMapping`/`MapViewOfFile` |
| Threaded pwrite | `pread()` (atomic) | `lseek` + `write` with `std::mutex` |
| ssize_t | POSIX | `typedef SSIZE_T ssize_t` via `<BaseTsd.h>` |
| File deletion | `unlink()` | `_unlink()` |

All `#ifdef _WIN32` guards are in `wal.cpp`, `storage.cpp`, and `mmap.cpp`.
