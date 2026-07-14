# XDL — eXtensible Data Layer

> A high-performance, embedded, compressed, page-based storage engine written in modern C++17.

[![CI](https://github.com/siddhant-bayas/xdl/actions/workflows/ci.yml/badge.svg)](https://github.com/siddhant-bayas/xdl/actions/workflows/ci.yml)
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue)]()
[![License](https://img.shields.io/badge/license-MIT-blue)]()

---

## Features

| Feature | Details |
|---|---|
| **Dynamic schema** | Define any number of `uint32`, `string`, `float32`, `bool`, `int64` fields at DB construction |
| **Page-based storage** | All data written in fixed-size pages (default 8 KB) with fixed-offset slot allocation |
| **LZ4 compression** | Per-page compression; bundled impl, no hard dependency |
| **B+ tree secondary indexes** | O(log n) point lookups and range queries on any indexed field |
| **WAL** | Write-Ahead Logging with CRC32 checksums; automatic crash recovery |
| **Reader-writer lock** | Concurrent reads; exclusive writes |
| **Schema versioning** | Migrations (ADD/DROP/RENAME columns) with automatic schema persistence |
| **Memory-mapped I/O** | `MmapFile` class for mmap-based file access (Linux/Windows) |
| **In-memory primary index** | O(1) primary-key lookups via `std::unordered_map` |
| **LRU buffer pool** | Configurable cache with dirty-page writeback on eviction |
| **Crash resilience** | WAL replay on open; fixed-offset pages prevent stale data |
| **Cross-platform** | Linux (GCC/Clang) and Windows (MSVC) with full CI |
| **CI/CD** | GitHub Actions: Linux + Windows builds, ASan/TSan/UBSan, release packaging |
| **Package-ready** | CMake install, CPack `.deb`/`.rpm`/`.tgz`, CMake config for `find_package(xdl)` |
| **Interactive CLI** | `xdl <file> [--schema ...]` — insert, get, scan, index, stats, checkpoint |
| **Zero dependencies** | Compiles with only a C++17 compiler and pthreads |

---

## Quick Start

### Build (Linux/macOS)

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

### Build (Windows / MSVC)

```cmd
mkdir build_win && cd build_win
cmake .. -G Ninja
cmake --build .
```

Or via Developer Command Prompt:

```cmd
cmake --build build_win --config Release
```

Run the tests:

```bash
./xdl_tests
# Expected: Passed: 139/139
```

---

## CI/CD

GitHub Actions workflows in `.github/workflows/`:

| Workflow | Trigger | What it does |
|----------|---------|-------------|
| **ci.yml** | push / PR to `main` | Linux GCC+Clang (Debug/Release), Windows MSVC, ASan + TSan + UBSan |
| **release.yml** | tag `v*` | Builds static Linux+Windows binaries, creates GitHub Release with `.deb`/`.rpm`/`.tgz` |

---

## CLI Usage

```
$ ./xdl mydb.xdl --schema "age:uint32,name:string,score:float32,active:bool"
XDL v2  — mydb.xdl
Schema v1 (4 field(s)):
  age    : uint32
  name   : string
  score  : float32
  active : bool
Type 'help' for commands.

xdl> insert 1 25 alice 3.14 true
Inserted id=1

xdl> get 1
id=     1  age=    25  name=alice       score=    3.14  active=  true

xdl> scan --min-age 20 --max-age 30
id=     1  age=    25  name=alice       score=    3.14  active=  true
1 row(s)

xdl> index age
Index created on 'age'

xdl> stats
rows          : 1
pages         : 1
cache capacity: 256 pages
file size     : 8277 bytes
schema version: 1
WAL active    : yes
indexes       : 1

xdl> checkpoint
Checkpoint done

xdl> quit
```

### CLI commands

| Command | Description |
|---|---|
| `insert <id> <v1> [<v2> ...]` | Insert a row (values in schema order) |
| `get <id>` | Retrieve row by primary key |
| `scan [--<field> <val>] [--min-<field> N] [--max-<field> N] [--prefix-<field> STR]` | Scan with optional filters |
| `index <field_name>` | Create a B+ tree secondary index |
| `schema` | Display current schema |
| `stats` | Show statistics (rows, pages, WAL, indexes) |
| `checkpoint` | Flush all data to disk, clear WAL |
| `help` | Show help |
| `quit` / `exit` | Close and exit |

When `--schema` is omitted, the default schema `{age:uint32, name:string}` is used for backward compatibility.

---

## C++ API

```cpp
#include <xdl.h>

int main() {
    xdl::Schema schema = {
        {"age",    xdl::FieldType::UINT32},
        {"name",   xdl::FieldType::STRING},
        {"score",  xdl::FieldType::FLOAT32},
        {"active", xdl::FieldType::BOOL},
        {"big_id", xdl::FieldType::INT64},
    };

    xdl::DB db("mydb.xdl", schema);
    db.open();

    // Insert
    db.insert(xdl::Row{1, {
        xdl::FieldValue{uint32_t(25)},
        xdl::FieldValue{std::string("alice")},
        xdl::FieldValue{3.14f},
        xdl::FieldValue{true},
        xdl::FieldValue{int64_t(999999999LL)},
    }});

    // Point lookup
    xdl::Row r = db.get(1);
    r.get_uint32(schema, "age");     // 25
    r.get_string(schema, "name");    // "alice"

    // Faster positional access (no name lookup)
    r.get_uint32(0);   // age
    r.get_string(1);   // name

    // Create a secondary index
    db.create_index("age");

    // Range scan using the index
    auto rows = db.index_range_scan("age",
        std::optional<xdl::FieldValue>{uint32_t(20)}, true,
        std::optional<xdl::FieldValue>{uint32_t(30)}, true);

    // Full scan with zero-copy filter (no deserialisation)
    xdl::ScanFilter f;
    f.min("age", uint32_t(20)).starts("name", "a");
    db.scan([](const xdl::Row& row) {
        std::cout << row.get_string(1) << "\n";
    }, f);

    // Checkpoint (flush to disk, clear WAL)
    db.checkpoint();
    db.close();
}
```

---

## Architecture

```
Application / CLI
       │
       ▼
     DB (db.h)              ← public API; holds Schema, RWLock
       │
   ┌───┴────────────────────────────┐
   │                                │
Pager                         WAL (write-ahead log)
   │    (fixed-offset pages)        │    (CRC32, fsync)
   ├── PageCache (LRU buffer pool)  │
   │                                │
   ├── IndexManager (unordered_map) │
   │                                │
   └── BPTree (secondary indexes)   │
                                    │
StorageEngine ◄─────────────────────┘
   │  (POSIX fd-based I/O, full Windows support)
   └── CompressionEngine (LZ4 / None)
          │
         Disk
```

See [`docs/architecture.md`](docs/architecture.md) for the full design document.

---

## On-Disk File Format

```
Offset 0      ┌────────────────────┐  20 bytes
               │ DBHeader           │
               │  magic: 0x58444C32 │
               │  version: 2        │
               │  page_size: 8192   │
               │  page_count: N     │
               │  compression: LZ4  │
               └────────────────────┘

Offset 20     ┌────────────────────┐  4 + N bytes
               │ Schema blob         │
               │  length: uint32    │
               │  data: serialized  │
               └────────────────────┘

Offset schema_end
               ┌────────────────────┐  slot_size = 20 + 8192
               │ Page 0 header       │  20 bytes
               │  page_id, sizes,   │
               │  row_count, type   │
               ├────────────────────┤
               │ Compressed data     │  up to 8192 bytes
               └────────────────────┘
               ┌────────────────────┐
               │ Page 1 header       │  (next slot)
               ├────────────────────┤
               │ Compressed data     │
               └────────────────────┘
               ...

WAL file (.xdl.wal):
               ┌────────────────────┐  32 bytes
               │ WAL header          │
               │  magic: 0x57414C31 │
               │  version, page_size│
               │  checksum          │
               └────────────────────┘
               ┌────────────────────┐
               │ Record: CRC32(4)   │
               │  length(4)         │
               │  type(1)           │
               │  row_id(4)         │
               │  page_id(4)        │
               │  row_slot(4)       │
               │  row_data(N)       │
               └────────────────────┘
               [more records...]
```

Each page occupies a fixed slot of `sizeof(PageHeader) + PAGE_SIZE` bytes, so pages can always be overwritten in place regardless of compressed size changes.

---

## Project Layout

```
xdl/
├── include/
│   ├── xdl.h              ← umbrella include
│   └── xdl/
│       ├── types.h         Schema, FieldDef, FieldType, FieldValue, Row, RWLock, exceptions
│       ├── page.h          Page struct, serialisation helpers
│       ├── compression.h   CompressionEngine
│       ├── storage.h       StorageEngine, DBHeader
│       ├── cache.h         PageCache (LRU)
│       ├── index.h         IndexManager
│       ├── pager.h         Pager (coordinates cache, storage, WAL)
│       ├── wal.h           WAL (Write-Ahead Log)
│       ├── bptree.h        B+ Tree secondary index (flat sorted vector)
│       ├── mmap.h          MmapFile (memory-mapped I/O, Linux + Windows)
│       ├── migrate.h       Schema migration
│       └── db.h            DB, ScanFilter (public entry-point)
├── src/
│   ├── page.cpp
│   ├── compression.cpp
│   ├── storage.cpp
│   ├── cache.cpp
│   ├── index.cpp
│   ├── pager.cpp
│   ├── db.cpp
│   ├── wal.cpp
│   ├── bptree.cpp
│   ├── mmap.cpp
│   ├── migrate.cpp
│   └── main.cpp            CLI
├── tests/
│   └── test_xdl.cpp        139 unit + integration tests
├── bench/
│   └── bench.cpp
├── cmake/
│   └── xdl-config.cmake.in  CMake package config template
├── docs/
│   ├── architecture.md
│   ├── api_reference.md
│   └── getting_started.md
├── .github/
│   └── workflows/
│       ├── ci.yml           CI: Linux + Windows, sanitizers
│       └── release.yml      Release: static binaries, .deb/.rpm/.tgz
├── CMakeLists.txt           CMake build (install + CPack)
├── LICENSE                  MIT
├── README.md
└── .gitignore
```

---

## License

MIT — see [LICENSE](LICENSE).
