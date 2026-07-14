#include "xdl/db.h"
#include "xdl/wal.h"
#include "xdl/bptree.h"
#include "xdl/migrate.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------

static int tests_run  = 0;
static int tests_pass = 0;
static int tests_fail = 0;

#define ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ \
                  << "  " #cond "\n"; \
        ++tests_fail; return; \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ \
                  << "  " #a " == " #b "  (" << (a) << " vs " << (b) \
                  << ")\n"; \
        ++tests_fail; return; \
    } \
} while (0)

#define ASSERT_THROWS(expr, ExType) do { \
    bool caught = false; \
    try { expr; } catch (const ExType&) { caught = true; } \
    if (!caught) { \
        std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ \
                  << "  Expected " #ExType " from " #expr "\n"; \
        ++tests_fail; return; \
    } \
} while(0)

#define RUN_TEST(fn) do { \
    std::cout << "  " #fn "... "; \
    ++tests_run; \
    try { \
        fn(); \
        ++tests_pass; \
        std::cout << "OK\n"; \
    } catch (const std::exception& e) { \
        std::cout << "EXCEPTION: " << e.what() << "\n"; \
        ++tests_fail; \
    } \
} while(0)

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

static std::string test_db_path() {
    return (std::filesystem::temp_directory_path() / "xdl_test.xdl").string();
}

static void cleanup() {
    auto path = test_db_path();
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".wal");
}

static xdl::Schema default_schema() {
    return {
        {"age",  xdl::FieldType::UINT32},
        {"name", xdl::FieldType::STRING},
    };
}

static xdl::Row make_row(uint32_t id, uint32_t age,
                         const std::string& name) {
    return xdl::Row{id, {xdl::FieldValue{age}, xdl::FieldValue{name}}};
}

// ---------------------------------------------------------------------------
// Core tests
// ---------------------------------------------------------------------------

void test_open_close() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    ASSERT(db.is_open());
    db.close();
    ASSERT(!db.is_open());
    db.open();
    ASSERT(db.is_open());
    db.close();
}

void test_insert_and_get() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    db.insert(make_row(1, 30, "alice"));

    xdl::Row r = db.get(1);
    ASSERT_EQ(r.id, (uint32_t)1);
    ASSERT_EQ(r.get_uint32(db.schema(), "age"),  (uint32_t)30);
    ASSERT_EQ(r.get_string(db.schema(), "name"), std::string("alice"));
    db.close();
}

void test_duplicate_key() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    db.insert(make_row(42, 20, "bob"));
    ASSERT_THROWS(db.insert(make_row(42, 21, "charlie")),
                  xdl::DuplicateKeyError);
    db.close();
}

void test_not_found() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    ASSERT_THROWS(db.get(999), xdl::NotFoundError);
    xdl::Row out;
    ASSERT(!db.try_get(999, out));
    db.close();
}

void test_scan_all() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    for (uint32_t i = 1; i <= 10; ++i)
        db.insert(make_row(i, 20 + i, "user" + std::to_string(i)));
    auto rows = db.scan_all();
    ASSERT_EQ(rows.size(), (size_t)10);
    db.close();
}

void test_scan_filter_age() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    for (uint32_t i = 1; i <= 20; ++i)
        db.insert(make_row(i, i + 10, "u" + std::to_string(i)));
    xdl::ScanFilter f;
    f.min("age", uint32_t(20)).max("age", uint32_t(25));
    auto rows = db.scan_all(f);
    for (auto& r : rows) {
        uint32_t age = r.get_uint32(db.schema(), "age");
        ASSERT(age >= 20 && age <= 25);
    }
    db.close();
}

void test_scan_filter_name_prefix() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    db.insert(make_row(1, 25, "alice"));
    db.insert(make_row(2, 26, "alfred"));
    db.insert(make_row(3, 27, "bob"));
    xdl::ScanFilter f;
    f.starts("name", "al");
    auto rows = db.scan_all(f);
    ASSERT_EQ(rows.size(), (size_t)2);
    db.close();
}

void test_persistence() {
    cleanup();
    {
        xdl::DB db(test_db_path(), default_schema());
        db.open();
        db.insert(make_row(100, 40, "persistent_alice"));
        db.close();
    }
    {
        xdl::DB db(test_db_path(), default_schema());
        db.open();
        xdl::Row r = db.get(100);
        ASSERT_EQ(r.id, (uint32_t)100);
        ASSERT_EQ(r.get_uint32(db.schema(), "age"),  (uint32_t)40);
        ASSERT_EQ(r.get_string(db.schema(), "name"),
                  std::string("persistent_alice"));
        db.close();
    }
}

void test_many_rows() {
    cleanup();
    constexpr int N = 500;
    xdl::DB db(test_db_path(), default_schema(), xdl::CompressionType::LZ4, 16);
    db.open();
    for (int i = 0; i < N; ++i)
        db.insert(make_row(static_cast<uint32_t>(i),
                           static_cast<uint32_t>(i % 100),
                           "name" + std::to_string(i)));
    db.checkpoint();
    for (int i = 0; i < N; ++i) {
        xdl::Row r = db.get(static_cast<uint32_t>(i));
        ASSERT_EQ(r.id, (uint32_t)i);
    }
    auto all = db.scan_all();
    ASSERT_EQ(all.size(), (size_t)N);
    db.close();
}

void test_no_compression() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema(), xdl::CompressionType::None);
    db.open();
    db.insert(make_row(1, 22, "nocompress"));
    db.close();
    {
        xdl::DB db2(test_db_path(), default_schema(), xdl::CompressionType::None);
        db2.open();
        auto r = db2.get(1);
        ASSERT_EQ(r.get_string(db2.schema(), "name"),
                  std::string("nocompress"));
        db2.close();
    }
}

void test_stats() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    db.insert(make_row(1, 20, "x"));
    db.insert(make_row(2, 21, "y"));
    auto s = db.stats();
    ASSERT_EQ(s.row_count, (size_t)2);
    db.close();
}

void test_checkpoint_then_read() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    db.insert(make_row(77, 33, "checkpoint_test"));
    db.checkpoint();
    auto r = db.get(77);
    ASSERT_EQ(r.id, (uint32_t)77);
    db.close();
}

void test_long_string() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    std::string long_name(200, 'x');
    db.insert(make_row(1, 1, long_name));
    xdl::Row r = db.get(1);
    ASSERT_EQ(r.get_string(db.schema(), "name"), long_name);
    db.close();
}

void test_custom_schema_three_fields() {
    cleanup();
    xdl::Schema schema = {
        {"score",    xdl::FieldType::UINT32},
        {"username", xdl::FieldType::STRING},
        {"level",    xdl::FieldType::UINT32},
    };
    xdl::DB db(test_db_path(), schema);
    db.open();
    db.insert(xdl::Row{1, {xdl::FieldValue{uint32_t(9000)},
                           xdl::FieldValue{std::string("hero")},
                           xdl::FieldValue{uint32_t(42)}}});
    xdl::Row r = db.get(1);
    ASSERT_EQ(r.get_uint32(schema, "score"),    (uint32_t)9000);
    ASSERT_EQ(r.get_string(schema, "username"), std::string("hero"));
    ASSERT_EQ(r.get_uint32(schema, "level"),    (uint32_t)42);
    db.close();
}

void test_scan_filter_eq() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    db.insert(make_row(1, 10, "a"));
    db.insert(make_row(2, 20, "b"));
    db.insert(make_row(3, 10, "c"));
    xdl::ScanFilter f;
    f.eq("age", uint32_t(10));
    auto rows = db.scan_all(f);
    ASSERT_EQ(rows.size(), (size_t)2);
    db.close();
}

void test_wrong_field_count() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    xdl::Row bad(1, {xdl::FieldValue{uint32_t(5)}});
    ASSERT_THROWS(db.insert(bad), xdl::XDLError);
    db.close();
}

// ---------------------------------------------------------------------------
// WAL tests
// ---------------------------------------------------------------------------

void test_wal_basic() {
    auto wp = (std::filesystem::temp_directory_path() / "_test_wal.wal").string();
    std::filesystem::remove(wp);

    char d1[] = "row_data_1";
    xdl::WAL wal(wp);
    wal.open();
    wal.log_insert(1, 0, 0, d1, sizeof(d1));
    wal.log_commit();
    wal.log_checkpoint();
    wal.close();

    xdl::WAL wal2(wp);
    wal2.open();
    auto recs = wal2.replay();
    wal2.close();

    ASSERT_EQ(recs.size(), (size_t)3);
    ASSERT_EQ(recs[0].type, xdl::WAL::WALRecord::INSERT);
    ASSERT_EQ(recs[0].row_id, (uint32_t)1);
    ASSERT_EQ(recs[1].type, xdl::WAL::WALRecord::COMMIT);
    ASSERT_EQ(recs[2].type, xdl::WAL::WALRecord::CHECKPOINT);

    std::filesystem::remove(wp);
}

void test_wal_clear() {
    auto wp = (std::filesystem::temp_directory_path() / "_test_wal_clear.wal").string();
    std::filesystem::remove(wp);

    xdl::WAL wal(wp);
    wal.open();
    char d[] = "x";
    wal.log_insert(1, 0, 0, d, 1);
    ASSERT(wal.file_size() > 32);
    wal.clear();
    ASSERT_EQ(wal.file_size(), (uint32_t)32);
    ASSERT(wal.is_open());
    wal.close();

    std::filesystem::remove(wp);
}

void test_wal_corrupted_trailing() {
    auto wp = (std::filesystem::temp_directory_path() / "_test_wal_corrupt.wal").string();
    std::filesystem::remove(wp);

    {
        xdl::WAL wal(wp);
        wal.open();
        char d[] = "good";
        wal.log_insert(1, 0, 0, d, sizeof(d));
        wal.log_commit();
        wal.close();
    }

    {
#ifdef _WIN32
        int fd;
        _sopen_s(&fd, wp.c_str(), _O_RDWR, _SH_DENYNO, 0);
        struct _stat st;
        ::_fstat(fd, &st);
        if (st.st_size > 20) {
            ::_chsize(fd, st.st_size - 20);
        }
        ::_close(fd);
#else
        int fd = ::open(wp.c_str(), O_RDWR);
        struct stat st;
        ::fstat(fd, &st);
        if (st.st_size > 20) {
            int rc = ::ftruncate(fd, st.st_size - 20);
            (void)rc;
        }
        ::close(fd);
#endif
    }

    {
        xdl::WAL wal(wp);
        wal.open();
        auto recs = wal.replay();
        ASSERT(recs.size() >= 1);
        ASSERT_EQ(recs[0].type, xdl::WAL::WALRecord::INSERT);
        wal.close();
    }

    std::filesystem::remove(wp);
}

