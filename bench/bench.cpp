#include "xdl/db.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <random>
#include <vector>
#include <string>
#include <filesystem>

using Clock = std::chrono::high_resolution_clock;
using Ms    = std::chrono::duration<double, std::milli>;

static double elapsed_ms(Clock::time_point start) {
    return Ms(Clock::now() - start).count();
}

static void bench(const std::string& label, std::function<void()> fn) {
    auto t0 = Clock::now();
    fn();
    double ms = elapsed_ms(t0);
    std::cout << std::setw(30) << std::left << label
              << std::setw(10) << std::right << std::fixed << std::setprecision(2) << ms << " ms\n";
}

int main(int argc, char* argv[]) {
    const std::string path = argc > 1 ? argv[1] : "/tmp/xdl_bench.xdl";
    const uint32_t N       = argc > 2 ? static_cast<uint32_t>(std::stoul(argv[2])) : 10'000;

    std::filesystem::remove(path);

    std::cout << "=== XDL Benchmark  (" << N << " rows) ===\n\n";

    // ── Sequential inserts ───────────────────────────────────────────────────
    {
        xdl::DB db(path, xdl::CompressionType::LZ4, 256);
        db.open();
        bench("Sequential insert " + std::to_string(N), [&]{
            for (uint32_t i = 0; i < N; ++i)
                db.insert(i, i % 100, "user_" + std::to_string(i));
        });
        bench("Checkpoint (flush all)", [&]{ db.checkpoint(); });

        // ── Random point lookups ─────────────────────────────────────────────
        std::mt19937 rng(42);
        std::uniform_int_distribution<uint32_t> dist(0, N - 1);
        std::vector<uint32_t> ids(N);
        std::generate(ids.begin(), ids.end(), [&]{ return dist(rng); });

        bench("Random get x" + std::to_string(N), [&]{
            for (uint32_t id : ids) { xdl::Row r; db.try_get(id, r); }
        });

        bench("Full scan", [&]{
            db.scan([](const xdl::Row&){});
        });

        auto s = db.stats();
        std::cout << "\n  rows       : " << s.row_count      << "\n"
                  << "  pages      : " << s.page_count      << "\n"
                  << "  file size  : " << s.file_size_bytes << " bytes  ("
                  << (s.file_size_bytes / 1024) << " KB)\n\n";
        db.close();
    }

    // ── Cold read (no cache) ─────────────────────────────────────────────────
    {
        xdl::DB db(path, xdl::CompressionType::LZ4, 1); // cache=1 => cold
        db.open();
        bench("Cold open + recover", [&]{});

        uint32_t mid = N / 2;
        bench("Cold get mid row", [&]{
            xdl::Row r; db.try_get(mid, r);
        });
        db.close();
    }

    std::filesystem::remove(path);
    return 0;
}