#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <variant>
#include <stdexcept>
#include <shared_mutex>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

constexpr uint32_t XDL_MAGIC           = 0x58444C31; // "XDL1"
constexpr uint32_t XDL_VERSION         = 1;
constexpr uint32_t XDL_VERSION_LEGACY  = 0;          // no legacy versions in v1
constexpr uint32_t PAGE_SIZE           = 8192;        // 8 KB default
constexpr uint32_t MAX_ROWS_PER_PAGE   = 64;
constexpr uint32_t INVALID_PAGE_ID     = UINT32_MAX;
constexpr uint32_t DEFAULT_CACHE_SIZE  = 256;         // pages
constexpr uint32_t BPLUS_TREE_ORDER    = 32;          // max keys per B+ tree node
constexpr uint64_t WAL_MAGIC           = 0x57414C31;  // "WAL1" little-endian
constexpr uint32_t WAL_CHECKSUM_SEED   = 0xDEADBEEF;

// ─────────────────────────────────────────────────────────────────────────────
// Compression type tag (1 byte on disk)
// ─────────────────────────────────────────────────────────────────────────────

enum class CompressionType : uint8_t {
    None = 0,
    LZ4  = 1,
};

// ─────────────────────────────────────────────────────────────────────────────
// Schema — dynamic field definitions
// ─────────────────────────────────────────────────────────────────────────────

enum class FieldType : uint8_t {
    UINT32  = 0,   // 32-bit unsigned integer  (4 bytes)
    STRING  = 1,   // variable-length UTF-8    (2-byte length + data)
    FLOAT32 = 2,   // 32-bit IEEE 754 float    (4 bytes)
    BOOL    = 3,   // boolean                  (1 byte)
    INT64   = 4,   // 64-bit signed integer    (8 bytes)
};

// Wire-size of a fixed-width field type (0 = variable).
inline uint32_t field_type_width(FieldType t) {
    switch (t) {
        case FieldType::UINT32:  return 4;
        case FieldType::FLOAT32: return 4;
        case FieldType::BOOL:    return 1;
        case FieldType::INT64:   return 8;
        case FieldType::STRING:  return 0; // variable
    }
    return 0;
}

inline const char* field_type_name(FieldType t) {
    switch (t) {
        case FieldType::UINT32:  return "uint32";
        case FieldType::STRING:  return "string";
        case FieldType::FLOAT32: return "float32";
        case FieldType::BOOL:    return "bool";
        case FieldType::INT64:   return "int64";
    }
    return "unknown";
}

// Parse "uint32", "string", "float32", "bool", "int64" → FieldType
inline FieldType parse_field_type(const std::string& s) {
    if (s == "uint32")  return FieldType::UINT32;
    if (s == "string")  return FieldType::STRING;
    if (s == "float32") return FieldType::FLOAT32;
    if (s == "bool")    return FieldType::BOOL;
    if (s == "int64")   return FieldType::INT64;
    throw std::invalid_argument("Unknown field type: " + s);
}

struct FieldDef {
    std::string name;
    FieldType   type;

    FieldDef() = default;
    FieldDef(std::string n, FieldType t) : name(std::move(n)), type(t) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// SchemaVersion — tracks schema generations for migration support
// ─────────────────────────────────────────────────────────────────────────────

struct SchemaVersion {
    uint32_t version = 1;
};

// ─────────────────────────────────────────────────────────────────────────────
// Schema — dynamic field definitions with versioning
// ─────────────────────────────────────────────────────────────────────────────

struct Schema {
    std::vector<FieldDef> fields;
    SchemaVersion         ver;

    Schema() = default;
    Schema(std::initializer_list<FieldDef> defs) : fields(defs) {}

    // Returns the index of a field by name, or -1 if not found.
    int field_index(const std::string& name) const {
        for (size_t i = 0; i < fields.size(); ++i)
            if (fields[i].name == name) return static_cast<int>(i);
        return -1;
    }