// ---------------------------------------------------------------------------
// B+ Tree tests
// ---------------------------------------------------------------------------

void test_bptree_basic() {
    xdl::BPTree tree;
    ASSERT(tree.empty());
    tree.insert(xdl::FieldValue{uint32_t(25)}, xdl::FieldType::UINT32, 1);
    tree.insert(xdl::FieldValue{uint32_t(10)}, xdl::FieldType::UINT32, 2);
    ASSERT_EQ(tree.size(), (size_t)2);
    auto* ids = tree.find(xdl::FieldValue{uint32_t(10)},
                          xdl::FieldType::UINT32);
    ASSERT(ids != nullptr);
    ASSERT_EQ((*ids)[0], (uint32_t)2);
    ASSERT(tree.find(xdl::FieldValue{uint32_t(99)},
                      xdl::FieldType::UINT32) == nullptr);
}

void test_bptree_range_scan() {
    xdl::BPTree tree;
    for (uint32_t i = 0; i < 100; ++i)
        tree.insert(xdl::FieldValue{i}, xdl::FieldType::UINT32, i * 10);
    int count = 0;
    tree.range_scan(
        std::optional<xdl::FieldValue>{uint32_t(20)}, xdl::FieldType::UINT32,
        true,
        std::optional<xdl::FieldValue>{uint32_t(30)}, xdl::FieldType::UINT32,
        true,
        [&](const std::vector<char>&, const std::vector<uint32_t>&) {
            ++count;
        });
    ASSERT_EQ(count, 11);
}

void test_bptree_erase() {
    xdl::BPTree tree;
    tree.insert(xdl::FieldValue{uint32_t(42)}, xdl::FieldType::UINT32, 100);
    tree.insert(xdl::FieldValue{uint32_t(42)}, xdl::FieldType::UINT32, 200);
    tree.erase(xdl::FieldValue{uint32_t(42)}, xdl::FieldType::UINT32, 100);
    ASSERT_EQ(tree.find(xdl::FieldValue{uint32_t(42)},
                        xdl::FieldType::UINT32)->size(), (size_t)1);
    tree.erase(xdl::FieldValue{uint32_t(42)}, xdl::FieldType::UINT32, 200);
    ASSERT(tree.find(xdl::FieldValue{uint32_t(42)},
                      xdl::FieldType::UINT32) == nullptr);
    ASSERT(tree.empty());
}

void test_bptree_string_keys() {
    xdl::BPTree tree;
    tree.insert(xdl::FieldValue{std::string("alice")}, xdl::FieldType::STRING,
                1);
    tree.insert(xdl::FieldValue{std::string("bob")}, xdl::FieldType::STRING,
                2);
    int count = 0;
    tree.range_scan(
        std::optional<xdl::FieldValue>{std::string("alice")},
        xdl::FieldType::STRING, true,
        std::optional<xdl::FieldValue>{std::string("charlie")},
        xdl::FieldType::STRING, true,
        [&](const std::vector<char>&, const std::vector<uint32_t>&) {
            ++count;
        });
    ASSERT_EQ(count, 2);
}

// ---------------------------------------------------------------------------
// Schema migration tests
// ---------------------------------------------------------------------------

void test_schema_serialisation() {
    xdl::Schema schema = {
        {"age",   xdl::FieldType::UINT32},
        {"name",  xdl::FieldType::STRING},
        {"score", xdl::FieldType::FLOAT32},
    };
    schema.ver.version = 3;
    auto data = xdl::serialise_schema(schema);
    ASSERT(!data.empty());
    xdl::Schema r = xdl::deserialise_schema(data.data(), data.size());
    ASSERT_EQ(r.ver.version, (uint32_t)3);
    ASSERT_EQ(r.fields.size(), (size_t)3);
}

void test_schema_migration_add_column() {
    xdl::Schema old_s = {
        {"age",  xdl::FieldType::UINT32},
        {"name", xdl::FieldType::STRING},
    };
    xdl::Schema new_s = {
        {"age",   xdl::FieldType::UINT32},
        {"name",  xdl::FieldType::STRING},
        {"score", xdl::FieldType::FLOAT32},
    };
    xdl::SchemaMigrate mig;
    mig.from_version = 1;
    mig.to_version   = 2;
    mig.ops.push_back({xdl::MigrateOp::ADD, "score", "",
                       xdl::FieldType::FLOAT32, xdl::FieldValue{0.0f}});
    xdl::Row old_r{1, {xdl::FieldValue{uint32_t(25)},
                       xdl::FieldValue{std::string("alice")}}};
    xdl::Row new_r = xdl::SchemaMigrator::apply_to_row(old_r, old_s, new_s,
                                                        mig);
    ASSERT_EQ(new_r.id, (uint32_t)1);
    ASSERT_EQ(new_r.get_float32(2), 0.0f);
}

// ---------------------------------------------------------------------------
// New field type tests
// ---------------------------------------------------------------------------

void test_float32_field() {
    cleanup();
    xdl::Schema schema = {
        {"score", xdl::FieldType::FLOAT32},
        {"name",  xdl::FieldType::STRING},
    };
    xdl::DB db(test_db_path(), schema);
    db.open();
    db.insert(xdl::Row{1, {xdl::FieldValue{3.14f},
                           xdl::FieldValue{std::string("pi")}}});
    db.close();
    db.open();
    auto r = db.get(1);
    ASSERT_EQ(r.get_float32(0), 3.14f);
    db.close();
}

void test_bool_field() {
    cleanup();
    xdl::Schema schema = {
        {"active", xdl::FieldType::BOOL},
        {"name",   xdl::FieldType::STRING},
    };
    xdl::DB db(test_db_path(), schema);
    db.open();
    db.insert(xdl::Row{1, {xdl::FieldValue{true},
                           xdl::FieldValue{std::string("alice")}}});
    xdl::Row r = db.get(1);
    ASSERT_EQ(r.get_bool(0), true);
    xdl::ScanFilter f;
    f.eq("active", true);
    auto rows = db.scan_all(f);
    ASSERT_EQ(rows.size(), (size_t)1);
    db.close();
}

void test_int64_field() {
    cleanup();
    xdl::Schema schema = {
        {"big_id", xdl::FieldType::INT64},
        {"name",   xdl::FieldType::STRING},
    };
    xdl::DB db(test_db_path(), schema);
    db.open();
    db.insert(xdl::Row{1, {xdl::FieldValue{int64_t(-999999999LL)},
                           xdl::FieldValue{std::string("neg")}}});
    db.insert(xdl::Row{2, {xdl::FieldValue{int64_t(0)},
                           xdl::FieldValue{std::string("zero")}}});
    db.close();
    db.open();
    auto r = db.get(1);
    ASSERT_EQ(r.get_int64(0), (int64_t(-999999999LL)));
    db.close();
}

// ---------------------------------------------------------------------------
// Secondary index tests
// ---------------------------------------------------------------------------

void test_secondary_index() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    for (uint32_t i = 0; i < 50; ++i)
        db.insert(make_row(i, 20 + (i % 30), "user" + std::to_string(i)));
    db.create_index("age");
    ASSERT(db.has_index("age"));
    auto rows = db.index_range_scan("age",
        std::optional<xdl::FieldValue>{uint32_t(25)}, true,
        std::optional<xdl::FieldValue>{uint32_t(30)}, true);
    for (auto& r : rows) {
        uint32_t age = r.get_uint32(db.schema(), "age");
        ASSERT(age >= 25 && age <= 30);
    }
    ASSERT(rows.size() > 0);
    db.close();
}

void test_secondary_index_after_insert() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    db.insert(make_row(1, 25, "alice"));
    db.create_index("age");
    db.insert(make_row(2, 30, "bob"));
    db.insert(make_row(3, 25, "charlie"));
    auto rows = db.index_range_scan("age",
        std::optional<xdl::FieldValue>{uint32_t(25)}, true,
        std::optional<xdl::FieldValue>{uint32_t(25)}, true);
    ASSERT_EQ(rows.size(), (size_t)2);
    db.close();
}

void test_drop_index() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    db.create_index("age");
    ASSERT(db.has_index("age"));
    db.drop_index("age");
    ASSERT(!db.has_index("age"));
    db.close();
}

// ---------------------------------------------------------------------------
// Concurrency test
// ---------------------------------------------------------------------------

void test_concurrent_reads() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    for (uint32_t i = 0; i < 100; ++i)
        db.insert(make_row(i, i % 50, "user" + std::to_string(i)));
    db.close();
    db.open();
    std::atomic<int> total{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            xdl::ScanFilter f;
            f.min("age", uint32_t(10)).max("age", uint32_t(20));
            auto rows = db.scan_all(f);
            total += static_cast<int>(rows.size());
        });
    }
    for (auto& th : threads) th.join();
    ASSERT(total > 0);
    db.close();
}

// ---------------------------------------------------------------------------
// WAL crash recovery test
// ---------------------------------------------------------------------------

void test_wal_crash_recovery() {
    cleanup();
    {
        xdl::DB db(test_db_path(), default_schema());
        db.open();
        for (uint32_t i = 0; i < 20; ++i)
            db.insert(make_row(i, 10 + i, "name" + std::to_string(i)));
        db.checkpoint();
        db.close();
    }
    {
        xdl::DB db(test_db_path(), default_schema());
        db.open();
        for (uint32_t i = 20; i < 30; ++i)
            db.insert(make_row(i, 10 + i, "name" + std::to_string(i)));
        db.checkpoint();
        db.close();
    }
    {
        xdl::DB db(test_db_path(), default_schema());
        db.open();
        xdl::Stats s = db.stats();
        ASSERT_EQ(s.row_count, (size_t)30);
        for (uint32_t i = 0; i < 30; ++i) {
            xdl::Row r = db.get(i);
            ASSERT_EQ(r.id, i);
        }
        db.close();
    }
}

