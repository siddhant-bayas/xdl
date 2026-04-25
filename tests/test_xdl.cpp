#include "xdl/db.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Minimal test harness
// ─────────────────────────────────────────────────────────────────────────────

static int tests_run  = 0;
static int tests_pass = 0;
static int tests_fail = 0;

#define ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond "\n"; \
        ++tests_fail; return; \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ \
                  << "  " #a " == " #b "  (" << (a) << " vs " << (b) << ")\n"; \
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
    std::cout << "  " #fn " ... "; \
    ++tests_run; \
    fn(); \
    if (tests_fail == 0) { ++tests_pass; std::cout << "OK\n"; } \
    else { std::cout << "(failed)\n"; } \
} while(0)

// ─────────────────────────────────────────────────────────────────────────────
// Fixtures
// ─────────────────────────────────────────────────────────────────────────────

static const std::string TEST_DB = "/tmp/xdl_test.xdl";

static void cleanup() {
    std::filesystem::remove(TEST_DB);
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

void test_open_close() {
    cleanup();
    xdl::DB db(TEST_DB);
    db.open();
    ASSERT(db.is_open());
    db.close();
    ASSERT(!db.is_open());
    // Reopen existing file
    db.open();
    ASSERT(db.is_open());
    db.close();
}

void test_insert_and_get() {
    cleanup();
    xdl::DB db(TEST_DB);
    db.open();

    db.insert(1, 30, "alice");
    xdl::Row r = db.get(1);
    ASSERT_EQ(r.id,  (uint32_t)1);
    ASSERT_EQ(r.age, (uint32_t)30);
    ASSERT_EQ(r.name_str(), "alice");

    db.close();
}

void test_duplicate_key() {
    cleanup();
    xdl::DB db(TEST_DB);
    db.open();
    db.insert(42, 20, "bob");
    ASSERT_THROWS(db.insert(42, 21, "charlie"), xdl::DuplicateKeyError);
    db.close();
}

void test_not_found() {
    cleanup();
    xdl::DB db(TEST_DB);
    db.open();
    ASSERT_THROWS(db.get(999), xdl::NotFoundError);
    xdl::Row out;
    ASSERT(!db.try_get(999, out));
    db.close();
}

void test_scan_all() {
    cleanup();
    xdl::DB db(TEST_DB);
    db.open();

    for (uint32_t i = 1; i <= 10; ++i)
        db.insert(i, 20 + i, "user" + std::to_string(i));

    auto rows = db.scan_all();
    ASSERT_EQ(rows.size(), (size_t)10);
    db.close();
}

void test_scan_filter_age() {
    cleanup();
    xdl::DB db(TEST_DB);
    db.open();

    for (uint32_t i = 1; i <= 20; ++i)
        db.insert(i, i + 10, "u" + std::to_string(i));

    xdl::ScanFilter f;
    f.min_age = 20;
    f.max_age = 25;

    auto rows = db.scan_all(f);
    for (auto& r : rows) {
        ASSERT(r.age >= 20 && r.age <= 25);
    }
    db.close();
}

void test_scan_filter_name_prefix() {
    cleanup();
    xdl::DB db(TEST_DB);
    db.open();
    db.insert(1, 25, "alice");
    db.insert(2, 26, "alfred");
    db.insert(3, 27, "bob");

    xdl::ScanFilter f;
    f.name_prefix = "al";
    auto rows = db.scan_all(f);
    ASSERT_EQ(rows.size(), (size_t)2);
    db.close();
}

void test_persistence() {
    cleanup();
    {
        xdl::DB db(TEST_DB);
        db.open();
        db.insert(100, 40, "persistent_alice");
        db.close();
    }
    // Reopen and verify data survived
    {
        xdl::DB db(TEST_DB);
        db.open();
        xdl::Row r = db.get(100);
        ASSERT_EQ(r.id,  (uint32_t)100);
        ASSERT_EQ(r.age, (uint32_t)40);
        ASSERT_EQ(r.name_str(), "persistent_alice");
        db.close();
    }
}

void test_many_rows() {
    cleanup();
    constexpr int N = 500;
    xdl::DB db(TEST_DB, xdl::CompressionType::LZ4, 16); // small cache to stress eviction
    db.open();

    for (int i = 0; i < N; ++i)
        db.insert(static_cast<uint32_t>(i), static_cast<uint32_t>(i % 100),
                  "name" + std::to_string(i));

    db.checkpoint();

    // Verify every row
    for (int i = 0; i < N; ++i) {
        xdl::Row r = db.get(static_cast<uint32_t>(i));
        ASSERT_EQ(r.id,  (uint32_t)i);
        ASSERT_EQ(r.age, (uint32_t)(i % 100));
        ASSERT_EQ(r.name_str(), "name" + std::to_string(i));
    }

    auto all = db.scan_all();
    ASSERT_EQ(all.size(), (size_t)N);
    db.close();
}

void test_no_compression() {
    cleanup();
    xdl::DB db(TEST_DB, xdl::CompressionType::None);
    db.open();
    db.insert(1, 22, "nocompress");
    db.close();
    {
        xdl::DB db2(TEST_DB, xdl::CompressionType::None);
        db2.open();
        auto r = db2.get(1);
        ASSERT_EQ(r.name_str(), "nocompress");
        db2.close();
    }
}

void test_stats() {
    cleanup();
    xdl::DB db(TEST_DB);
    db.open();
    db.insert(1, 20, "x");
    db.insert(2, 21, "y");
    auto s = db.stats();
    ASSERT_EQ(s.row_count, (size_t)2);
    db.close();
}

void test_row_serialized_size() {
    // Regression: serialised size must be stable
    ASSERT_EQ(xdl::Row::serialized_size(), (size_t)(4 + 4 + 2 + 64));
}

void test_checkpoint_then_read() {
    cleanup();
    xdl::DB db(TEST_DB);
    db.open();
    db.insert(77, 33, "checkpoint_test");
    db.checkpoint();
    auto r = db.get(77);
    ASSERT_EQ(r.id, (uint32_t)77);
    db.close();
}

void test_large_name_truncation() {
    cleanup();
    xdl::DB db(TEST_DB);
    db.open();
    // Name longer than NAME_MAX_LEN-1 should be silently truncated
    std::string long_name(200, 'x');
    db.insert(1, 1, long_name);
    xdl::Row r = db.get(1);
    ASSERT(r.name_len <= xdl::NAME_MAX_LEN - 1);
    db.close();
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== XDL Test Suite ===\n\n";

    RUN_TEST(test_row_serialized_size);
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
    RUN_TEST(test_large_name_truncation);

    std::cout << "\n--- Results ---\n"
              << "Passed: " << tests_pass << "/" << tests_run << "\n";
    if (tests_fail > 0)
        std::cout << "FAILED: " << tests_fail << "\n";

    cleanup();
    return tests_fail > 0 ? 1 : 0;
}