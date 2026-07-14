#pragma once

#include "types.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <stdexcept>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// MmapFile — memory-mapped file I/O
//
// A simple wrapper around POSIX mmap.  The entire file is mapped into a
// contiguous region.  Growing the file requires munmap + remap.
// ─────────────────────────────────────────────────────────────────────────────

class MmapFile {
public:
    MmapFile();
    ~MmapFile();

    MmapFile(const MmapFile&) = delete;
    MmapFile& operator=(const MmapFile&) = delete;

    // Open existing file or create new.  If 'create' and initial_size > 0 the
    // file is created/ftruncated to that size.
    void open(const std::string& path, bool create = false, size_t initial_size = 0);
    void close();
    bool is_open() const { return addr_ != nullptr; }

    // Read / write at byte offset.  No bounds checking — caller must ensure
    // offset + len <= size_.
    void read (uint64_t offset, void* dst, size_t len) const;
    void write(uint64_t offset, const void* src, size_t len);

    // Grow the file (and mapping) to at least min_size bytes.
    // Uses 2× growth strategy to amortise remap cost.
    void ensure_size(size_t min_size);

    // Flush all changes to disk (MS_SYNC).
    void flush();

    size_t   size() const { return size_; }
    uint8_t* data() const { return static_cast<uint8_t*>(addr_); }
    int      fd()   const { return fd_; }

private:
    void remap(size_t new_size);

    std::string path_;
    int         fd_   = -1;
    void*       addr_ = nullptr;
    size_t      size_ = 0;
};

} // namespace xdl