// ===========================================================================
// EXPANDED TESTS — target 100+
// ===========================================================================

// ---------------------------------------------------------------------------
// DB core — more coverage
// ---------------------------------------------------------------------------

void test_empty_db_scan() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    auto rows = db.scan_all();
    ASSERT(rows.empty());
    db.close();
}

void test_try_get_found() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    db.insert(make_row(5, 25, "found_me"));
    xdl::Row r;
    ASSERT(db.try_get(5, r));
    ASSERT_EQ(r.id, (uint32_t)5);
    ASSERT_EQ(r.get_string(db.schema(), "name"), std::string("found_me"));
    db.close();
}

void test_open_close_reopen_insert() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open(); db.close();
    db.open();
    db.insert(make_row(1, 10, "after_reopen"));
    xdl::Row r = db.get(1);
    ASSERT_EQ(r.get_string(db.schema(), "name"), std::string("after_reopen"));
    db.close();
}

void test_scan_all_empty_db() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    auto rows = db.scan_all();
    ASSERT(rows.empty());
    ASSERT_EQ(rows.size(), (size_t)0);
    db.close();
}

void test_insert_retrieve_string_only() {
    cleanup();
    xdl::Schema s = {{"val", xdl::FieldType::STRING}};
    xdl::DB db(test_db_path(), s);
    db.open();
    db.insert(xdl::Row{1, {xdl::FieldValue{std::string("hello")}}});
    auto r = db.get(1);
    ASSERT_EQ(r.get_string(s, "val"), std::string("hello"));
    db.close();
}

void test_insert_retrieve_int64_only() {
    cleanup();
    xdl::Schema s = {{"val", xdl::FieldType::INT64}};
    xdl::DB db(test_db_path(), s);
    db.open();
    db.insert(xdl::Row{1, {xdl::FieldValue{int64_t(42)}}});
    auto r = db.get(1);
    ASSERT_EQ(r.get_int64(0), int64_t(42));
    db.close();
}

void test_insert_retrieve_float32_only() {
    cleanup();
    xdl::Schema s = {{"val", xdl::FieldType::FLOAT32}};
    xdl::DB db(test_db_path(), s);
    db.open();
    db.insert(xdl::Row{1, {xdl::FieldValue{1.5f}}});
    auto r = db.get(1);
    ASSERT(r.get_float32(0) > 1.4f && r.get_float32(0) < 1.6f);
    db.close();
}

void test_insert_retrieve_bool_only() {
    cleanup();
    xdl::Schema s = {{"val", xdl::FieldType::BOOL}};
    xdl::DB db(test_db_path(), s);
    db.open();
    db.insert(xdl::Row{1, {xdl::FieldValue{true}}});
    db.insert(xdl::Row{2, {xdl::FieldValue{false}}});
    ASSERT_EQ(db.get(1).get_bool(0), true);
    ASSERT_EQ(db.get(2).get_bool(0), false);
    db.close();
}

void test_insert_retrieve_mixed_all_types() {
    cleanup();
    xdl::Schema s = {
        {"u32",  xdl::FieldType::UINT32},
        {"str",  xdl::FieldType::STRING},
        {"f32",  xdl::FieldType::FLOAT32},
        {"b",    xdl::FieldType::BOOL},
        {"i64",  xdl::FieldType::INT64},
    };
    xdl::DB db(test_db_path(), s);
    db.open();
    db.insert(xdl::Row{1, {
        xdl::FieldValue{uint32_t(99)},
        xdl::FieldValue{std::string("mixed")},
        xdl::FieldValue{2.71f},
        xdl::FieldValue{true},
        xdl::FieldValue{int64_t(-123456789LL)}
    }});
    auto r = db.get(1);
    ASSERT_EQ(r.get_uint32(s, "u32"), (uint32_t)99);
    ASSERT_EQ(r.get_string(s, "str"), std::string("mixed"));
    ASSERT(r.get_float32(s, "f32") > 2.7f && r.get_float32(s, "f32") < 2.8f);
    ASSERT_EQ(r.get_bool(s, "b"), true);
    ASSERT_EQ(r.get_int64(s, "i64"), int64_t(-123456789LL));
    db.close();
}

void test_scan_filter_uint32_min_only() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    for (uint32_t i = 0; i < 10; ++i)
        db.insert(make_row(i, i * 10, "u" + std::to_string(i)));
    xdl::ScanFilter f;
    f.min("age", uint32_t(50));
    auto rows = db.scan_all(f);
    for (auto& r : rows)
        ASSERT(r.get_uint32(db.schema(), "age") >= 50);
    ASSERT(rows.size() > 0);
    db.close();
}

void test_scan_filter_uint32_max_only() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    for (uint32_t i = 0; i < 10; ++i)
        db.insert(make_row(i, i * 10, "u" + std::to_string(i)));
    xdl::ScanFilter f;
    f.max("age", uint32_t(30));
    auto rows = db.scan_all(f);
    for (auto& r : rows)
        ASSERT(r.get_uint32(db.schema(), "age") <= 30);
    ASSERT(rows.size() > 0);
    db.close();
}

void test_scan_filter_int64_eq() {
    cleanup();
    xdl::Schema s = {{"val", xdl::FieldType::INT64}};
    xdl::DB db(test_db_path(), s);
    db.open();
    db.insert(xdl::Row{1, {xdl::FieldValue{int64_t(100)}}});
    db.insert(xdl::Row{2, {xdl::FieldValue{int64_t(200)}}});
    db.insert(xdl::Row{3, {xdl::FieldValue{int64_t(100)}}});
    xdl::ScanFilter f;
    f.eq("val", int64_t(100));
    auto rows = db.scan_all(f);
    ASSERT_EQ(rows.size(), (size_t)2);
    db.close();
}

void test_scan_filter_int64_min_max() {
    cleanup();
    xdl::Schema s = {{"val", xdl::FieldType::INT64}};
    xdl::DB db(test_db_path(), s);
    db.open();
    for (int i = 0; i < 10; ++i)
        db.insert(xdl::Row{static_cast<uint32_t>(i), {xdl::FieldValue{int64_t(i * 100)}}});
    xdl::ScanFilter f;
    f.min("val", int64_t(300)).max("val", int64_t(700));
    auto rows = db.scan_all(f);
    for (auto& r : rows) {
        int64_t v = r.get_int64(s, "val");
        ASSERT(v >= 300 && v <= 700);
    }
    ASSERT(rows.size() > 0);
    db.close();
}

void test_scan_filter_float32_min_max() {
    cleanup();
    xdl::Schema s = {{"score", xdl::FieldType::FLOAT32}};
    xdl::DB db(test_db_path(), s);
    db.open();
    for (uint32_t i = 0; i < 10; ++i)
        db.insert(xdl::Row{i, {xdl::FieldValue{static_cast<float>(i) * 1.5f}}});
    xdl::ScanFilter f;
    f.min("score", 3.0f).max("score", 7.0f);
    auto rows = db.scan_all(f);
    for (auto& r : rows) {
        float v = r.get_float32(s, "score");
        ASSERT(v >= 3.0f && v <= 7.0f);
    }
    db.close();
}

void test_scan_filter_string_eq() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    db.insert(make_row(1, 10, "alice"));
    db.insert(make_row(2, 20, "alice"));
    db.insert(make_row(3, 30, "bob"));
    xdl::ScanFilter f;
    f.eq("name", std::string("alice"));
    auto rows = db.scan_all(f);
    ASSERT_EQ(rows.size(), (size_t)2);
    for (auto& r : rows)
        ASSERT_EQ(r.get_string(db.schema(), "name"), std::string("alice"));
    db.close();
}

void test_scan_filter_empty_result() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    for (uint32_t i = 0; i < 5; ++i)
        db.insert(make_row(i, i, "u" + std::to_string(i)));
    xdl::ScanFilter f;
    f.eq("age", uint32_t(9999));
    auto rows = db.scan_all(f);
    ASSERT(rows.empty());
    db.close();
}

void test_scan_filter_combined_two_fields() {
    cleanup();
    xdl::Schema s = {{"age", xdl::FieldType::UINT32}, {"score", xdl::FieldType::UINT32}};
    xdl::DB db(test_db_path(), s);
    db.open();
    for (uint32_t i = 0; i < 20; ++i)
        db.insert(xdl::Row{i, {xdl::FieldValue{i}, xdl::FieldValue{i * 10}}});
    xdl::ScanFilter f;
    f.min("age", uint32_t(5)).max("age", uint32_t(10))
     .min("score", uint32_t(50));
    auto rows = db.scan_all(f);
    for (auto& r : rows) {
        ASSERT(r.get_uint32(s, "age") >= 5 && r.get_uint32(s, "age") <= 10);
        ASSERT(r.get_uint32(s, "score") >= 50);
    }
    ASSERT(rows.size() > 0);
    db.close();
}

void test_checkpoint_multiple_times() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    for (int round = 0; round < 5; ++round) {
        for (uint32_t i = 0; i < 10; ++i)
            db.insert(make_row(round * 10 + i, i, "r" + std::to_string(round * 10 + i)));
        db.checkpoint();
    }
    auto s = db.stats();
    ASSERT_EQ(s.row_count, (size_t)50);
    db.close();
}

void test_persistence_after_checkpoint() {
    cleanup();
    {
        xdl::DB db(test_db_path(), default_schema());
        db.open();
        for (uint32_t i = 0; i < 50; ++i)
            db.insert(make_row(i, i, "persist_" + std::to_string(i)));
        db.checkpoint();
        db.close();
    }
    {
        xdl::DB db(test_db_path(), default_schema());
        db.open();
        auto s = db.stats();
        ASSERT_EQ(s.row_count, (size_t)50);
        auto rows = db.scan_all();
        ASSERT_EQ(rows.size(), (size_t)50);
        db.close();
    }
}

void test_many_rows_no_compression() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema(), xdl::CompressionType::None, 32);
    db.open();
    constexpr int N = 200;
    for (int i = 0; i < N; ++i)
        db.insert(make_row(static_cast<uint32_t>(i),
                           static_cast<uint32_t>(i % 50),
                           "nc_" + std::to_string(i)));
    auto s = db.stats();
    ASSERT_EQ(s.row_count, (size_t)N);
    for (int i = 0; i < N; ++i) {
        auto r = db.get(static_cast<uint32_t>(i));
        ASSERT_EQ(r.id, static_cast<uint32_t>(i));
    }
    db.close();
}

