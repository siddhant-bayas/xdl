#pragma once

#include "types.h"
#include <vector>
#include <cstring>

namespace xdl {

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
// ─────────────────────────────────────────────────────────────────────────────

struct Page {
    uint32_t          id;
    std::vector<char> data;     // uncompressed serialised rows
    uint32_t          row_count;
    bool              dirty;

    Page() : id(INVALID_PAGE_ID), row_count(0), dirty(false) {}

    explicit Page(uint32_t page_id)
        : id(page_id), row_count(0), dirty(false)
    {
        data.reserve(PAGE_SIZE);
    }

    bool has_capacity() const {
        return row_count < MAX_ROWS_PER_PAGE;
    }

    // Append a row; caller must check has_capacity() first
    void append_row(const Row& row);

    // Decode the idx-th row (0-based)
    Row get_row(uint32_t idx) const;

    // Find row by id; returns -1 if absent
    int find_row(uint32_t id) const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Row serialisation helpers (platform-independent, little-endian explicit)
// ─────────────────────────────────────────────────────────────────────────────

namespace serialise {

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

} // namespace serialise

} // namespace xdl