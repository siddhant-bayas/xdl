#include "xdl/migrate.h"
#include <cstring>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// Schema serialisation / deserialisation
// ─────────────────────────────────────────────────────────────────────────────

std::vector<char> serialise_schema(const Schema& schema) {
    std::vector<char> out;
    // Header: version(4) + field_count(4)
    auto append_u32 = [&](uint32_t v) {
        char tmp[4];
        tmp[0] = static_cast<char>(v & 0xFF);
        tmp[1] = static_cast<char>((v >> 8) & 0xFF);
        tmp[2] = static_cast<char>((v >> 16) & 0xFF);
        tmp[3] = static_cast<char>((v >> 24) & 0xFF);
        out.insert(out.end(), tmp, tmp + 4);
    };

    append_u32(schema.ver.version);
    append_u32(static_cast<uint32_t>(schema.fields.size()));

    for (const auto& f : schema.fields) {
        uint16_t nlen = static_cast<uint16_t>(f.name.size() > 0xFFFF ? 0xFFFF : f.name.size());
        char nlen_buf[2];
        nlen_buf[0] = static_cast<char>(nlen & 0xFF);
        nlen_buf[1] = static_cast<char>((nlen >> 8) & 0xFF);
        out.insert(out.end(), nlen_buf, nlen_buf + 2);
        out.insert(out.end(), f.name.begin(), f.name.begin() + nlen);
        out.push_back(static_cast<char>(f.type));
    }
    return out;
}

Schema deserialise_schema(const char* data, size_t len) {
    Schema schema;
    size_t pos = 0;
    auto read_u32 = [&](uint32_t& v) {
        if (pos + 4 > len) throw CorruptionError("Schema truncated");
        v = static_cast<uint8_t>(data[pos]) |
            (static_cast<uint8_t>(data[pos+1]) << 8) |
            (static_cast<uint8_t>(data[pos+2]) << 16) |
            (static_cast<uint8_t>(data[pos+3]) << 24);
        pos += 4;
    };
    auto read_u16 = [&](uint16_t& v) {
        if (pos + 2 > len) throw CorruptionError("Schema truncated at field name length");
        v = static_cast<uint8_t>(data[pos]) |
            (static_cast<uint8_t>(data[pos+1]) << 8);
        pos += 2;
    };

    uint32_t ver = 0;
    read_u32(ver);
    schema.ver.version = ver;

    uint32_t count = 0;
    read_u32(count);
    if (count == 0 || count > 1024) throw CorruptionError("Schema field count invalid");

    for (uint32_t i = 0; i < count; ++i) {
        uint16_t nlen = 0;
        read_u16(nlen);
        if (pos + nlen + 1 > len) throw CorruptionError("Schema field truncated");
        std::string name(data + pos, static_cast<size_t>(nlen));
        pos += nlen;
        FieldType ft = static_cast<FieldType>(data[pos]);
        pos += 1;
        schema.fields.emplace_back(std::move(name), ft);
    }
    return schema;
}

// ─────────────────────────────────────────────────────────────────────────────
// SchemaMigrator
// ─────────────────────────────────────────────────────────────────────────────

Row SchemaMigrator::apply_to_row(const Row& old_row,
                                 const Schema& old_schema,
                                 const Schema& new_schema,
                                 const SchemaMigrate& migration)
{
    // Build new field vector by mapping old fields to new positions.
    std::vector<FieldValue> new_fields(new_schema.fields.size());

    // First, copy over fields that exist in both schemas (by name).
    for (size_t ni = 0; ni < new_schema.fields.size(); ++ni) {
        const auto& nf = new_schema.fields[ni];
        int oi = old_schema.field_index(nf.name);
        if (oi >= 0) {
            // Field exists in old schema — copy value
            new_fields[ni] = old_row.fields[static_cast<size_t>(oi)];
        }
    }

    // Then apply migration ops: RENAME (copy data), ADD (set default).
    for (const auto& op : migration.ops) {
        if (op.op == MigrateOp::RENAME) {
            int ni = new_schema.field_index(op.new_name);
            int oi = old_schema.field_index(op.field_name);
            if (ni >= 0 && oi >= 0) {
                new_fields[static_cast<size_t>(ni)] = old_row.fields[static_cast<size_t>(oi)];
            }
        } else if (op.op == MigrateOp::ADD) {
            int ni = new_schema.field_index(op.field_name);
            if (ni >= 0 && ni < static_cast<int>(new_fields.size())) {
                new_fields[static_cast<size_t>(ni)] = op.default_value;
            }
        }
    }

    return Row{old_row.id, std::move(new_fields)};
}

bool SchemaMigrator::is_compatible(const Schema& old_schema,
                                   const Schema& new_schema,
                                   const SchemaMigrate& migration)
{
    // Verify that every field in new_schema is either:
    //   (a) present in old_schema by name, or
    //   (b) added by a MigrateOp::ADD in the migration
    for (const auto& nf : new_schema.fields) {
        if (old_schema.field_index(nf.name) >= 0) continue;
        bool found_add = false;
        for (const auto& op : migration.ops) {
            if (op.op == MigrateOp::ADD && op.field_name == nf.name) {
                found_add = true;
                break;
            }
        }
        if (!found_add) return false;
    }
    return true;
}

} // namespace xdl
