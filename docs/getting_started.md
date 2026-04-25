# Getting Started with XDL

This guide walks you from zero to a working embedded database in under five minutes.

---

## Prerequisites

| Tool | Minimum version |
|------|----------------|
| C++ compiler | GCC 9 / Clang 10 / MSVC 2019 (C++17) |
| CMake (optional) | 3.16 |

No other dependencies are required. XDL bundles a compact LZ4 implementation and will automatically use the system `liblz4` if CMake detects it.

---

## 1. Build

### With CMake (recommended)

```bash
git clone https://github.com/yourorg/xdl.git
cd xdl
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

This produces:

- `build/xdl`        — interactive CLI
- `build/xdl_tests`  — test suite
- `build/libxdl_core.a` — static library to link against

### Without CMake

```bash
g++ -std=c++17 -O2 -Iinclude \
    src/page.cpp src/compression.cpp src/storage.cpp \
    src/cache.cpp src/index.cpp src/pager.cpp src/db.cpp \
    src/main.cpp -o xdl
```

### Run the tests

```bash
./build/xdl_tests
# Expected: Passed: 14/14
```

---

## 2. Using the CLI

The `xdl` binary opens (or creates) a database file and accepts commands interactively or from stdin.

```bash
./xdl mydata.xdl
```

### Available commands

| Command | Description |
|---------|-------------|
| `insert <id> <age> <name>` | Insert a new row |
| `get <id>` | Retrieve a row by primary key |
| `scan [--min-age N] [--max-age N] [--name-prefix STR]` | Scan all rows, optionally filtered |
| `stats` | Show row count, page count, file size |
| `checkpoint` | Flush all dirty pages to disk |
| `help` | Show command reference |
| `quit` / `exit` | Save and exit |

### Example session

```
xdl> insert 1 28 alice
Inserted id=1
xdl> insert 2 34 bob
Inserted id=2
xdl> insert 3 22 carol
Inserted id=3
xdl> scan
id=     1  age=  28  name=alice
id=     2  age=  34  name=bob
id=     3  age=  22  name=carol
3 row(s)
xdl> scan --name-prefix bo
id=     2  age=  34  name=bob
1 row(s)
xdl> get 1
id=     1  age=  28  name=alice
xdl> stats
rows          : 3
pages         : 1
cache capacity: 256 pages
file size     : 452 bytes
xdl> quit
```

### Batch / scripted usage

```bash
printf "insert 1 25 alice\ninsert 2 30 bob\nstats\nquit\n" | ./xdl mydb.xdl
```

---

## 3. Embedding XDL in Your Application

Add XDL as a static library or copy the source files directly into your project.

### Step 1: Include the umbrella header

```cpp
#include <xdl.h>   // or #include "xdl/db.h" for just the DB API
```

### Step 2: Open a database

```cpp
xdl::DB db("path/to/mydb.xdl");   // file created if absent
db.open();
```

You can customise compression and cache size:

```cpp
xdl::DB db(
    "mydb.xdl",
    xdl::CompressionType::LZ4,   // or CompressionType::None
    512                           // LRU cache size in pages
);
db.open();
```

### Step 3: Insert rows

```cpp
// Individual fields
db.insert(1, 25, "alice");

// Via Row struct
xdl::Row row(2, 30, "bob");
db.insert(row);
```

Inserting a duplicate primary key throws `xdl::DuplicateKeyError`.

### Step 4: Look up a row

```cpp
// Throws xdl::NotFoundError if absent
xdl::Row r = db.get(1);
std::cout << r.id << " " << r.name_str() << "\n";

// Non-throwing variant
xdl::Row out;
if (db.try_get(42, out)) {
    // found
}
```

### Step 5: Scan

```cpp
// Scan all rows
db.scan([](const xdl::Row& row) {
    std::cout << row.id << "\n";
});

// Filtered scan
xdl::ScanFilter f;
f.min_age     = 20;
f.max_age     = 30;
f.name_prefix = "al";

auto results = db.scan_all(f);   // returns std::vector<Row>
```

### Step 6: Close

```cpp
db.close();   // flushes all dirty pages first
```

The destructor also calls `close()` automatically, so RAII usage is safe.

### Step 7: Checkpoint (optional)

Call `checkpoint()` at any point to flush all in-memory dirty pages to disk without closing:

```cpp
db.checkpoint();
```

---

## 4. Compile-time integration example

```cmake
add_subdirectory(third_party/xdl)

target_link_libraries(your_app PRIVATE xdl_core)
target_include_directories(your_app PRIVATE third_party/xdl/include)
```

```cpp
// your_app/main.cpp
#include <xdl.h>

int main() {
    xdl::DB db("app.xdl");
    db.open();
    db.insert(1, 99, "hello_xdl");
    auto r = db.get(1);
    return 0;
}
```

---

## 5. Error Handling

All XDL errors inherit from `xdl::XDLError` (which itself inherits `std::runtime_error`).

```cpp
try {
    db.insert(1, 25, "alice");
    db.insert(1, 26, "alice2");  // duplicate!
} catch (const xdl::DuplicateKeyError& e) {
    std::cerr << "Duplicate: " << e.what() << "\n";
} catch (const xdl::NotFoundError& e) {
    std::cerr << "Missing:   " << e.what() << "\n";
} catch (const xdl::IOError& e) {
    std::cerr << "I/O:       " << e.what() << "\n";
} catch (const xdl::CorruptionError& e) {
    std::cerr << "Corrupt:   " << e.what() << "\n";
} catch (const xdl::XDLError& e) {
    std::cerr << "XDL error: " << e.what() << "\n";
}
```

---

## 6. Next Steps

- Read [`docs/architecture.md`](architecture.md) to understand the internal page/compression/cache pipeline.
- Read [`docs/api_reference.md`](api_reference.md) for the complete public API.
- Look at [`bench/bench.cpp`](../bench/bench.cpp) to understand how to measure your workload.