#include "xdl/db.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using Clock = std::chrono::high_resolution_clock;
using Ms    = std::chrono::duration<double, std::milli>;

static double to_ms(Clock::time_point t0) {
    return Ms(Clock::now() - t0).count();
}

static void bench(const std::string& label, std::function<void()> fn) {
    auto t0 = Clock::now();
    fn();
    double ms = to_ms(t0);
    std::cout << std::setw(30) << std::left << label
              << std::setw(10) << std::right << std::fixed
              << std::setprecision(2) << ms << " ms\n";
}

int main(int argc, char* argv[]) {
    const std::string path = argc > 1 ? argv[1]
        : (std::filesystem::temp_directory_path() / "xdl_bench.xdl").string();
    const uint32_t N = argc > 2 ? static_cast<uint32_t>(std::stoul(argv[2]))
                                : 10'000;
    const std::string comp_str = argc > 3 ? argv[3] : "lz4";
    const bool use_lz4 = comp_str == "lz4";
    const xdl::CompressionType comp =
        use_lz4 ? xdl::CompressionType::LZ4 : xdl::CompressionType::None;
    // Number of random lookups (defaults to N; fixed to isolate per-get cost).
    const uint32_t GETS = argc > 4 ? static_cast<uint32_t>(std::stoul(argv[4]))
                                   : N;

    std::filesystem::remove(path);
    std::filesystem::remove(path + ".wal");

    std::cout << "=== XDL Benchmark  (" << N << " rows, " << comp_str
              << ") ===\n\n";

    // -- Batch inserts (single WAL fsync per batch) ---------------------------
    {
        const std::string batch_path = path + ".batch";
        std::filesystem::remove(batch_path);
        std::filesystem::remove(batch_path + ".wal");

        xdl::Schema schema = {
            {"age",  xdl::FieldType::UINT32},
            {"name", xdl::FieldType::STRING},
        };
        xdl::DB db(batch_path, schema, comp, 256);
        db.open();

        std::vector<xdl::Row> rows;
        rows.reserve(N);
        for (uint32_t i = 0; i < N; ++i)
            rows.push_back(xdl::Row{i, {xdl::FieldValue{uint32_t(i % 100)},
                                        xdl::FieldValue{"user_" + std::to_string(i)}}});

        bench("Batch insert " + std::to_string(N), [&] { db.insert_batch(rows); });
        bench("Checkpoint (batch)", [&] { db.checkpoint(); });
        db.close();

        std::filesystem::remove(batch_path);
        std::filesystem::remove(batch_path + ".wal");
        std::cout << "\n";
    }

    // -- Sequential inserts ---------------------------------------------------
    {
        xdl::Schema schema = {
            {"age",  xdl::FieldType::UINT32},
            {"name", xdl::FieldType::STRING},
        };
        xdl::DB db(path, schema, comp, 256);
        db.open();
        bench("Sequential insert " + std::to_string(N), [&] {
            for (uint32_t i = 0; i < N; ++i)
                db.insert(xdl::Row{i, {xdl::FieldValue{uint32_t(i % 100)},
                                      xdl::FieldValue{"user_" + std::to_string(i)}}});
        });
        bench("Checkpoint (flush all)", [&] { db.checkpoint(); });

        // -- Random point lookups ---------------------------------------------
        std::mt19937 rng(42);
        std::uniform_int_distribution<uint32_t> dist(0, N - 1);
        std::vector<uint32_t> ids(GETS);
        std::generate(ids.begin(), ids.end(),
                      [&] { return dist(rng); });

        bench("Random get x" + std::to_string(GETS), [&] {
            for (uint32_t id : ids) {
                xdl::Row r;
                db.try_get(id, r);
            }
        });
        {
            auto t0 = Clock::now();
            for (uint32_t id : ids) {
                xdl::Row r;
                db.try_get(id, r);
            }
            double us = (Ms(Clock::now() - t0).count() * 1000.0) / GETS;
            std::cout << "  per-get    : " << std::fixed << std::setprecision(2)
                      << us << " us\n";
        }

        bench("Full scan", [&] {
            db.scan([](const xdl::Row&) {});
        });

        auto s = db.stats();
        std::cout << "\n  rows       : " << s.row_count << "\n"
                  << "  pages      : " << s.page_count << "\n"
                  << "  file size  : " << s.file_size_bytes << " bytes  ("
                  << (s.file_size_bytes / 1024) << " KB)\n\n";
        db.close();
    }

    // -- Verify after reopen: every row must read back intact ----------------
    {
        xdl::Schema schema = {
            {"age",  xdl::FieldType::UINT32},
            {"name", xdl::FieldType::STRING},
        };
        xdl::DB db(path, schema, comp, 256);
        db.open();
        std::atomic<uint64_t> verified{0};
        bench("Verify all rows after reopen", [&] {
            for (uint32_t i = 0; i < N; ++i) {
                xdl::Row r;
                if (!db.try_get(i, r)) continue;
                if (r.get_uint32(schema, "age") == i % 100 &&
                    r.get_string(schema, "name") == "user_" + std::to_string(i))
                    verified.fetch_add(1, std::memory_order_relaxed);
            }
        });
        if (verified.load() != N) {
            std::cerr << "VERIFY FAILED: " << verified.load() << "/" << N
                      << " rows intact\n";
            return 1;
        }
        std::cout << "  verified   : " << verified.load() << "/" << N
                  << " rows intact\n\n";
        db.close();
    }

    // -- Cold read (cache=1 forces disk on every access) ----------------------
    {
        xdl::Schema schema = {
            {"age",  xdl::FieldType::UINT32},
            {"name", xdl::FieldType::STRING},
        };
        xdl::DB db(path, schema, xdl::CompressionType::LZ4, 1);
        db.open();
        bench("Cold open + recover", [&] {});

        uint32_t mid = N / 2;
        bench("Cold get mid row", [&] {
            xdl::Row r;
            db.try_get(mid, r);
        });
        db.close();
    }

    // -- Index range scan, erase, update --------------------------------------
    {
        xdl::Schema schema = {
            {"age",  xdl::FieldType::UINT32},
            {"name", xdl::FieldType::STRING},
        };
        xdl::DB db(path, schema, comp, 256);
        db.open();
        db.create_index("age");

        bench("Index range scan age 40..49", [&] {
            auto rows = db.index_range_scan("age",
                std::optional<xdl::FieldValue>{uint32_t(40)}, true,
                std::optional<xdl::FieldValue>{uint32_t(49)}, true);
            (void)rows;
        });

        std::vector<uint32_t> to_erase;
        for (uint32_t i = 0; i < N; i += 10) to_erase.push_back(i);

        bench("Erase 1/10th of rows (batch)", [&] {
            db.erase_batch(to_erase);
        });

        std::vector<xdl::Row> to_update;
        for (uint32_t i = 1; i < N; i += 10)
            to_update.push_back(xdl::Row{i, {xdl::FieldValue{uint32_t(i % 100)},
                                             xdl::FieldValue{"updated_" + std::to_string(i)}}});

        bench("Update 1/10th of rows (batch)", [&] {
            db.update_batch(to_update);
        });

        auto s = db.stats();
        std::cout << "\n  rows after delete/update : " << s.row_count << "\n\n";
        db.close();
    }

    std::filesystem::remove(path);
    std::filesystem::remove(path + ".wal");
    return 0;
}
