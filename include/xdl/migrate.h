#pragma once

#include "types.h"
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <functional>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// SchemaMigration — support for schema evolution
//
// A migration defines a transformation from one schema version to the next.
// Supported operations:
//   - ADD column (with optional default value)
//   - DROP column
//   - RENAME column
//
// Migrations are applied sequentially to bring a database from its on-disk
// schema version to the current code's expected schema.
// ─────────────────────────────────────────────────────────────────────────────

enum class MigrateOp : uint8_t {
    ADD    = 0,
    DROP   = 1,
    RENAME = 2,
};

struct MigrateEntry {
    MigrateOp   op;
    std::string field_name;       // column name (for ADD the new name, for DROP/RENAME the old name)
    std::string new_name;         // for RENAME: the new column name
    FieldType   type;             // for ADD: the type of the new column
    FieldValue  default_value;    // for ADD: default value for existing rows
};

struct SchemaMigrate {
    uint32_t                from_version = 0;
    uint32_t                to_version   = 0;
    std::vector<MigrateEntry> ops;
};

// Apply a migration to all pages in the database.
// This is a full-table rewrite: it reads every row, transforms it, and
// writes it back with the new schema.  Only called during open() when a
// schema version mismatch is detected.
//
// Visitor pattern: the caller provides callbacks for storage I/O.
class SchemaMigrator {
public:
    // Apply migration to a single row.
    static Row apply_to_row(const Row& old_row,
                            const Schema& old_schema,
                            const Schema& new_schema,
                            const SchemaMigrate& migration);

    // Check if the migration chain can transform old_schema → new_schema.
    static bool is_compatible(const Schema& old_schema,
                              const Schema& new_schema,
                              const SchemaMigrate& migration);
};

// Serialise a schema to a byte vector (stored in the DB header area).
std::vector<char> serialise_schema(const Schema& schema);

// Deserialise a schema from a byte vector.
Schema deserialise_schema(const char* data, size_t len);

} // namespace xdl