void test_empty_string_field() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    db.insert(make_row(1, 10, ""));
    auto r = db.get(1);
    ASSERT_EQ(r.get_string(db.schema(), "name"), std::string(""));
    db.close();
}

void test_very_long_string_field() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    std::string long_str(4000, 'Z');
    db.insert(make_row(1, 1, long_str));
    auto r = db.get(1);
    ASSERT_EQ(r.get_string(db.schema(), "name"), long_str);
    db.close();
}

void test_stats_after_many_inserts() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema(), xdl::CompressionType::LZ4, 16);
    db.open();
    constexpr int N = 300;
    for (int i = 0; i < N; ++i)
        db.insert(make_row(static_cast<uint32_t>(i), 0, "s" + std::to_string(i)));
    auto s = db.stats();
    ASSERT_EQ(s.row_count, (size_t)N);
    ASSERT(s.page_count > 0);
    ASSERT(s.file_size_bytes > 0);
    db.close();
}

// ---------------------------------------------------------------------------
// WAL — more coverage
// ---------------------------------------------------------------------------

void test_wal_multiple_inserts() {
    auto wp = (std::filesystem::temp_directory_path() / "_test_wal_multi.wal").string();
    std::filesystem::remove(wp);

    xdl::WAL wal(wp);
    wal.open();
    for (int i = 0; i < 10; ++i) {
        std::string d = "payload_" + std::to_string(i);
        wal.log_insert(static_cast<uint32_t>(i), 0, 0, d.data(),
                       static_cast<uint32_t>(d.size()));
    }
    wal.log_commit();
    wal.close();

    xdl::WAL wal2(wp);
    wal2.open();
    auto recs = wal2.replay();
    wal2.close();
    ASSERT_EQ(recs.size(), (size_t)11);
    int insert_count = 0;
    for (auto& r : recs)
        if (r.type == xdl::WAL::WALRecord::INSERT) ++insert_count;
    ASSERT_EQ(insert_count, 10);
    std::filesystem::remove(wp);
}

void test_wal_large_payload() {
    auto wp = (std::filesystem::temp_directory_path() / "_test_wal_large.wal").string();
    std::filesystem::remove(wp);

    xdl::WAL wal(wp);
    wal.open();
    std::string big(1024, 'X');
    wal.log_insert(1, 0, 0, big.data(), static_cast<uint32_t>(big.size()));
    wal.log_commit();
    wal.close();

    xdl::WAL wal2(wp);
    wal2.open();
    auto recs = wal2.replay();
    wal2.close();
    ASSERT_EQ(recs.size(), (size_t)2);
    ASSERT_EQ(recs[0].row_data.size(), big.size());
    std::filesystem::remove(wp);
}

void test_wal_empty_payload() {
    auto wp = (std::filesystem::temp_directory_path() / "_test_wal_empty.wal").string();
    std::filesystem::remove(wp);

    xdl::WAL wal(wp);
    wal.open();
    wal.log_insert(1, 0, 0, nullptr, 0);
    wal.log_commit();
    wal.close();

    xdl::WAL wal2(wp);
    wal2.open();
    auto recs = wal2.replay();
    wal2.close();
    ASSERT_EQ(recs.size(), (size_t)2);
    ASSERT_EQ(recs[0].row_data.size(), (size_t)0);
    std::filesystem::remove(wp);
}

void test_wal_replay_record_types() {
    auto wp = (std::filesystem::temp_directory_path() / "_test_wal_types.wal").string();
    std::filesystem::remove(wp);

    xdl::WAL wal(wp);
    wal.open();
    char d[] = "data";
    wal.log_insert(1, 0, 0, d, sizeof(d));
    wal.log_commit();
    wal.log_checkpoint();
    wal.close();

    xdl::WAL wal2(wp);
    wal2.open();
    auto recs = wal2.replay();
    wal2.close();
    ASSERT_EQ(recs[0].type, xdl::WAL::WALRecord::INSERT);
    ASSERT_EQ(recs[1].type, xdl::WAL::WALRecord::COMMIT);
    ASSERT_EQ(recs[2].type, xdl::WAL::WALRecord::CHECKPOINT);
    std::filesystem::remove(wp);
}

void test_wal_reopen_and_append() {
    auto wp = (std::filesystem::temp_directory_path() / "_test_wal_append.wal").string();
    std::filesystem::remove(wp);

    {
        xdl::WAL wal(wp);
        wal.open();
        char d[] = "first";
        wal.log_insert(1, 0, 0, d, sizeof(d));
        wal.log_commit();
        wal.close();
    }
    {
        xdl::WAL wal(wp);
        wal.open();
        char d[] = "second";
        wal.log_insert(2, 0, 0, d, sizeof(d));
        wal.log_commit();
        wal.close();
    }
    {
        xdl::WAL wal(wp);
        wal.open();
        auto recs = wal.replay();
        wal.close();
        ASSERT_EQ(recs.size(), (size_t)4);
        ASSERT_EQ(recs[0].row_id, (uint32_t)1);
        ASSERT_EQ(recs[2].row_id, (uint32_t)2);
    }
    std::filesystem::remove(wp);
}

void test_wal_header_integrity() {
    auto wp = (std::filesystem::temp_directory_path() / "_test_wal_hdr.wal").string();
    std::filesystem::remove(wp);

    xdl::WAL wal(wp);
    wal.open();
    wal.close();

    xdl::WAL wal2(wp);
    wal2.open();
    auto recs = wal2.replay();
    wal2.close();
    ASSERT(recs.empty());
    std::filesystem::remove(wp);
}

void test_wal_clear_and_reuse() {
    auto wp = (std::filesystem::temp_directory_path() / "_test_wal_reuse.wal").string();
    std::filesystem::remove(wp);

    xdl::WAL wal(wp);
    wal.open();
    char d[] = "x";
    wal.log_insert(1, 0, 0, d, 1);
    wal.log_commit();
    wal.clear();
    ASSERT(wal.is_open());
    char d2[] = "y";
    wal.log_insert(2, 0, 0, d2, 1);
    wal.log_commit();
    wal.close();

    xdl::WAL wal2(wp);
    wal2.open();
    auto recs = wal2.replay();
    wal2.close();
    ASSERT_EQ(recs.size(), (size_t)2);
    ASSERT_EQ(recs[0].row_id, (uint32_t)2);
    std::filesystem::remove(wp);
}

// ---------------------------------------------------------------------------
// BPTree — more coverage
// ---------------------------------------------------------------------------

void test_bptree_empty_find() {
    xdl::BPTree tree;
    ASSERT(tree.find(xdl::FieldValue{uint32_t(1)}, xdl::FieldType::UINT32) == nullptr);
}

void test_bptree_single_element() {
    xdl::BPTree tree;
    tree.insert(xdl::FieldValue{uint32_t(42)}, xdl::FieldType::UINT32, 1);
    auto* ids = tree.find(xdl::FieldValue{uint32_t(42)}, xdl::FieldType::UINT32);
    ASSERT(ids != nullptr);
    ASSERT_EQ(ids->size(), (size_t)1);
    ASSERT_EQ((*ids)[0], (uint32_t)1);
}

void test_bptree_multiple_duplicates() {
    xdl::BPTree tree;
    for (uint32_t i = 0; i < 10; ++i)
        tree.insert(xdl::FieldValue{uint32_t(100)}, xdl::FieldType::UINT32, i);
    auto* ids = tree.find(xdl::FieldValue{uint32_t(100)}, xdl::FieldType::UINT32);
    ASSERT(ids != nullptr);
    ASSERT_EQ(ids->size(), (size_t)10);
}

void test_bptree_large_dataset() {
    xdl::BPTree tree;
    for (uint32_t i = 0; i < 500; ++i)
        tree.insert(xdl::FieldValue{i}, xdl::FieldType::UINT32, i);
    ASSERT_EQ(tree.size(), (size_t)500);
    for (uint32_t i = 0; i < 500; ++i) {
        auto* ids = tree.find(xdl::FieldValue{i}, xdl::FieldType::UINT32);
        ASSERT(ids != nullptr);
        ASSERT_EQ((*ids)[0], i);
    }
}

void test_bptree_erase_nonexistent() {
    xdl::BPTree tree;
    tree.insert(xdl::FieldValue{uint32_t(1)}, xdl::FieldType::UINT32, 10);
    tree.erase(xdl::FieldValue{uint32_t(999)}, xdl::FieldType::UINT32, 10);
    auto* ids = tree.find(xdl::FieldValue{uint32_t(1)}, xdl::FieldType::UINT32);
    ASSERT(ids != nullptr);
    ASSERT_EQ(ids->size(), (size_t)1);
}

void test_bptree_range_scan_open_ended_hi() {
    xdl::BPTree tree;
    for (uint32_t i = 0; i < 50; ++i)
        tree.insert(xdl::FieldValue{i}, xdl::FieldType::UINT32, i);
    int count = 0;
    tree.range_scan(
        std::optional<xdl::FieldValue>{uint32_t(40)}, xdl::FieldType::UINT32, true,
        std::nullopt, xdl::FieldType::UINT32, false,
        [&](const std::vector<char>&, const std::vector<uint32_t>&) { ++count; });
    ASSERT_EQ(count, 10);
}

void test_bptree_range_scan_open_ended_lo() {
    xdl::BPTree tree;
    for (uint32_t i = 0; i < 50; ++i)
        tree.insert(xdl::FieldValue{i}, xdl::FieldType::UINT32, i);
    int count = 0;
    tree.range_scan(
        std::nullopt, xdl::FieldType::UINT32, false,
        std::optional<xdl::FieldValue>{uint32_t(5)}, xdl::FieldType::UINT32, true,
        [&](const std::vector<char>&, const std::vector<uint32_t>&) { ++count; });
    ASSERT_EQ(count, 6);
}

