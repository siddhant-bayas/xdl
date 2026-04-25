# XDL — eXtensible Data Layer

> A high-performance, embedded, compressed, page-based storage engine written in modern C++17.
> ⚠️ Note: The engine is currently hardcoded to store `age` and `name`.  
> Dynamic field support is planned and coming soon.
[![Build](https://img.shields.io/badge/build-passing-brightgreen)]()
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue)]()
[![License](https://img.shields.io/badge/license-MIT-blue)]()

---

## ✨ Features

| Feature | Details |
|---|---|
| **Page-based storage** | All data written in fixed-size pages (default 8 KB) |
| **LZ4 compression** | Page-level compression; bundled impl, no hard dependency |
| **In-memory index** | O(1) primary-key lookups via `std::unordered_map` |
| **LRU buffer pool** | Configurable cache; dirty pages flushed on eviction |
| **Crash resilience** | Index rebuilt from disk pages on open; checkpoint API |
| **Zero dependencies** | Compiles with only a C++17 compiler |
| **Interactive CLI** | `xdl <file>` — insert, get, scan, stats |
| **Embeddable library** | Link `xdl_core` and include `<xdl.h>` |

---

## ⚡ Performance (5 000 rows, Release build)

```
Sequential insert 5000        1.51 ms
Checkpoint (flush all)        3.02 ms
Random get ×5000              0.18 ms
Full scan                     0.02 ms
File size                     75 KB  (LZ4 compressed)
```

---

## 🚀 Quick Start

### Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

Or directly with g++:

```bash
g++ -std=c++17 -O2 -Iinclude \
    src/page.cpp src/compression.cpp src/storage.cpp \
    src/cache.cpp src/index.cpp src/pager.cpp src/db.cpp \
    src/main.cpp -o xdl
```

### CLI

```
$ ./xdl mydb.xdl
XDL v1  — mydb.xdl
Type 'help' for commands.

xdl> insert 1 25 alice
Inserted id=1

xdl> insert 2 30 bob
Inserted id=2

xdl> get 1
id=     1  age=  25  name=alice

xdl> scan --min-age 26
id=     2  age=  30  name=bob
1 row(s)

xdl> stats
rows          : 2
pages         : 1
cache capacity: 256 pages
file size     : 452 bytes

xdl> quit
```

### C++ API

```cpp
#include <xdl.h>

int main() {
    xdl::DB db("mydb.xdl");
    db.open();

    // Insert
    db.insert(1, 25, "alice");
    db.insert({2, 30, "bob"});

    // Point lookup
    xdl::Row r = db.get(1);
    std::cout << r.name_str() << "\n";  // alice

    // Filtered scan
    xdl::ScanFilter f;
    f.min_age = 26;
    db.scan([](const xdl::Row& row){
        std::cout << row.id << " " << row.name_str() << "\n";
    }, f);

    db.close();
}
```

---

## 🏗️ Architecture

```
CLI / Application
       │
       ▼
     DB (db.h)          ← public API
       │
   ┌───┴────────┐
   │            │
Pager        IndexManager
   │         (always in memory, O(1))
   ├── PageCache (LRU buffer pool)
   │
StorageEngine (file I/O)
   │
CompressionEngine (LZ4 / None)
   │
  Disk
```

See [`docs/architecture.md`](docs/architecture.md) for the detailed write-up.

---

## 📂 Project Layout

```
xdl/
├── include/
│   ├── xdl.h              ← umbrella include
│   └── xdl/
│       ├── types.h         Row, constants, exceptions
│       ├── page.h          Page struct, PageHeader
│       ├── compression.h   CompressionEngine
│       ├── storage.h       StorageEngine, DBHeader
│       ├── cache.h         PageCache (LRU)
│       ├── index.h         IndexManager
│       ├── pager.h         Pager
│       └── db.h            DB (public entry-point)
├── src/
│   ├── page.cpp
│   ├── compression.cpp
│   ├── storage.cpp
│   ├── cache.cpp
│   ├── index.cpp
│   ├── pager.cpp
│   ├── db.cpp
│   └── main.cpp            CLI
├── tests/
│   └── test_xdl.cpp        14 unit + integration tests
├── bench/
│   └── bench.cpp
├── docs/
│   ├── architecture.md
│   ├── getting_started.md
│   └── api_reference.md
└── CMakeLists.txt
```

---

## 🛣️ Roadmap

- [x] Page-based file storage
- [x] LZ4 page compression
- [x] In-memory index
- [x] LRU buffer pool with dirty-page eviction
- [x] CLI (insert / get / scan / stats)
- [x] Persistence + recovery on reopen
- [ ] WAL (Write-Ahead Logging) for crash safety
- [ ] B+ Tree secondary index
- [ ] Memory-mapped I/O (`mmap`)
- [ ] Schema versioning / migrations
- [ ] Concurrent read access (reader-writer lock)

---

## 📄 License

MIT — see [LICENSE](LICENSE).