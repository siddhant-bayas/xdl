#pragma once

#include "types.h"
#include "page.h"
#include <string>
#include <fstream>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// DB File Header (first bytes of the file)
// ─────────────────────────────────────────────────────────────────────────────

#pragma pack(push, 1)
struct DBHeader {
    uint32_t magic;           // XDL_MAGIC
    uint32_t version;         // XDL_VERSION
    uint32_t page_size;       // logical page size
    uint32_t page_count;      // number of data pages written
    uint8_t  compression;     // default CompressionType for new pages
    uint8_t  _pad[3];
};
#pragma pack(pop)

static_assert(sizeof(DBHeader) == 20, "DBHeader layout changed");

// ─────────────────────────────────────────────────────────────────────────────
// StorageEngine
//
// Owns the file handle.  All I/O goes through here.
// Compression is handled *above* this layer (in Pager), so StorageEngine only
// sees raw byte buffers.
// ─────────────────────────────────────────────────────────────────────────────

class StorageEngine {
public:
    explicit StorageEngine(const std::string& path, CompressionType compression);
    ~StorageEngine();

    // Non-copyable, moveable
    StorageEngine(const StorageEngine&)            = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;
    StorageEngine(StorageEngine&&)                 = default;
    StorageEngine& operator=(StorageEngine&&)      = default;

    // Open existing or create new database file
    void open();
    void close();
    bool is_open() const;

    // Read page header + compressed blob at given file offset
    void read_page_raw(uint64_t offset, PageHeader& header, std::vector<char>& blob);

    // Append header + blob to end of file; returns file offset of this page
    uint64_t append_page_raw(const PageHeader& header, const std::vector<char>& blob);

    // Overwrite an existing page in-place (header + blob must be same size as original)
    void overwrite_page_raw(uint64_t offset, const PageHeader& header, const std::vector<char>& blob);

    // DB-level metadata
    DBHeader  read_db_header();
    void      write_db_header(const DBHeader& hdr);

    uint64_t  file_size() const;
    const std::string& path() const { return path_; }
    CompressionType    default_compression() const { return compression_; }

private:
    std::string     path_;
    CompressionType compression_;
    std::fstream    file_;
    bool            open_ = false;
};

} // namespace xdl