void test_bptree_range_scan_exclusive_bounds() {
    xdl::BPTree tree;
    for (uint32_t i = 0; i < 20; ++i)
        tree.insert(xdl::FieldValue{i}, xdl::FieldType::UINT32, i);
    int count = 0;
    tree.range_scan(
        std::optional<xdl::FieldValue>{uint32_t(5)}, xdl::FieldType::UINT32, false,
        std::optional<xdl::FieldValue>{uint32_t(10)}, xdl::FieldType::UINT32, false,
        [&](const std::vector<char>&, const std::vector<uint32_t>&) { ++count; });
    ASSERT_EQ(count, 4);
}

void test_bptree_string_keys_ordering() {
    xdl::BPTree tree;
    tree.insert(xdl::FieldValue{std::string("cherry")}, xdl::FieldType::STRING, 3);
    tree.insert(xdl::FieldValue{std::string("apple")},  xdl::FieldType::STRING, 1);
    tree.insert(xdl::FieldValue{std::string("banana")}, xdl::FieldType::STRING, 2);
    std::vector<uint32_t> ids;
    tree.for_each([&](const std::vector<char>&, const std::vector<uint32_t>& id_vec) {
        ids.insert(ids.end(), id_vec.begin(), id_vec.end());
    });
    ASSERT_EQ(ids.size(), (size_t)3);
    ASSERT_EQ(ids[0], (uint32_t)1);
    ASSERT_EQ(ids[1], (uint32_t)2);
    ASSERT_EQ(ids[2], (uint32_t)3);
}

void test_bptree_for_each() {
    xdl::BPTree tree;
    for (uint32_t i = 0; i < 20; ++i)
        tree.insert(xdl::FieldValue{i}, xdl::FieldType::UINT32, i);
    int count = 0;
    tree.for_each([&](const std::vector<char>&, const std::vector<uint32_t>& ids) {
        count += static_cast<int>(ids.size());
    });
    ASSERT_EQ(count, 20);
}

void test_bptree_range_scan_empty() {
    xdl::BPTree tree;
    for (uint32_t i = 0; i < 10; ++i)
        tree.insert(xdl::FieldValue{i}, xdl::FieldType::UINT32, i);
    int count = 0;
    tree.range_scan(
        std::optional<xdl::FieldValue>{uint32_t(50)}, xdl::FieldType::UINT32, true,
        std::optional<xdl::FieldValue>{uint32_t(60)}, xdl::FieldType::UINT32, true,
        [&](const std::vector<char>&, const std::vector<uint32_t>&) { ++count; });
    ASSERT_EQ(count, 0);
}

void test_bptree_erase_all_duplicates() {
    xdl::BPTree tree;
    for (uint32_t i = 0; i < 5; ++i)
        tree.insert(xdl::FieldValue{uint32_t(77)}, xdl::FieldType::UINT32, i);
    for (uint32_t i = 0; i < 5; ++i)
        tree.erase(xdl::FieldValue{uint32_t(77)}, xdl::FieldType::UINT32, i);
    ASSERT(tree.find(xdl::FieldValue{uint32_t(77)}, xdl::FieldType::UINT32) == nullptr);
    ASSERT(tree.empty());
}

// ---------------------------------------------------------------------------
// KeyBuffer operators
// ---------------------------------------------------------------------------

void test_key_buffer_equal() {
    auto a = xdl::bptree_key_encode(xdl::FieldValue{uint32_t(42)}, xdl::FieldType::UINT32);
    auto b = xdl::bptree_key_encode(xdl::FieldValue{uint32_t(42)}, xdl::FieldType::UINT32);
    ASSERT(a == b);
    ASSERT(!(a != b));
}

void test_key_buffer_less() {
    auto a = xdl::bptree_key_encode(xdl::FieldValue{uint32_t(10)}, xdl::FieldType::UINT32);
    auto b = xdl::bptree_key_encode(xdl::FieldValue{uint32_t(20)}, xdl::FieldType::UINT32);
    ASSERT(a < b);
    ASSERT(b > a);
    ASSERT(a <= b);
    ASSERT(b >= a);
}

// ---------------------------------------------------------------------------
// Serialisation helpers
// ---------------------------------------------------------------------------

void test_serialise_u32_roundtrip() {
    char buf[4];
    xdl::serialise::write_u32(buf, 0xDEADBEEF);
    ASSERT_EQ(xdl::serialise::read_u32(buf), (uint32_t)0xDEADBEEF);
}

void test_serialise_u64_roundtrip() {
    char buf[8];
    xdl::serialise::write_u64(buf, 0x123456789ABCDEF0ULL);
    ASSERT_EQ(xdl::serialise::read_u64(buf), 0x123456789ABCDEF0ULL);
}

void test_serialise_u8_roundtrip() {
    char buf[1];
    xdl::serialise::write_u8(buf, 0xAB);
    ASSERT_EQ(xdl::serialise::read_u8(buf), (uint8_t)0xAB);
}

void test_serialise_u16_roundtrip() {
    char buf[2];
    xdl::serialise::write_u16(buf, 0xBEEF);
    ASSERT_EQ(xdl::serialise::read_u16(buf), (uint16_t)0xBEEF);
}

void test_serialise_u32_zero() {
    char buf[4] = {};
    xdl::serialise::write_u32(buf, 0);
    ASSERT_EQ(xdl::serialise::read_u32(buf), (uint32_t)0);
}

void test_serialise_u32_max() {
    char buf[4];
    xdl::serialise::write_u32(buf, UINT32_MAX);
    ASSERT_EQ(xdl::serialise::read_u32(buf), UINT32_MAX);
}

// ---------------------------------------------------------------------------
// Compression
// ---------------------------------------------------------------------------

void test_lz4_compress_decompress() {
    std::string input(1000, 'A');
    std::vector<char> compressed;
    size_t cs = xdl::CompressionEngine::compress(
        xdl::CompressionType::LZ4, input.data(), input.size(), compressed);
    ASSERT(cs > 0);
    ASSERT(cs < input.size());
    std::vector<char> decompressed(input.size());
    xdl::CompressionEngine::decompress(
        xdl::CompressionType::LZ4, compressed.data(), cs,
        decompressed.data(), input.size());
    ASSERT(memcmp(decompressed.data(), input.data(), input.size()) == 0);
}

void test_compression_none_roundtrip() {
    std::string input(100, 'B');
    std::vector<char> compressed;
    size_t cs = xdl::CompressionEngine::compress(
        xdl::CompressionType::None, input.data(), input.size(), compressed);
    ASSERT_EQ(cs, input.size());
    std::vector<char> decompressed(input.size());
    xdl::CompressionEngine::decompress(
        xdl::CompressionType::None, compressed.data(), cs,
        decompressed.data(), input.size());
    ASSERT(memcmp(decompressed.data(), input.data(), input.size()) == 0);
}

void test_compression_max_size() {
    size_t ms = xdl::CompressionEngine::max_compressed_size(xdl::CompressionType::LZ4, 1000);
    ASSERT(ms >= 1000);
}

void test_compression_repetitive_data() {
    std::string input(5000, 'X');
    std::vector<char> compressed;
    size_t cs = xdl::CompressionEngine::compress(
        xdl::CompressionType::LZ4, input.data(), input.size(), compressed);
    ASSERT(cs < 100);
    std::vector<char> decompressed(input.size());
    xdl::CompressionEngine::decompress(
        xdl::CompressionType::LZ4, compressed.data(), cs,
        decompressed.data(), input.size());
    ASSERT(memcmp(decompressed.data(), input.data(), input.size()) == 0);
}

void test_compression_name() {
    ASSERT(std::string(xdl::CompressionEngine::name(xdl::CompressionType::LZ4)) == "lz4");
    ASSERT(std::string(xdl::CompressionEngine::name(xdl::CompressionType::None)) == "none");
}

// ---------------------------------------------------------------------------
// Schema & migration — more coverage
// ---------------------------------------------------------------------------

void test_schema_serialisation_roundtrip_all_types() {
    xdl::Schema s = {
        {"u32",  xdl::FieldType::UINT32},
        {"str",  xdl::FieldType::STRING},
        {"f32",  xdl::FieldType::FLOAT32},
        {"bl",   xdl::FieldType::BOOL},
        {"i64",  xdl::FieldType::INT64},
    };
    s.ver.version = 7;
    auto data = xdl::serialise_schema(s);
    auto r = xdl::deserialise_schema(data.data(), data.size());
    ASSERT_EQ(r.fields.size(), (size_t)5);
    ASSERT_EQ(r.ver.version, (uint32_t)7);
    ASSERT_EQ(static_cast<int>(r.fields[0].type), static_cast<int>(xdl::FieldType::UINT32));
    ASSERT_EQ(static_cast<int>(r.fields[1].type), static_cast<int>(xdl::FieldType::STRING));
    ASSERT_EQ(static_cast<int>(r.fields[2].type), static_cast<int>(xdl::FieldType::FLOAT32));
    ASSERT_EQ(static_cast<int>(r.fields[3].type), static_cast<int>(xdl::FieldType::BOOL));
    ASSERT_EQ(static_cast<int>(r.fields[4].type), static_cast<int>(xdl::FieldType::INT64));
}

void test_schema_validate_unique_names() {
    xdl::Schema s = {{"a", xdl::FieldType::UINT32}, {"a", xdl::FieldType::STRING}};
    ASSERT_THROWS(s.validate(), std::invalid_argument);
}

void test_schema_validate_empty() {
    xdl::Schema s;
    ASSERT_THROWS(s.validate(), std::invalid_argument);
}

void test_schema_field_index_found() {
    xdl::Schema s = {{"x", xdl::FieldType::UINT32}, {"y", xdl::FieldType::STRING}};
    ASSERT_EQ(s.field_index("x"), 0);
    ASSERT_EQ(s.field_index("y"), 1);
}

void test_schema_field_index_not_found() {
    xdl::Schema s = {{"x", xdl::FieldType::UINT32}};
    ASSERT_EQ(s.field_index("z"), -1);
}

void test_parse_field_type_all() {
    ASSERT(xdl::parse_field_type("uint32")  == xdl::FieldType::UINT32);
    ASSERT(xdl::parse_field_type("string")  == xdl::FieldType::STRING);
    ASSERT(xdl::parse_field_type("float32") == xdl::FieldType::FLOAT32);
    ASSERT(xdl::parse_field_type("bool")    == xdl::FieldType::BOOL);
    ASSERT(xdl::parse_field_type("int64")   == xdl::FieldType::INT64);
    ASSERT_THROWS(xdl::parse_field_type("unknown"), std::invalid_argument);
}

