# Getting Started with XDL

This guide walks you from zero to a working embedded database in under five minutes.

---

## Prerequisites

| Tool | Minimum version |
|------|----------------|
| C++ compiler | GCC 9+ / Clang 10+ / MSVC 2019 (C++17) |
| CMake (optional) | 3.16 |

No other dependencies. XDL bundles a compact LZ4 implementation and will automatically use the system `liblz4` if CMake detects it.

---

## 1. Build

### Linux / macOS

```bash
git clone https://github.com/siddhant-bayas/xdl.git
cd xdl
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

This produces:

- `build/xdl` — interactive CLI
- `build/xdl_tests` — test suite (139 tests)
- `build/libxdl_core.a` — static library to link against

### Windows (MSVC)

Open a **Developer Command Prompt** or use `vcvarsall.bat`:

```cmd
git clone https://github.com/siddhant-bayas/xdl.git
cd xdl
mkdir build_win && cd build_win
cmake .. -G Ninja
cmake --build .
```

Or with Visual Studio solution:

```cmd
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build build_win --config Release
```

Produces `xdl.exe`, `xdl_tests.exe`, `xdl_core.lib`.

### Run the tests

```bash
./xdl_tests
# Expected: Passed: 139/139
```

### Without CMake

```bash
g++ -std=c++17 -O2 -Iinclude -lpthread \
    src/page.cpp src/compression.cpp src/storage.cpp \
    src/cache.cpp src/index.cpp src/pager.cpp src/db.cpp \
    src/wal.cpp src/bptree.cpp src/mmap.cpp src/migrate.cpp \
    src/main.cpp -o xdl
```

> **Note:** On Windows without CMake, use the MSVC `cl.exe` compiler instead, or build inside the MSYS2 shell.

---

## 2. Using the CLI

The `xdl` binary opens (or creates) a database file and accepts commands interactively or from stdin.

```bash
./xdl mydata.xdl --schema "age:uint32,name:string,score:float32"
```

### Available commands

| Command | Description |
|---------|-------------|
| `insert <id> <v1> [<v2> ...]` | Insert a row (values in schema order) |
| `get <id>` | Retrieve a row by primary key |
| `scan [--<field> <val>] [--min-<field> N] [--max-<field> N] [--prefix-<field> STR]` | Scan with optional filters |
| `index <field_name>` | Create a B+ tree secondary index on a field |
| `schema` | Display current schema |
| `stats` | Show row count, page count, WAL status, indexes |
| `checkpoint` | Flush all data to disk, clear WAL |
| `help` | Show command reference |
| `quit` / `exit` | Save and exit |

### Example session

```
$ ./xdl mydata.xdl --schema "age:uint32,name:string,score:float32"
XDL v2  — mydata.xdl
Schema v1 (3 field(s)):
  age   : uint32
  name  : string
  score : float32
Type 'help' for commands.

xdl> insert 1 25 alice 95.5
Inserted id=1

xdl> insert 2 30 bob 87.2
Inserted id=2

xdl> insert 3 22 carol 91.0
Inserted id=3

xdl> scan
id=     1  age=    25  name=alice       score=   95.50
id=     2  age=    30  name=bob         score=   87.20
id=     3  age=    22  name=carol       score=   91.00
3 row(s)

xdl> scan --min-age 25 --max-score 90.0
id=     2  age=    30  name=bob         score=   87.20
1 row(s)

xdl> index age
Index created on 'age'

xdl> stats
rows          : 3
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

### Batch / scripted usage

```bash
printf "insert 1 25 alice 95.5\ninsert 2 30 bob 87.2\nstats\nquit\n" | ./xdl mydb.xdl
```

---

## 3. Embedding XDL in Your Application

### Step 1: Include the umbrella header

```cpp
#include <xdl.h>
```

### Step 2: Define a schema and open a database

```cpp
xdl::Schema schema = {
    {"age",    xdl::FieldType::UINT32},
    {"name",   xdl::FieldType::STRING},
    {"score",  xdl::FieldType::FLOAT32},
    {"active", xdl::FieldType::BOOL},
};

xdl::DB db("path/to/mydb.xdl", schema);
db.open();
```

### Step 3: Insert rows

```cpp
// Using a Row struct
db.insert(xdl::Row{1, {
    xdl::FieldValue{uint32_t(25)},
    xdl::FieldValue{std::string("alice")},
    xdl::FieldValue{95.5f},
    xdl::FieldValue{true},
}});

// Convenience overload
db.insert(2, {xdl::FieldValue{uint32_t(30)},
              xdl::FieldValue{std::string("bob")},
              xdl::FieldValue{87.2f},
              xdl::FieldValue{false}});
```

### Step 4: Look up a row

```cpp
// Throws xdl::NotFoundError if absent
xdl::Row r = db.get(1);
std::cout << r.get_string(schema, "name") << "\n";  // "alice"

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
f.min("age", uint32_t(20)).max("age", uint32_t(30)).starts("name", "a");
auto results = db.scan_all(f);
```

### Step 6: Create a secondary index

```cpp
db.create_index("age");

// Range scan using the index
auto rows = db.index_range_scan("age",
    std::optional<xdl::FieldValue>{uint32_t(20)}, true,
    std::optional<xdl::FieldValue>{uint32_t(30)}, true);
```

### Step 7: Checkpoint and close

```cpp
db.checkpoint();   // flush to disk, clear WAL
db.close();        // also called automatically by destructor
```

---

## 4. Compile-time integration example

### Using CMake `find_package`

```cmake
# Install XDL first: cmake --install build
find_package(xdl REQUIRED)
target_link_libraries(your_app PRIVATE xdl::xdl_core)
```

### Using `add_subdirectory`

```cmake
add_subdirectory(third_party/xdl)
target_link_libraries(your_app PRIVATE xdl_core)
target_include_directories(your_app PRIVATE third_party/xdl/include)
```

```cpp
// your_app/main.cpp
#include <xdl.h>

int main() {
    xdl::Schema schema = {
        {"name", xdl::FieldType::STRING},
        {"age",  xdl::FieldType::UINT32},
    };
    xdl::DB db("app.xdl", schema);
    db.open();
    db.insert(xdl::Row{1, {xdl::FieldValue{std::string("hello")},
                           xdl::FieldValue{uint32_t(99)}}});
    auto r = db.get(1);
    return 0;
}
```

---

## 5. Error Handling

```cpp
try {
    db.insert(xdl::Row{1, {xdl::FieldValue{uint32_t(25)},
                           xdl::FieldValue{std::string("alice")}}});
    db.insert(xdl::Row{1, {xdl::FieldValue{uint32_t(26)},
                           xdl::FieldValue{std::string("alice2")}}});  // duplicate!
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

## 6. Packaging

XDL ships with CPack support for building system packages:

```bash
cmake --build build
cd build
cpack -G DEB     # produces xdl_1.0.0_amd64.deb
cpack -G RPM     # produces xdl-1.0.0-1.x86_64.rpm
cpack -G TGZ     # produces xdl-1.0.0-Linux.tar.gz
```

---

## 7. Next Steps

- Read [`docs/architecture.md`](architecture.md) to understand the internal page/compression/cache/WAL pipeline.
- Read [`docs/api_reference.md`](api_reference.md) for the complete public API.
- Look at [`bench/bench.cpp`](../bench/bench.cpp) to benchmark your workload.
- See `.github/workflows/` for CI/CD pipeline details.
