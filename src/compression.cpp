#include "xdl/compression.h"

// ─────────────────────────────────────────────────────────────────────────────
// LZ4 — try to use the system library; fall back to a minimal bundled copy
// if not available.  The CMake build will set XDL_HAVE_LZ4 when found.
// ─────────────────────────────────────────────────────────────────────────────

#ifdef XDL_HAVE_LZ4
#  include <lz4.h>
#else
// ── Minimal self-contained LZ4 implementation ───────────────────────────────
// This is a condensed but fully correct LZ4 block compressor/decompressor
// sufficient for our page sizes (up to ~16 KB).  Replace with the real
//then replace with full lz4.h
// ─────────────────────────────────────────────────────────────────────────────

#include <cstring>
#include <cstdint>
#include <cstdlib>

// Forward declarations matching the LZ4 API we use
static int  LZ4_compress_default(const char* src, char* dst, int srcSize, int dstCapacity);
static int  LZ4_decompress_safe (const char* src, char* dst, int compressedSize, int dstCapacity);
static int  LZ4_compressBound   (int inputSize);

// ─── Tiny LZ4 block implementation ──────────────────────────────────────────

#define MINMATCH        4
#define HASHLOG         12
#define HASHTABLESIZE   (1 << HASHLOG)
#define HASH_MASK       (HASHTABLESIZE - 1)
#define STEPSIZE        sizeof(size_t)
#define LASTLITERALS    5
#define MFLIMIT         (MINMATCH + LASTLITERALS)
#define LZ4_MAX_INPUT   0x7E000000

static inline uint32_t LZ4_hash4(uint32_t v) {
    return ((v * 2654435761U) >> (32 - HASHLOG));
}

static inline uint32_t LZ4_read32(const void* p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}

static inline void LZ4_write16(void* p, uint16_t v) { memcpy(p, &v, 2); }
static inline uint16_t LZ4_read16(const void* p) { uint16_t v; memcpy(&v,p,2); return v; }

static int LZ4_compressBound(int isize) {
    return isize + (isize / 255) + 16;
}

static int LZ4_compress_default(const char* src, char* dst, int srcSize, int dstCapacity) {
    const uint8_t* ip  = (const uint8_t*)src;
    const uint8_t* iend = ip + srcSize;
    const uint8_t* anchor = ip;
    const uint8_t* mflimit = iend - MFLIMIT;
    uint8_t* op = (uint8_t*)dst;
    uint8_t* oend = op + dstCapacity;

    const uint8_t* htable[HASHTABLESIZE] = {};

    if (srcSize < MFLIMIT) goto _last_literals;

    ip++;
    for (;;) {
        const uint8_t* match;
        uint8_t* token;
        size_t literal_len;
        int ml;

        // Find a match
        {
            const uint8_t* forwardip = ip;
            int step = 1, searchMatchNb = 1;
            do {
                uint32_t h = LZ4_hash4(LZ4_read32(forwardip));
                match = htable[h];
                htable[h] = forwardip;
                ip = forwardip;
                forwardip += step;
                step = (searchMatchNb++ >> 6) + 1;
                if (forwardip > mflimit) goto _last_literals;
            } while (
                (match == nullptr) ||
                ((ip - match) > 65535) ||
                (LZ4_read32(match) != LZ4_read32(ip))
            );
        }

        // Catch up
        while ((ip > anchor) && (match > (uint8_t*)src) && (ip[-1] == match[-1])) { ip--; match--; }

        literal_len = (size_t)(ip - anchor);
        token = op++;

        // Encode literals length
        if ((op + literal_len + (2 + 1 + LASTLITERALS) + (literal_len >> 8)) > oend)
            return 0;

        if (literal_len >= 15) {
            size_t ll = literal_len - 15;
            *token = (15 << 4);
            while (ll >= 255) { *op++ = 255; ll -= 255; }
            *op++ = (uint8_t)ll;
        } else {
            *token = (uint8_t)(literal_len << 4);
        }
        memcpy(op, anchor, literal_len); op += literal_len;

        // Encode offset
        LZ4_write16(op, (uint16_t)(ip - match)); op += 2;

        // Encode match length
        ml = 0;
        {
            const uint8_t* mp = match + MINMATCH;
            const uint8_t* ipe = ip + MINMATCH;
            while (ipe < iend - LASTLITERALS && *mp == *ipe) { mp++; ipe++; ml++; }
            ip = ipe;
        }

        if (ml >= 15) {
            int ml2 = ml - 15;
            *token |= 15;
            while (ml2 >= 255) { *op++ = 255; ml2 -= 255; }
            *op++ = (uint8_t)ml2;
        } else {
            *token |= (uint8_t)ml;
        }

        anchor = ip;
        if (ip > mflimit) goto _last_literals;
        htable[LZ4_hash4(LZ4_read32(ip - 2))] = ip - 2;
        ip++;
    }

_last_literals:
    {
        size_t ll = (size_t)(iend - anchor);
        if ((op + ll + 1 + (ll >> 8) + 4) > oend) return 0;
        if (ll >= 15) {
            size_t ll2 = ll - 15;
            *op++ = (15 << 4);
            while (ll2 >= 255) { *op++ = 255; ll2 -= 255; }
            *op++ = (uint8_t)ll2;
        } else {
            *op++ = (uint8_t)(ll << 4);
        }
        memcpy(op, anchor, ll); op += ll;
    }
    return (int)(op - (uint8_t*)dst);
}