void test_field_type_width() {
    ASSERT_EQ(xdl::field_type_width(xdl::FieldType::UINT32),  (uint32_t)4);
    ASSERT_EQ(xdl::field_type_width(xdl::FieldType::FLOAT32), (uint32_t)4);
    ASSERT_EQ(xdl::field_type_width(xdl::FieldType::BOOL),    (uint32_t)1);
    ASSERT_EQ(xdl::field_type_width(xdl::FieldType::INT64),   (uint32_t)8);
    ASSERT_EQ(xdl::field_type_width(xdl::FieldType::STRING),  (uint32_t)0);
}

void test_schema_migration_drop_column() {
    xdl::Schema old_s = {{"age", xdl::FieldType::UINT32}, {"name", xdl::FieldType::STRING}};
    xdl::Schema new_s = {{"age", xdl::FieldType::UINT32}};
    xdl::SchemaMigrate mig;
    mig.from_version = 1;
    mig.to_version = 2;
    mig.ops.push_back({xdl::MigrateOp::DROP, "name", "", xdl::FieldType::STRING, {}});
    xdl::Row old_r{1, {xdl::FieldValue{uint32_t(25)}, xdl::FieldValue{std::string("alice")}}};
    xdl::Row new_r = xdl::SchemaMigrator::apply_to_row(old_r, old_s, new_s, mig);
    ASSERT_EQ(new_r.fields.size(), (size_t)1);
    ASSERT_EQ(new_r.get_uint32(0), (uint32_t)25);
}

void test_schema_migration_rename_column() {
    xdl::Schema old_s = {{"age", xdl::FieldType::UINT32}};
    xdl::Schema new_s = {{"years", xdl::FieldType::UINT32}};
    xdl::SchemaMigrate mig;
    mig.from_version = 1;
    mig.to_version = 2;
    mig.ops.push_back({xdl::MigrateOp::RENAME, "age", "years", xdl::FieldType::UINT32, {}});
    xdl::Row old_r{1, {xdl::FieldValue{uint32_t(30)}}};
    xdl::Row new_r = xdl::SchemaMigrator::apply_to_row(old_r, old_s, new_s, mig);
    ASSERT_EQ(new_r.get_uint32(new_s, "years"), (uint32_t)30);
}

void test_schema_migration_add_column_with_default() {
    xdl::Schema old_s = {{"id", xdl::FieldType::UINT32}};
    xdl::Schema new_s = {{"id", xdl::FieldType::UINT32}, {"extra", xdl::FieldType::INT64}};
    xdl::SchemaMigrate mig;
    mig.from_version = 1;
    mig.to_version = 2;
    mig.ops.push_back({xdl::MigrateOp::ADD, "extra", "", xdl::FieldType::INT64,
                       xdl::FieldValue{int64_t(-1)}});
    xdl::Row old_r{1, {xdl::FieldValue{uint32_t(10)}}};
    xdl::Row new_r = xdl::SchemaMigrator::apply_to_row(old_r, old_s, new_s, mig);
    ASSERT_EQ(new_r.fields.size(), (size_t)2);
    ASSERT_EQ(new_r.get_int64(1), int64_t(-1));
}

void test_schema_migration_compatibility() {
    xdl::Schema old_s = {{"a", xdl::FieldType::UINT32}};
    xdl::Schema new_s = {{"a", xdl::FieldType::UINT32}, {"b", xdl::FieldType::STRING}};
    xdl::SchemaMigrate mig;
    mig.from_version = 1;
    mig.to_version = 2;
    mig.ops.push_back({xdl::MigrateOp::ADD, "b", "", xdl::FieldType::STRING,
                       xdl::FieldValue{std::string("")}});
    ASSERT(xdl::SchemaMigrator::is_compatible(old_s, new_s, mig));
}

// ---------------------------------------------------------------------------
// IndexManager — unit tests
// ---------------------------------------------------------------------------

void test_index_insert_and_find() {
    xdl::IndexManager idx;
    idx.insert(1, {0, 100, 0});
    idx.insert(5, {1, 200, 3});
    auto* e = idx.find(5);
    ASSERT(e != nullptr);
    ASSERT_EQ(e->page_id, (uint32_t)1);
    ASSERT_EQ(e->page_offset, (uint64_t)200);
    ASSERT(idx.find(999) == nullptr);
}

void test_index_remove() {
    xdl::IndexManager idx;
    idx.insert(1, {0, 100, 0});
    ASSERT(idx.remove(1));
    ASSERT(idx.find(1) == nullptr);
    ASSERT(!idx.remove(1));
}

void test_index_update_offset() {
    xdl::IndexManager idx;
    idx.insert(1, {0, 100, 0});
    idx.update_offset(1, 999);
    auto* e = idx.find(1);
    ASSERT(e != nullptr);
    ASSERT_EQ(e->page_offset, (uint64_t)999);
}

void test_index_size() {
    xdl::IndexManager idx;
    ASSERT(idx.empty());
    idx.insert(1, {0, 0, 0});
    idx.insert(2, {0, 0, 0});
    ASSERT_EQ(idx.size(), (size_t)2);
}

void test_index_for_each() {
    xdl::IndexManager idx;
    for (uint32_t i = 0; i < 10; ++i)
        idx.insert(i, {0, 0, i});
    int count = 0;
    idx.for_each([&](uint32_t, const xdl::IndexEntry&) { ++count; });
    ASSERT_EQ(count, 10);
}

void test_index_prepare_and_find() {
    xdl::IndexManager idx;
    for (uint32_t i = 0; i < 20; ++i)
        idx.insert(i, {0, static_cast<uint64_t>(i * 100), 0});
    idx.prepare();
    for (uint32_t i = 0; i < 20; ++i) {
        auto* e = idx.find(i);
        ASSERT(e != nullptr);
        ASSERT_EQ(e->page_offset, static_cast<uint64_t>(i * 100));
    }
}

void test_index_page_to_ids_reverse_mapping() {
    xdl::IndexManager idx;
    idx.insert(1, {5, 0, 0});
    idx.insert(2, {5, 0, 1});
    idx.insert(3, {10, 0, 0});
    idx.update_offsets_for_page(5, 999);
    auto* e1 = idx.find(1);
    auto* e2 = idx.find(2);
    auto* e3 = idx.find(3);
    ASSERT(e1 != nullptr && e2 != nullptr && e3 != nullptr);
    ASSERT_EQ(e1->page_offset, (uint64_t)999);
    ASSERT_EQ(e2->page_offset, (uint64_t)999);
    ASSERT_EQ(e3->page_offset, (uint64_t)0);
}

// ---------------------------------------------------------------------------
// Cache — unit tests
// ---------------------------------------------------------------------------

void test_cache_put_get() {
    xdl::PageCache cache(10);
    xdl::Page p(1);
    p.row_count = 5;
    cache.put(std::move(p));
    auto got = cache.get(1);
    ASSERT(got.has_value());
    ASSERT_EQ(got->id, (uint32_t)1);
    ASSERT_EQ(got->row_count, (uint32_t)5);
}

void test_cache_miss() {
    xdl::PageCache cache(10);
    ASSERT(!cache.get(999).has_value());
}

void test_cache_eviction() {
    xdl::PageCache cache(2);
    cache.put(xdl::Page(1));
    cache.put(xdl::Page(2));
    cache.put(xdl::Page(3));
    ASSERT(!cache.get(1).has_value());
    ASSERT(cache.get(2).has_value());
    ASSERT(cache.get(3).has_value());
}

void test_cache_lru_order() {
    xdl::PageCache cache(2);
    cache.put(xdl::Page(1));
    cache.put(xdl::Page(2));
    cache.get(1);
    cache.put(xdl::Page(3));
    ASSERT(cache.get(1).has_value());
    ASSERT(!cache.get(2).has_value());
}

void test_cache_eviction_callback() {
    int evicted = 0;
    xdl::PageCache cache(2, [&](xdl::Page& p) { (void)p; ++evicted; });
    xdl::Page p1(1); p1.dirty = true;
    xdl::Page p2(2); p2.dirty = true;
    xdl::Page p3(3); p3.dirty = true;
    cache.put(std::move(p1));
    cache.put(std::move(p2));
    cache.put(std::move(p3));
    ASSERT_EQ(evicted, 1);
}

void test_cache_capacity() {
    xdl::PageCache cache(5);
    ASSERT_EQ(cache.capacity(), (size_t)5);
}

void test_cache_size_after_operations() {
    xdl::PageCache cache(10);
    ASSERT_EQ(cache.size(), (size_t)0);
    cache.put(xdl::Page(1));
    ASSERT_EQ(cache.size(), (size_t)1);
    cache.put(xdl::Page(2));
    ASSERT_EQ(cache.size(), (size_t)2);
    cache.evict(1);
    ASSERT_EQ(cache.size(), (size_t)1);
}

void test_cache_put_replaces() {
    xdl::PageCache cache(10);
    xdl::Page p1(1);
    p1.row_count = 1;
    cache.put(std::move(p1));
    xdl::Page p2(1);
    p2.row_count = 2;
    cache.put(std::move(p2));
    auto got = cache.get(1);
    ASSERT(got.has_value());
    ASSERT_EQ(got->row_count, (uint32_t)2);
}

void test_cache_flush_all() {
    int flushed = 0;
    xdl::PageCache cache(10, [&](xdl::Page& p) { (void)p; ++flushed; });
    xdl::Page p1(1);
    p1.dirty = true;
    cache.put(std::move(p1));
    xdl::Page p2(2);
    p2.dirty = false;
    cache.put(std::move(p2));
    cache.flush_all();
    ASSERT_EQ(flushed, 1);
}

// ---------------------------------------------------------------------------
// Row helpers
// ---------------------------------------------------------------------------

void test_row_get_by_position() {
    xdl::Row r(10, {xdl::FieldValue{uint32_t(42)},
                    xdl::FieldValue{std::string("hello")},
                    xdl::FieldValue{3.14f}});
    ASSERT_EQ(r.get_uint32(0), (uint32_t)42);
    ASSERT_EQ(r.get_string(1), std::string("hello"));
    ASSERT(r.get_float32(2) > 3.1f);
}

