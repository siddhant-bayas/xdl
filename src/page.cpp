#include "xdl/page.h"
#include <stdexcept>
#include <cstring>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// Row serialisation
//
// Wire format for one row (all integers little-endian):
//
//   Offset  Size  Description
//   ──────  ────  ───────────
//   0       4     total row length in bytes (includes this 4-byte header)
//   4       4     row id
//   8       ...   fields, one after another:
//                   UINT32  → 4 bytes
//                   STRING  → 2 bytes (length N) + N bytes of string data
//                   FLOAT32 → 4 bytes (IEEE 754 copy)
//                   BOOL    → 1 byte (0 or 1)
//                   INT64   → 8 bytes
//
// row_offsets[] caches the byte offset of the start (the length word) of
// each row inside Page::data, so get_row(idx) is O(1).
// ─────────────────────────────────────────────────────────────────────────────

// Compute serialised byte size of one field, validating that the variant holds
// the type declared in the schema.  Throws XDLError (not bad_variant_access) so
// batch validation can reject the whole batch before anything is written.
static uint32_t field_byte_size(const FieldValue& v, FieldType type,
                                const std::string& name) {
    switch (type) {
    case FieldType::UINT32:
        if (!std::holds_alternative<uint32_t>(v))
            throw XDLError("Field '" + name + "' expects uint32");
        return 4;
    case FieldType::STRING: {
        if (!std::holds_alternative<std::string>(v))
            throw XDLError("Field '" + name + "' expects string");
        const std::string& s = std::get<std::string>(v);
        if (s.size() > 65535) {
            throw XDLError("String field '" + name + "' is too large: " +
                           std::to_string(s.size()) +
                           " bytes (maximum 65535)");
        }
        return 2 + static_cast<uint32_t>(s.size());
    }
    case FieldType::FLOAT32:
        if (!std::holds_alternative<float>(v))
            throw XDLError("Field '" + name + "' expects float32");
        return 4;
    case FieldType::BOOL:
        if (!std::holds_alternative<bool>(v))
            throw XDLError("Field '" + name + "' expects bool");
        return 1;
    case FieldType::INT64:
        if (!std::holds_alternative<int64_t>(v))
            throw XDLError("Field '" + name + "' expects int64");
        return 8;
    }
    throw XDLError("Unknown field type for '" + name + "'");
}

// Compute serialised byte size of one row given its field values and schema.
static uint32_t row_byte_size(const Row& row, const Schema& schema) {
    // 4 bytes length header + 4 bytes id
    uint32_t sz = 4 + 4;
    for (size_t i = 0; i < schema.fields.size(); ++i) {
        sz += field_byte_size(row.fields.at(i), schema.fields[i].type,
                              schema.fields[i].name);
    }
    return sz;
}

uint32_t serialised_row_size(const Row& row, const Schema& schema) {
    return row_byte_size(row, schema);
}

void Page::append_row(const Row& row, const Schema& schema) {
    uint32_t offset = static_cast<uint32_t>(data.size());
    uint32_t total  = row_byte_size(row, schema);

    if (total > PAGE_SIZE)
        throw XDLError("Row too large for a page: " + std::to_string(total) +
                       " bytes (page limit " + std::to_string(PAGE_SIZE) + ")");

    data.resize(offset + total);
    char* dst = data.data() + offset;

    // length header
    serialise::write_u32(dst, total); dst += 4;
    // row id
    serialise::write_u32(dst, row.id); dst += 4;

    for (size_t i = 0; i < schema.fields.size(); ++i) {
        switch (schema.fields[i].type) {
        case FieldType::UINT32:
            serialise::write_u32(dst, std::get<uint32_t>(row.fields.at(i)));
            dst += 4;
            break;
        case FieldType::STRING: {
            const std::string& s = std::get<std::string>(row.fields.at(i));
            uint16_t len = static_cast<uint16_t>(s.size());
            serialise::write_u16(dst, len); dst += 2;
            std::memcpy(dst, s.data(), len); dst += len;
            break;
        }
        case FieldType::FLOAT32: {
            float f = std::get<float>(row.fields.at(i));
            uint32_t bits;
            std::memcpy(&bits, &f, 4);
            serialise::write_u32(dst, bits);
            dst += 4;
            break;
        }
        case FieldType::BOOL: {
            bool b = std::get<bool>(row.fields.at(i));
            serialise::write_u8(dst, b ? 1 : 0);
            dst += 1;
            break;
        }
        case FieldType::INT64: {
            int64_t v = std::get<int64_t>(row.fields.at(i));
            serialise::write_u64(dst, static_cast<uint64_t>(v));
            dst += 8;
            break;
        }
        }
    }

    row_offsets.push_back(offset);
    ++row_count;
    dirty = true;
}

void Page::erase_row(uint32_t idx) {
    if (idx >= row_count)
        throw XDLError("Row index out of bounds: " + std::to_string(idx));

    uint32_t start = row_offsets[idx];
    uint32_t total = serialise::read_u32(data.data() + start);

    data.erase(data.begin() + start, data.begin() + start + total);
    row_offsets.erase(row_offsets.begin() + idx);
    --row_count;
    for (uint32_t i = idx; i < row_count; ++i)
        row_offsets[i] -= total;
    dirty = true;
}

Row Page::get_row(uint32_t idx, const Schema& schema) const {
    if (idx >= row_count)
        throw XDLError("Row index out of bounds: " + std::to_string(idx));

    const char* src = data.data() + row_offsets[idx];
    // skip 4-byte length header
    src += 4;
    uint32_t row_id = serialise::read_u32(src); src += 4;

    std::vector<FieldValue> fv;
    fv.reserve(schema.fields.size());

    for (const auto& fd : schema.fields) {
        switch (fd.type) {
        case FieldType::UINT32: {
            uint32_t v = serialise::read_u32(src); src += 4;
            fv.emplace_back(v);
            break;
        }
        case FieldType::STRING: {
            uint16_t len = serialise::read_u16(src); src += 2;
            fv.emplace_back(std::string(src, len)); src += len;
            break;
        }
        case FieldType::FLOAT32: {
            float v;
            serialise::read_f32(src, v);
            src += 4;
            fv.emplace_back(v);
            break;
        }
        case FieldType::BOOL: {
            fv.emplace_back(serialise::read_u8(src) != 0);
            src += 1;
            break;
        }
        case FieldType::INT64: {
            uint64_t u = serialise::read_u64(src); src += 8;
            int64_t v;
            std::memcpy(&v, &u, 8);
            fv.emplace_back(v);
            break;
        }
        }
    }

    return Row{row_id, std::move(fv)};
}

int Page::find_row(uint32_t row_id, const Schema& schema) const {
    for (uint32_t i = 0; i < row_count; ++i) {
        // id is always at byte 4 inside each row entry
        const char* src = data.data() + row_offsets[i] + 4;
        if (serialise::read_u32(src) == row_id)
            return static_cast<int>(i);
    }
    (void)schema;
    return -1;
}

std::vector<char> Page::row_data_copy(uint32_t idx, const Schema& schema) const {
    if (idx >= row_count)
        throw XDLError("Row index out of bounds: " + std::to_string(idx));
    const char* base = data.data() + row_offsets[idx];
    uint32_t total = serialise::read_u32(base);
    return std::vector<char>(base, base + total);
}

} // namespace xdl