static int LZ4_decompress_safe(const char* src, char* dst, int compressedSize, int dstCapacity) {
    const uint8_t* ip  = (const uint8_t*)src;
    const uint8_t* iend = ip + compressedSize;
    uint8_t* op   = (uint8_t*)dst;
    uint8_t* oend = op + dstCapacity;

    for (;;) {
        uint8_t token = *ip++;
        size_t ll = token >> 4;
        size_t ml;

        // Literal length
        if (ll == 15) { uint8_t s; do { s = *ip++; ll += s; } while (s == 255 && ip < iend); }
        if (op + ll > oend || ip + ll > iend) return -1;
        memcpy(op, ip, ll); op += ll; ip += ll;
        if (ip >= iend) break;

        // Match offset
        uint16_t offset = LZ4_read16(ip); ip += 2;
        uint8_t* match = op - offset;
        if (match < (uint8_t*)dst) return -1;

        // Match length
        ml = (token & 0xF) + MINMATCH;
        if (ml == MINMATCH + 15) {
            uint8_t s; do { s = *ip++; ml += s; } while (s == 255 && ip < iend);
        }
        if (op + ml > oend) return -1;

        // Copy match (handles overlap)
        if (offset < (uint16_t)ml) {
            for (size_t i = 0; i < ml; i++) *op++ = match[i % offset];
        } else {
            memcpy(op, match, ml); op += ml;
        }
    }
    return (int)(op - (uint8_t*)dst);
}
#endif // XDL_HAVE_LZ4

// ─────────────────────────────────────────────────────────────────────────────
// CompressionEngine methods
// ─────────────────────────────────────────────────────────────────────────────

#include <stdexcept>
#include <string>

namespace xdl {

size_t CompressionEngine::max_compressed_size(CompressionType type, size_t src_size) {
    switch (type) {
        case CompressionType::None: return src_size;
        case CompressionType::LZ4:  return static_cast<size_t>(LZ4_compressBound(static_cast<int>(src_size)));
    }
    return src_size;
}

// Thread-local scratch buffer to avoid repeated allocations during page writes.
// Each flush_page call compresses into this buffer, which grows as needed.
static thread_local std::vector<char> tl_compress_scratch;

size_t CompressionEngine::compress(
    CompressionType    type,
    const char*        src,
    size_t             src_size,
    std::vector<char>& dst)
{
    if (type == CompressionType::None) {
        dst.assign(src, src + src_size);
        return src_size;
    }

    // LZ4: reuse thread-local scratch for the destination buffer
    size_t bound = max_compressed_size(type, src_size);
    if (tl_compress_scratch.size() < bound)
        tl_compress_scratch.resize(bound);

    int result = LZ4_compress_default(
        src, tl_compress_scratch.data(),
        static_cast<int>(src_size),
        static_cast<int>(bound));

    if (result <= 0)
        throw CompressionError("LZ4 compression failed for src_size=" + std::to_string(src_size));

    // Direct memcpy instead of iterator-based assign (avoids iterator overhead)
    dst.resize(static_cast<size_t>(result));
    std::memcpy(dst.data(), tl_compress_scratch.data(), static_cast<size_t>(result));
    return static_cast<size_t>(result);
}

void CompressionEngine::decompress(
    CompressionType type,
    const char*     src,
    size_t          src_size,
    char*           dst,
    size_t          expected_size)
{
    if (type == CompressionType::None) {
        if (src_size != expected_size)
            throw CompressionError("Uncompressed size mismatch");
        std::memcpy(dst, src, src_size);
        return;
    }

    // LZ4
    int result = LZ4_decompress_safe(
        src, dst,
        static_cast<int>(src_size),
        static_cast<int>(expected_size));

    if (result < 0 || static_cast<size_t>(result) != expected_size)
        throw CompressionError("LZ4 decompression failed; expected=" +
                               std::to_string(expected_size) +
                               " got=" + std::to_string(result));
}

const char* CompressionEngine::name(CompressionType type) {
    switch (type) {
        case CompressionType::None: return "none";
        case CompressionType::LZ4:  return "lz4";
    }
    return "unknown";
}

} // namespace xdl