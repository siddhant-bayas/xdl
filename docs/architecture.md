# XDL Architecture

## Overview

XDL is a layered, embedded key-value store designed around three core ideas:

1. **Pages** — the unit of I/O; never read less than one page.
2. **Compression** — applied per page at the storage boundary, so the rest of the engine always operates on plain bytes.
3. **In-memory index** — a hash map from primary key → (page_id, slot), enabling O(1) point lookups without a single disk touch.

---

## Component Map

```
┌──────────────────────────────────────────────────────────┐
│                   Application / CLI                       │
└────────────────────────┬─────────────────────────────────┘
                         │
┌────────────────────────▼─────────────────────────────────┐
│                    DB  (db.h / db.cpp)                    │
│  The single public entry-point.  Owns all subsystems.     │
└───┬──────────────┬──────────────────────────────────────-─┘
    │              │
    │         ┌────▼──────────────────────────┐
    │         │  IndexManager  (index.h)       │
    │         │  std::unordered_map<id,Entry>  │
    │         │  O(1) lookup, never on disk    │
    │         └───────────────────────────────┘
    │
┌───▼───────────────────────────────────────────────────────┐
│                    Pager  (pager.h)                        │
│  Coordinates cache, storage, and compression.             │
│  The ONLY component that knows about both the cache        │
│  and the disk.                                            │
├──────────────────┬────────────────────────────────────────┤
│ PageCache (LRU)  │  StorageEngine  (storage.h)            │
│  In-memory pool  │  fstream wrapper                       │
│  dirty-on-evict  │  read/write raw byte buffers           │
└──────────────────┴──────────┬─────────────────────────────┘
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
  └─ IndexManager::find(id)           // guard: reject duplicates
  └─ DB::get_or_create_writable_page()
       └─ Pager::get_page(last_pid)   // cache hit → return
          or Pager::new_page()        // allocate fresh
  └─ Page::append_row(row)            // serialise into page.data
  └─ Page::dirty = true
  └─ IndexManager::insert(id, {page_id, offset=UINT64_MAX, slot})
       // offset filled in when page is flushed
```

### Get

```
DB::get(id)
  └─ IndexManager::find(id)   → IndexEntry{page_id, offset, slot}
  └─ Pager::get_page(page_id)
       └─ PageCache::get(page_id)  → Page* (cache hit)
          or StorageEngine::read_page_raw(offset)
             → CompressionEngine::decompress(blob)
             → PageCache::put(page)
  └─ Page::get_row(slot)
```

### Scan

```
DB::scan(visitor, filter)
  for pid in [0, page_count):
    └─ Pager::get_page(pid)      // cache-friendly sequential access
    for slot in [0, row_count):
      └─ Page::get_row(slot)
      └─ ScanFilter::matches(row) → visitor(row)
```

### Flush / Checkpoint

```
Pager::flush_all()
  for each dirty page in cache:
    └─ CompressionEngine::compress(page.data)
    └─ StorageEngine::append_page_raw(header, blob)   // new location
       or overwrite_page_raw(offset, …)               // fits in-place
    └─ Update page_offsets_[page_id]
    └─ IndexManager::update_offset(all rows on page)
  └─ StorageEngine::write_db_header(page_count)
```

---

## On-Disk File Format

```
Offset 0
┌─────────────────────────────────┐  ← 20 bytes
│ DBHeader                        │
│  magic       : 0x58444C31 XDL1  │
│  version     : 1                │
│  page_size   : 8192             │
│  page_count  : N                │
│  compression : LZ4 / None       │
└─────────────────────────────────┘

Offset 20
┌─────────────────────────────────┐  ← 20-byte PageHeader
│ PageHeader                      │
│  page_id           : uint32     │
│  compressed_size   : uint32     │
│  uncompressed_size : uint32     │
│  row_count         : uint32     │
│  compression_type  : uint8      │
│  _pad              : [3]        │
├─────────────────────────────────┤
│ CompressedBlob                  │  ← compressed_size bytes
│ (LZ4-compressed row data)       │
└─────────────────────────────────┘

[Page 2 header + blob]
[Page 3 header + blob]
...
```

### In-Memory Page (uncompressed layout)

```
page.data = [ Row0_bytes ][ Row1_bytes ] ... [ RowN_bytes ]
```

Each row is serialised as:

```
[ id: 4 bytes ][ age: 4 bytes ][ name_len: 2 bytes ][ name: 64 bytes ]
= 74 bytes per row  (Row::serialized_size())
```

All integer fields are little-endian. No struct padding on disk — explicit byte-by-byte serialisation is used.

---

## Index Design

```cpp
struct IndexEntry {
    uint32_t page_id;
    uint64_t page_offset;  // file byte offset of the PageHeader
    uint32_t row_slot;     // 0-based position within the page
};
std::unordered_map<uint32_t, IndexEntry> index_;
```

The index is **never written to disk**. On `open()`, `Pager::recover()` scans the file linearly, decompresses each page, reads all row IDs, and rebuilds the index. For typical databases (< 1 M rows) this takes milliseconds.

For very large datasets, a persistent index (WAL or a B+ tree file) is the next architectural step — the interface is already abstracted enough to swap it in.

---

## Cache (Buffer Pool)

`PageCache` is an LRU doubly-linked-list + hash-map (`std::list<Page>` + `std::unordered_map<uint32_t, iterator>`).

- **Get**: O(1) lookup, splice to front.
- **Put**: O(1) insert at front; evict from back if over capacity.
- **Eviction callback**: before a dirty page leaves the cache, the Pager's `on_cache_evict` lambda compresses and writes it to disk.

This means the cache is *self-flushing* — explicit `checkpoint()` is optional; it just gives a stronger durability guarantee.

---

## Compression

`CompressionEngine` is a **stateless** utility class. It:

- Compresses a `char*` buffer → `std::vector<char>` (returns actual compressed size).
- Decompresses a `char*` blob → pre-allocated `char*` (caller knows `uncompressed_size` from the header).
- Falls back to a bundled minimal LZ4 implementation if the system library is absent.

The compression boundary is deliberately **only at the storage layer**. The Pager, Cache, and execution engine always work with uncompressed `Page::data`.

---

## Error Hierarchy

```
std::runtime_error
  └─ xdl::XDLError
       ├─ CorruptionError   — bad magic / version mismatch
       ├─ DuplicateKeyError — insert on existing id
       ├─ NotFoundError     — get on absent id
       ├─ IOError           — file read/write failures
       └─ CompressionError  — LZ4 compress/decompress failure
```

---

## Threading Model

XDL v1 is **single-threaded** — no internal locking. Applications requiring concurrent writes should serialise access externally (e.g. a single writer thread + message queue). A reader-writer lock at the DB level is on the roadmap.

---

## Planned Enhancements

| Feature | Benefit |
|---------|---------|
| Write-Ahead Logging | Crash safety without checkpoint |
| B+ Tree index | Range queries, ordered iteration |
| `mmap` I/O | Reduce syscall overhead for large files |
| Secondary indexes | Query by age, name, etc. |
| Compaction | Reclaim space from overwritten pages |
| Schema versioning | Forward/backward compatible migrations |