    // Validates the schema is non-empty and all names are unique.
    void validate() const {
        if (fields.empty())
            throw std::invalid_argument("Schema must have at least one field");
        for (size_t i = 0; i < fields.size(); ++i)
            for (size_t j = i + 1; j < fields.size(); ++j)
                if (fields[i].name == fields[j].name)
                    throw std::invalid_argument("Duplicate field name: " + fields[i].name);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// FieldValue — a single typed field value
// ─────────────────────────────────────────────────────────────────────────────

using FieldValue = std::variant<uint32_t, std::string, float, bool, int64_t>;

// ─────────────────────────────────────────────────────────────────────────────
// Row — the fundamental user-visible record
//
// Fields are stored in the same order as the Schema they were created with.
// Use positional accessors for speed, or named overloads for convenience.
// ─────────────────────────────────────────────────────────────────────────────

struct Row {
    uint32_t                id;
    std::vector<FieldValue> fields;

    Row() : id(0) {}

    Row(uint32_t id_, std::vector<FieldValue> fv)
        : id(id_), fields(std::move(fv)) {}

    // Typed accessors — throw if field index is out of range or wrong type.
    uint32_t           get_uint32(size_t idx) const { return std::get<uint32_t>(fields.at(idx)); }
    const std::string& get_string (size_t idx) const { return std::get<std::string>(fields.at(idx)); }
    float              get_float32(size_t idx) const { return std::get<float>(fields.at(idx)); }
    bool               get_bool(size_t idx) const { return std::get<bool>(fields.at(idx)); }
    int64_t            get_int64(size_t idx) const { return std::get<int64_t>(fields.at(idx)); }

    // Named accessors — require a Schema reference.
    uint32_t get_uint32(const Schema& schema, const std::string& name) const {
        int idx = schema.field_index(name);
        if (idx < 0) throw std::invalid_argument("Unknown field: " + name);
        return get_uint32(static_cast<size_t>(idx));
    }
    const std::string& get_string(const Schema& schema, const std::string& name) const {
        int idx = schema.field_index(name);
        if (idx < 0) throw std::invalid_argument("Unknown field: " + name);
        return get_string(static_cast<size_t>(idx));
    }
    float get_float32(const Schema& schema, const std::string& name) const {
        int idx = schema.field_index(name);
        if (idx < 0) throw std::invalid_argument("Unknown field: " + name);
        return get_float32(static_cast<size_t>(idx));
    }
    bool get_bool(const Schema& schema, const std::string& name) const {
        int idx = schema.field_index(name);
        if (idx < 0) throw std::invalid_argument("Unknown field: " + name);
        return get_bool(static_cast<size_t>(idx));
    }
    int64_t get_int64(const Schema& schema, const std::string& name) const {
        int idx = schema.field_index(name);
        if (idx < 0) throw std::invalid_argument("Unknown field: " + name);
        return get_int64(static_cast<size_t>(idx));
    }

    // Set a field by index.
    void set(size_t idx, FieldValue v) { fields.at(idx) = std::move(v); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Reader-Writer Lock — wraps std::shared_mutex for concurrent read access
// ─────────────────────────────────────────────────────────────────────────────

class RWLock {
public:
    // Exclusive (write) lock
    void lock()   { mtx_.lock(); }
    void unlock() { mtx_.unlock(); }

    // Shared (read) lock
    void lock_shared()   { mtx_.lock_shared(); }
    void unlock_shared() { mtx_.unlock_shared(); }

    bool try_lock()        { return mtx_.try_lock(); }
    bool try_lock_shared() { return mtx_.try_lock_shared(); }

private:
    std::shared_mutex mtx_;
};

// RAII wrappers
class ReadLock {
public:
    explicit ReadLock(RWLock& lk) : lk_(lk) { lk_.lock_shared(); }
    ~ReadLock() { lk_.unlock_shared(); }
    ReadLock(const ReadLock&) = delete;
    ReadLock& operator=(const ReadLock&) = delete;
private:
    RWLock& lk_;
};

class WriteLock {
public:
    explicit WriteLock(RWLock& lk) : lk_(lk) { lk_.lock(); }
    ~WriteLock() { lk_.unlock(); }
    WriteLock(const WriteLock&) = delete;
    WriteLock& operator=(const WriteLock&) = delete;
private:
    RWLock& lk_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Exceptions
// ─────────────────────────────────────────────────────────────────────────────

struct XDLError : std::runtime_error {
    explicit XDLError(const std::string& msg) : std::runtime_error(msg) {}
};

struct CorruptionError     : XDLError { using XDLError::XDLError; };
struct DuplicateKeyError   : XDLError { using XDLError::XDLError; };
struct NotFoundError       : XDLError { using XDLError::XDLError; };
struct IOError             : XDLError { using XDLError::XDLError; };
struct CompressionError    : XDLError { using XDLError::XDLError; };
struct MigrationError      : XDLError { using XDLError::XDLError; };
class  WALError            : public XDLError { public: using XDLError::XDLError; };

} // namespace xdl
