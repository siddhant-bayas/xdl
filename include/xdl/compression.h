#pragma once

#include "types.h"
#include <vector>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// CompressionEngine
//
// Stateless — all methods are static.  Strategy is chosen per call so future
// per-table or per-column compression settings are trivially supported.
// ─────────────────────────────────────────────────────────────────────────────

class CompressionEngine {
public:
    // Compress src into dst; returns actual compressed size.
    // Throws CompressionError on failure.
    static size_t compress(
        CompressionType    type,
        const char*        src,
        size_t             src_size,
        std::vector<char>& dst);

    // Decompress src into dst (dst is pre-sized to expected_size).
    // Throws CompressionError on failure.
    static void decompress(
        CompressionType    type,
        const char*        src,
        size_t             src_size,
        char*              dst,
        size_t             expected_size);

    // Maximum compressed size for a given input size (for buffer pre-allocation).
    static size_t max_compressed_size(CompressionType type, size_t src_size);

    // Human-readable name for logging / diagnostics
    static const char* name(CompressionType type);
};

} // namespace xdl