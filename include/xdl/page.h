#pragma once

#include "types.h"
#include <vector>
#include <cstring>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// Serialisation helpers (platform-independent, little-endian explicit)
// ─────────────────────────────────────────────────────────────────────────────

namespace serialise {

inline void write_u8(char* dst, uint8_t v) {
    dst[0] = static_cast<char>(v);
}
inline void write_u16(char* dst, uint16_t v) {
    dst[0] = static_cast<char>(v & 0xFF);
    dst[1] = static_cast<char>((v >> 8) & 0xFF);
}
inline void write_u32(char* dst, uint32_t v) {
    dst[0] = static_cast<char>(v & 0xFF);
    dst[1] = static_cast<char>((v >> 8) & 0xFF);
    dst[2] = static_cast<char>((v >> 16) & 0xFF);
    dst[3] = static_cast<char>((v >> 24) & 0xFF);
}
inline void write_u64(char* dst, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        dst[i] = static_cast<char>((v >> (i * 8)) & 0xFF);
}
inline uint8_t  read_u8 (const char* src) {
    return static_cast<uint8_t>(src[0]);
}
inline uint16_t read_u16(const char* src) {
    return static_cast<uint16_t>(
        (static_cast<uint8_t>(src[0])) |
        (static_cast<uint8_t>(src[1]) << 8));
}
inline uint32_t read_u32(const char* src) {
    return
        (static_cast<uint8_t>(src[0]))       |
        (static_cast<uint8_t>(src[1]) << 8)  |
        (static_cast<uint8_t>(src[2]) << 16) |
        (static_cast<uint8_t>(src[3]) << 24);
}
inline uint64_t read_u64(const char* src) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(static_cast<uint8_t>(src[i])) << (i * 8);
    return v;
}
inline void read_f32(const char* src, float& out) {
    uint32_t v = read_u32(src);
    std::memcpy(&out, &v, 4);
}

} // namespace serialise

// ─────────────────────────────────────────────────────────────────────────────
// On-disk page header  (packed, written verbatim to storage)
// ─────────────────────────────────────────────────────────────────────────────

#pragma pack(push, 1)
struct PageHeader {
    uint32_t page_id;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t row_count;
    uint8_t  compression_type;
    uint8_t  _pad[3];                 // reserved, written as 0
};
#pragma pack(pop)

static_assert(sizeof(PageHeader) == 20, "PageHeader layout changed");

// ─────────────────────────────────────────────────────────────────────────────
// In-memory page  (always holds uncompressed row bytes)
//
// Row layout on Page::data (variable-length, little-endian):
//
//   Per row:
//     [4 bytes : total row byte length including this header]
//     [4 bytes : row id]
//     [N fields, each serialised as:]
//       UINT32  → 4 bytes (little-endian)
//       STRING  → 2 bytes length + <length> bytes UTF-8 data
//       FLOAT32 → 4 bytes (IEEE 754, memcpy)
//       BOOL    → 1 byte (0 or 1)
//       INT64   → 8 bytes (little-endian)
// ─────────────────────────────────────────────────────────────────────────────

struct Page {
    uint32_t          id;
    std::vector<char> data;       // uncompressed serialised rows
    uint32_t          row_count;
    bool              dirty;

    // byte offset of the start of each row inside data[] (for random access)
    std::vector<uint32_t> row_offsets;

    Page() : id(INVALID_PAGE_ID), row_count(0), dirty(false) {}

    explicit Page(uint32_t page_id)
        : id(page_id), row_count(0), dirty(false)
    {
        data.reserve(PAGE_SIZE);
    }

    bool has_capacity() const {
        return row_count < MAX_ROWS_PER_PAGE;
    }

    // Append a row; caller must check has_capacity() first.
    void append_row(const Row& row, const Schema& schema);

    // Decode the idx-th row (0-based).
    Row get_row(uint32_t idx, const Schema& schema) const;

    // Find row by id; returns -1 if absent.
    int find_row(uint32_t id, const Schema& schema) const;

    // Export the serialised data for WAL logging.
    std::vector<char> row_data_copy(uint32_t idx, const Schema& schema) const;

    // Zero-copy access: pointer into data[] for the idx-th row.
    const char* row_data_ptr(uint32_t idx) const {
        return data.data() + row_offsets[idx];
    }

    // Byte length of the idx-th row's serialised data.
    uint32_t row_data_len(uint32_t idx) const {
        return serialise::read_u32(data.data() + row_offsets[idx]);
    }
};

} // namespace xdl