void test_row_set() {
    xdl::Row r(1, {xdl::FieldValue{uint32_t(10)}});
    r.set(0, xdl::FieldValue{uint32_t(99)});
    ASSERT_EQ(r.get_uint32(0), (uint32_t)99);
}

void test_row_named_accessors() {
    xdl::Schema s = {{"age", xdl::FieldType::UINT32}, {"name", xdl::FieldType::STRING}};
    xdl::Row r(1, {xdl::FieldValue{uint32_t(25)}, xdl::FieldValue{std::string("bob")}});
    ASSERT_EQ(r.get_uint32(s, "age"), (uint32_t)25);
    ASSERT_EQ(r.get_string(s, "name"), std::string("bob"));
    ASSERT_THROWS(r.get_uint32(s, "nope"), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// DB error handling
// ---------------------------------------------------------------------------

void test_scan_with_filter_no_results() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    for (uint32_t i = 0; i < 10; ++i)
        db.insert(make_row(i, i * 10, "u" + std::to_string(i)));
    xdl::ScanFilter f;
    f.min("age", uint32_t(999));
    auto rows = db.scan_all(f);
    ASSERT(rows.empty());
    db.close();
}

void test_insert_and_scan_filter_eq_string() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    db.insert(make_row(1, 20, "cat"));
    db.insert(make_row(2, 20, "dog"));
    db.insert(make_row(3, 20, "cat"));
    xdl::ScanFilter f;
    f.eq("name", std::string("cat"));
    auto rows = db.scan_all(f);
    ASSERT_EQ(rows.size(), (size_t)2);
    db.close();
}

void test_scan_starts_filter() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    db.insert(make_row(1, 10, "prefix_aaa"));
    db.insert(make_row(2, 20, "prefix_bbb"));
    db.insert(make_row(3, 30, "other"));
    xdl::ScanFilter f;
    f.starts("name", "prefix");
    auto rows = db.scan_all(f);
    ASSERT_EQ(rows.size(), (size_t)2);
    db.close();
}

void test_scan_int64_eq_filter() {
    cleanup();
    xdl::Schema s = {{"val", xdl::FieldType::INT64}};
    xdl::DB db(test_db_path(), s);
    db.open();
    db.insert(xdl::Row{1, {xdl::FieldValue{int64_t(100)}}});
    db.insert(xdl::Row{2, {xdl::FieldValue{int64_t(200)}}});
    xdl::ScanFilter f;
    f.eq("val", int64_t(200));
    auto rows = db.scan_all(f);
    ASSERT_EQ(rows.size(), (size_t)1);
    ASSERT_EQ(rows[0].get_int64(s, "val"), int64_t(200));
    db.close();
}

void test_scan_float32_eq_filter() {
    cleanup();
    xdl::Schema s = {{"val", xdl::FieldType::FLOAT32}};
    xdl::DB db(test_db_path(), s);
    db.open();
    db.insert(xdl::Row{1, {xdl::FieldValue{1.0f}}});
    db.insert(xdl::Row{2, {xdl::FieldValue{2.0f}}});
    db.insert(xdl::Row{3, {xdl::FieldValue{1.0f}}});
    xdl::ScanFilter f;
    f.eq("val", 1.0f);
    auto rows = db.scan_all(f);
    ASSERT_EQ(rows.size(), (size_t)2);
    db.close();
}

void test_scan_bool_eq_filter() {
    cleanup();
    xdl::Schema s = {{"flag", xdl::FieldType::BOOL}};
    xdl::DB db(test_db_path(), s);
    db.open();
    db.insert(xdl::Row{1, {xdl::FieldValue{true}}});
    db.insert(xdl::Row{2, {xdl::FieldValue{false}}});
    db.insert(xdl::Row{3, {xdl::FieldValue{true}}});
    xdl::ScanFilter f;
    f.eq("flag", false);
    auto rows = db.scan_all(f);
    ASSERT_EQ(rows.size(), (size_t)1);
    ASSERT_EQ(rows[0].get_bool(0), false);
    db.close();
}

// ---------------------------------------------------------------------------
// Concurrency — more coverage
// ---------------------------------------------------------------------------

void test_concurrent_gets() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    for (uint32_t i = 0; i < 100; ++i)
        db.insert(make_row(i, i, "u" + std::to_string(i)));
    db.close();
    db.open();
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&]() {
            for (uint32_t i = 0; i < 100; ++i) {
                auto r = db.get(i);
                if (r.id == i) ok++;
            }
        });
    }
    for (auto& th : threads) th.join();
    ASSERT_EQ(ok.load(), (int)800);
    db.close();
}

void test_concurrent_scans_different_filters() {
    cleanup();
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    for (uint32_t i = 0; i < 200; ++i)
        db.insert(make_row(i, i % 100, "u" + std::to_string(i)));
    db.close();
    db.open();
    std::atomic<int> total{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t]() {
            xdl::ScanFilter f;
            f.min("age", uint32_t(t * 25)).max("age", uint32_t(t * 25 + 24));
            auto rows = db.scan_all(f);
            total += static_cast<int>(rows.size());
        });
    }
    for (auto& th : threads) th.join();
    ASSERT(total > 0);
    db.close();
}

// ---------------------------------------------------------------------------
// WAL crash recovery — more coverage
// ---------------------------------------------------------------------------

void test_wal_crash_recovery_no_checkpoint() {
    cleanup();
    {
        xdl::DB db(test_db_path(), default_schema());
        db.open();
        for (uint32_t i = 0; i < 10; ++i)
            db.insert(make_row(i, i, "nc_" + std::to_string(i)));
        db.close();
    }
    {
        xdl::DB db(test_db_path(), default_schema());
        db.open();
        auto s = db.stats();
        ASSERT_EQ(s.row_count, (size_t)10);
        db.close();
    }
}

void test_wal_crash_recovery_empty_wal() {
    cleanup();
    {
        xdl::DB db(test_db_path(), default_schema());
        db.open();
        db.insert(make_row(1, 10, "only"));
        db.checkpoint();
        db.close();
    }
    {
        xdl::DB db(test_db_path(), default_schema());
        db.open();
        auto s = db.stats();
        ASSERT_EQ(s.row_count, (size_t)1);
        auto r = db.get(1);
        ASSERT_EQ(r.get_string(db.schema(), "name"), std::string("only"));
        db.close();
    }
}

void test_wal_crash_recovery_many_cycles() {
    cleanup();
    for (int cycle = 0; cycle < 10; ++cycle) {
        xdl::DB db(test_db_path(), default_schema());
        db.open();
        db.insert(make_row(static_cast<uint32_t>(cycle), cycle,
                           "cycle_" + std::to_string(cycle)));
        if (cycle % 2 == 0) db.checkpoint();
        db.close();
    }
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    auto s = db.stats();
    ASSERT_EQ(s.row_count, (size_t)10);
    for (int i = 0; i < 10; ++i) {
        auto r = db.get(static_cast<uint32_t>(i));
        ASSERT_EQ(r.id, static_cast<uint32_t>(i));
    }
    db.close();
}

void test_wal_crash_recovery_persistence() {
    cleanup();
    for (int phase = 0; phase < 5; ++phase) {
        xdl::DB db(test_db_path(), default_schema());
        db.open();
        for (uint32_t i = 0; i < 10; ++i) {
            uint32_t id = phase * 10 + i;
            db.insert(make_row(id, id, "p" + std::to_string(id)));
        }
        db.checkpoint();
        db.close();
    }
    xdl::DB db(test_db_path(), default_schema());
    db.open();
    auto s = db.stats();
    ASSERT_EQ(s.row_count, (size_t)50);
    db.close();
}

// ---------------------------------------------------------------------------
// Page unit tests
// ---------------------------------------------------------------------------

void test_page_append_and_get() {
    xdl::Schema s = {{"a", xdl::FieldType::UINT32}, {"b", xdl::FieldType::STRING}};
    xdl::Page p(1);
    xdl::Row r1(10, {xdl::FieldValue{uint32_t(42)}, xdl::FieldValue{std::string("hi")}});
    p.append_row(r1, s);
    ASSERT_EQ(p.row_count, (uint32_t)1);
    xdl::Row got = p.get_row(0, s);
    ASSERT_EQ(got.id, (uint32_t)10);
    ASSERT_EQ(got.get_uint32(s, "a"), (uint32_t)42);
    ASSERT_EQ(got.get_string(s, "b"), std::string("hi"));
}

void test_page_find_row() {
    xdl::Schema s = {{"v", xdl::FieldType::UINT32}};
    xdl::Page p(1);
    p.append_row(xdl::Row{10, {xdl::FieldValue{uint32_t(1)}}}, s);
    p.append_row(xdl::Row{20, {xdl::FieldValue{uint32_t(2)}}}, s);
    p.append_row(xdl::Row{30, {xdl::FieldValue{uint32_t(3)}}}, s);
    ASSERT_EQ(p.find_row(20, s), 1);
    ASSERT_EQ(p.find_row(10, s), 0);
    ASSERT_EQ(p.find_row(99, s), -1);
}

void test_page_multiple_rows() {
    xdl::Schema s = {{"v", xdl::FieldType::UINT32}};
    xdl::Page p(1);
    for (uint32_t i = 0; i < 10; ++i)
        p.append_row(xdl::Row{i * 10, {xdl::FieldValue{i}}}, s);
    ASSERT_EQ(p.row_count, (uint32_t)10);
    for (uint32_t i = 0; i < 10; ++i) {
        xdl::Row r = p.get_row(i, s);
        ASSERT_EQ(r.id, i * 10);
    }
}

void test_page_row_data_copy() {
    xdl::Schema s = {{"v", xdl::FieldType::UINT32}};
    xdl::Page p(1);
    p.append_row(xdl::Row{1, {xdl::FieldValue{uint32_t(42)}}}, s);
    auto copy = p.row_data_copy(0, s);
    ASSERT(!copy.empty());
}

void test_page_row_data_ptr_and_len() {
    xdl::Schema s = {{"v", xdl::FieldType::UINT32}};
    xdl::Page p(1);
    p.append_row(xdl::Row{1, {xdl::FieldValue{uint32_t(77)}}}, s);
    const char* ptr = p.row_data_ptr(0);
    uint32_t len = p.row_data_len(0);
    ASSERT(ptr != nullptr);
    ASSERT(len > 0);
}

