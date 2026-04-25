#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <stdexcept>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

constexpr uint32_t XDL_MAGIC          = 0x58444C31; // "XDL1"
constexpr uint32_t XDL_VERSION        = 1;
constexpr uint32_t PAGE_SIZE          = 8192;        // 8 KB default
constexpr uint32_t MAX_ROWS_PER_PAGE  = 64;
constexpr uint32_t NAME_MAX_LEN       = 64;
constexpr uint32_t INVALID_PAGE_ID    = UINT32_MAX;
constexpr uint32_t DEFAULT_CACHE_SIZE = 256;         // pages

// ─────────────────────────────────────────────────────────────────────────────
// Compression type tag (1 byte on disk)
// ─────────────────────────────────────────────────────────────────────────────

enum class CompressionType : uint8_t {
    None = 0,
    LZ4  = 1,
};

// ─────────────────────────────────────────────────────────────────────────────
// Row  — the fundamental user-visible record
// ─────────────────────────────────────────────────────────────────────────────

struct Row {
    uint32_t id;
    uint32_t age;
    uint16_t name_len;
    char     name[NAME_MAX_LEN];

    Row() : id(0), age(0), name_len(0) { name[0] = '\0'; }

    Row(uint32_t id_, uint32_t age_, const std::string& name_str)
        : id(id_), age(age_)
    {
        size_t len = std::min(name_str.size(), static_cast<size_t>(NAME_MAX_LEN - 1));
        name_len   = static_cast<uint16_t>(len);
        std::memcpy(name, name_str.data(), len);
        name[len]  = '\0';
    }

    std::string name_str() const { return std::string(name, name_len); }

    // Serialised byte width (fixed — no padding surprises across platforms)
    static constexpr size_t serialized_size() {
        return sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint16_t) + NAME_MAX_LEN;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Exceptions
// ─────────────────────────────────────────────────────────────────────────────

struct XDLError : std::runtime_error {
    explicit XDLError(const std::string& msg) : std::runtime_error(msg) {}
};

struct CorruptionError  : XDLError { using XDLError::XDLError; };
struct DuplicateKeyError: XDLError { using XDLError::XDLError; };
struct NotFoundError    : XDLError { using XDLError::XDLError; };
struct IOError          : XDLError { using XDLError::XDLError; };
struct CompressionError : XDLError { using XDLError::XDLError; };

} // namespace xdl