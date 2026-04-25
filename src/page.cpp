#include "xdl/page.h"
#include <cstring>
#include <stdexcept>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// Row layout inside Page::data (after the 4-byte row_count header):
//
//  [4 bytes: row_count][Row::serialised_size() bytes × N]
//
// We keep row_count in the Page struct itself; the raw Page::data vector
// contains *only* the packed rows, no separate count prefix on the vector.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr size_t ROW_BYTES = Row::serialized_size();

void Page::append_row(const Row& row) {
    size_t offset = row_count * ROW_BYTES;
    data.resize(offset + ROW_BYTES);

    char* dst = data.data() + offset;
    serialise::write_u32(dst + 0, row.id);
    serialise::write_u32(dst + 4, row.age);
    serialise::write_u16(dst + 8, row.name_len);
    std::memset(dst + 10, 0, NAME_MAX_LEN);
    std::memcpy(dst + 10, row.name, row.name_len);

    ++row_count;
    dirty = true;
}

Row Page::get_row(uint32_t idx) const {
    if (idx >= row_count)
        throw XDLError("Row index out of bounds: " + std::to_string(idx));

    const char* src = data.data() + idx * ROW_BYTES;
    Row r;
    r.id       = serialise::read_u32(src + 0);
    r.age      = serialise::read_u32(src + 4);
    r.name_len = serialise::read_u16(src + 8);
    std::memcpy(r.name, src + 10, NAME_MAX_LEN);
    r.name[NAME_MAX_LEN - 1] = '\0'; // safety null-term
    return r;
}

int Page::find_row(uint32_t id) const {
    for (uint32_t i = 0; i < row_count; ++i) {
        const char* src = data.data() + i * ROW_BYTES;
        if (serialise::read_u32(src) == id)
            return static_cast<int>(i);
    }
    return -1;
}

} // namespace xdl