// ---------------------------------------------------------------------------
// StorageEngine — move semantics
// ---------------------------------------------------------------------------

void test_storage_move_constructor() {
    cleanup();
    auto p = test_db_path();
    xdl::StorageEngine se(p, xdl::CompressionType::LZ4);
    se.open();
    xdl::StorageEngine se2(std::move(se));
    ASSERT(!se.is_open());
    ASSERT(se2.is_open());
    se2.close();
}

void test_storage_move_assignment() {
    cleanup();
    auto p = test_db_path();
    xdl::StorageEngine se(p, xdl::CompressionType::LZ4);
    se.open();
    xdl::StorageEngine se2("", xdl::CompressionType::None);
    se2 = std::move(se);
    ASSERT(!se.is_open());
    ASSERT(se2.is_open());
    se2.close();
}

// ---------------------------------------------------------------------------
// ScanFilter matches unit tests
// ---------------------------------------------------------------------------

void test_scan_filter_no_constraints_matches_all() {
    xdl::Schema s = {{"a", xdl::FieldType::UINT32}};
    xdl::ScanFilter f;
    xdl::Row r(1, {xdl::FieldValue{uint32_t(42)}});
    ASSERT(f.matches(r, s));
}

void test_scan_filter_matches_string_eq() {
    xdl::Schema s = {{"name", xdl::FieldType::STRING}};
    xdl::ScanFilter f;
    f.eq("name", std::string("alice"));
    xdl::Row match(1, {xdl::FieldValue{std::string("alice")}});
    xdl::Row nomatch(2, {xdl::FieldValue{std::string("bob")}});
    ASSERT(f.matches(match, s));
    ASSERT(!f.matches(nomatch, s));
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    std::cout << "=== XDL v1.0 Test Suite ===\n\n";

    // Core
    RUN_TEST(test_open_close);
    RUN_TEST(test_insert_and_get);
    RUN_TEST(test_duplicate_key);
    RUN_TEST(test_not_found);
    RUN_TEST(test_scan_all);
    RUN_TEST(test_scan_filter_age);
    RUN_TEST(test_scan_filter_name_prefix);
    RUN_TEST(test_persistence);
    RUN_TEST(test_many_rows);
    RUN_TEST(test_no_compression);
    RUN_TEST(test_stats);
    RUN_TEST(test_checkpoint_then_read);
    RUN_TEST(test_long_string);
    RUN_TEST(test_custom_schema_three_fields);
    RUN_TEST(test_scan_filter_eq);
    RUN_TEST(test_wrong_field_count);

    // Core — extended
    RUN_TEST(test_empty_db_scan);
    RUN_TEST(test_try_get_found);
    RUN_TEST(test_open_close_reopen_insert);
    RUN_TEST(test_scan_all_empty_db);
    RUN_TEST(test_insert_retrieve_string_only);
    RUN_TEST(test_insert_retrieve_int64_only);
    RUN_TEST(test_insert_retrieve_float32_only);
    RUN_TEST(test_insert_retrieve_bool_only);
    RUN_TEST(test_insert_retrieve_mixed_all_types);
    RUN_TEST(test_scan_filter_uint32_min_only);
    RUN_TEST(test_scan_filter_uint32_max_only);
    RUN_TEST(test_scan_filter_int64_eq);
    RUN_TEST(test_scan_filter_int64_min_max);
    RUN_TEST(test_scan_filter_float32_min_max);
    RUN_TEST(test_scan_filter_string_eq);
    RUN_TEST(test_scan_filter_empty_result);
    RUN_TEST(test_scan_filter_combined_two_fields);
    RUN_TEST(test_checkpoint_multiple_times);
    RUN_TEST(test_persistence_after_checkpoint);
    RUN_TEST(test_many_rows_no_compression);
    RUN_TEST(test_empty_string_field);
    RUN_TEST(test_very_long_string_field);
    RUN_TEST(test_stats_after_many_inserts);

    // WAL
    RUN_TEST(test_wal_basic);
    RUN_TEST(test_wal_clear);
    RUN_TEST(test_wal_corrupted_trailing);

    // WAL — extended
    RUN_TEST(test_wal_multiple_inserts);
    RUN_TEST(test_wal_large_payload);
    RUN_TEST(test_wal_empty_payload);
    RUN_TEST(test_wal_replay_record_types);
    RUN_TEST(test_wal_reopen_and_append);
    RUN_TEST(test_wal_header_integrity);
    RUN_TEST(test_wal_clear_and_reuse);

    // B+ Tree
    RUN_TEST(test_bptree_basic);
    RUN_TEST(test_bptree_range_scan);
    RUN_TEST(test_bptree_erase);
    RUN_TEST(test_bptree_string_keys);

    // B+ Tree — extended
    RUN_TEST(test_bptree_empty_find);
    RUN_TEST(test_bptree_single_element);
    RUN_TEST(test_bptree_multiple_duplicates);
    RUN_TEST(test_bptree_large_dataset);
    RUN_TEST(test_bptree_erase_nonexistent);
    RUN_TEST(test_bptree_range_scan_open_ended_hi);
    RUN_TEST(test_bptree_range_scan_open_ended_lo);
    RUN_TEST(test_bptree_range_scan_exclusive_bounds);
    RUN_TEST(test_bptree_string_keys_ordering);
    RUN_TEST(test_bptree_for_each);
    RUN_TEST(test_bptree_range_scan_empty);
    RUN_TEST(test_bptree_erase_all_duplicates);

    // KeyBuffer
    RUN_TEST(test_key_buffer_equal);
    RUN_TEST(test_key_buffer_less);

    // Serialisation
    RUN_TEST(test_serialise_u32_roundtrip);
    RUN_TEST(test_serialise_u64_roundtrip);
    RUN_TEST(test_serialise_u8_roundtrip);
    RUN_TEST(test_serialise_u16_roundtrip);
    RUN_TEST(test_serialise_u32_zero);
    RUN_TEST(test_serialise_u32_max);

    // Compression
    RUN_TEST(test_lz4_compress_decompress);
    RUN_TEST(test_compression_none_roundtrip);
    RUN_TEST(test_compression_max_size);
    RUN_TEST(test_compression_repetitive_data);
    RUN_TEST(test_compression_name);

    // Schema migration
    RUN_TEST(test_schema_serialisation);
    RUN_TEST(test_schema_migration_add_column);

    // Schema — extended
    RUN_TEST(test_schema_serialisation_roundtrip_all_types);
    RUN_TEST(test_schema_validate_unique_names);
    RUN_TEST(test_schema_validate_empty);
    RUN_TEST(test_schema_field_index_found);
    RUN_TEST(test_schema_field_index_not_found);
    RUN_TEST(test_parse_field_type_all);
    RUN_TEST(test_field_type_width);
    RUN_TEST(test_schema_migration_drop_column);
    RUN_TEST(test_schema_migration_rename_column);
    RUN_TEST(test_schema_migration_add_column_with_default);
    RUN_TEST(test_schema_migration_compatibility);

    // New field types
    RUN_TEST(test_float32_field);
    RUN_TEST(test_bool_field);
    RUN_TEST(test_int64_field);

    // Secondary indexes
    RUN_TEST(test_secondary_index);
    RUN_TEST(test_secondary_index_after_insert);
    RUN_TEST(test_drop_index);

    // IndexManager — unit
    RUN_TEST(test_index_insert_and_find);
    RUN_TEST(test_index_remove);
    RUN_TEST(test_index_update_offset);
    RUN_TEST(test_index_size);
    RUN_TEST(test_index_for_each);
    RUN_TEST(test_index_prepare_and_find);
    RUN_TEST(test_index_page_to_ids_reverse_mapping);

    // Cache — unit
    RUN_TEST(test_cache_put_get);
    RUN_TEST(test_cache_miss);
    RUN_TEST(test_cache_eviction);
    RUN_TEST(test_cache_lru_order);
    RUN_TEST(test_cache_eviction_callback);
    RUN_TEST(test_cache_capacity);
    RUN_TEST(test_cache_size_after_operations);
    RUN_TEST(test_cache_put_replaces);
    RUN_TEST(test_cache_flush_all);

    // Row helpers
    RUN_TEST(test_row_get_by_position);
    RUN_TEST(test_row_set);
    RUN_TEST(test_row_named_accessors);

    // DB error handling
    RUN_TEST(test_scan_with_filter_no_results);
    RUN_TEST(test_insert_and_scan_filter_eq_string);
    RUN_TEST(test_scan_starts_filter);
    RUN_TEST(test_scan_int64_eq_filter);
    RUN_TEST(test_scan_float32_eq_filter);
    RUN_TEST(test_scan_bool_eq_filter);

    // Concurrency
    RUN_TEST(test_concurrent_reads);
    RUN_TEST(test_concurrent_gets);
    RUN_TEST(test_concurrent_scans_different_filters);

    // Crash recovery
    RUN_TEST(test_wal_crash_recovery);
    RUN_TEST(test_wal_crash_recovery_no_checkpoint);
    RUN_TEST(test_wal_crash_recovery_empty_wal);
    RUN_TEST(test_wal_crash_recovery_many_cycles);
    RUN_TEST(test_wal_crash_recovery_persistence);

    // Page — unit
    RUN_TEST(test_page_append_and_get);
    RUN_TEST(test_page_find_row);
    RUN_TEST(test_page_multiple_rows);
    RUN_TEST(test_page_row_data_copy);
    RUN_TEST(test_page_row_data_ptr_and_len);

    // Storage — move semantics
    RUN_TEST(test_storage_move_constructor);
    RUN_TEST(test_storage_move_assignment);

    // ScanFilter
    RUN_TEST(test_scan_filter_no_constraints_matches_all);
    RUN_TEST(test_scan_filter_matches_string_eq);

    std::cout << "\n--- Results ---\n"
              << "Passed: " << tests_pass << "/" << tests_run << "\n";
    if (tests_fail > 0)
        std::cout << "FAILED: " << tests_fail << "\n";

    cleanup();
    return tests_fail > 0 ? 1 : 0;
}
