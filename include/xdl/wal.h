#pragma once

#include "xdl/types.h"

#include <cstdint>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// WAL Constants
// ─────────────────────────────────────────────────────────────────────────────

constexpr uint32_t WAL_HEADER_SIZE   = 32;   // bytes
constexpr uint32_t WAL_VERSION       = 1;
constexpr uint32_t WAL_RECORD_HEADER = 13;   // crc4 + len4 + type1 + (row4+page4+slot4 for INSERT)

// ─────────────────────────────────────────────────────────────────────────────
// WAL — Write-Ahead Log
// ─────────────────────────────────────────────────────────────────────────────

class WAL {
public:
    explicit WAL(const std::string& wal_path);
    ~WAL();

    void open();
    void close();
    bool is_open() const;

    // Write a WAL record for an insert.  Flushes to disk immediately (fsync).
    // Returns the byte offset of this WAL record in the file.
    uint64_t log_insert(uint32_t row_id, uint32_t page_id, uint32_t row_slot,
                        const char* row_data, uint32_t row_data_len);

    // Zero-copy variant: writes directly from caller's buffer (no intermediate copy).
    uint64_t log_insert_direct(uint32_t row_id, uint32_t page_id, uint32_t row_slot,
                               const char* row_data, uint32_t row_data_len);

    // Write a COMMIT record (no data).  Called after page is flushed to main DB.
    void log_commit();

    // Write a CHECKPOINT record.  Called after all dirty pages flushed.
    void log_checkpoint();

    // Replay WAL: read all records and return them as a vector of WALRecord.
    // Any trailing corrupted/truncated record is silently ignored.
    struct WALRecord {
        enum Type : uint8_t { INSERT = 0, COMMIT = 1, CHECKPOINT = 2, ROLLBACK = 3 };
        Type       type;
        uint32_t   row_id;
        uint32_t   page_id;
        uint32_t   row_slot;
        std::vector<char> row_data; // only for INSERT
    };
    std::vector<WALRecord> replay();

    // Destroy the WAL file (called after successful checkpoint).
    void clear();

    uint64_t file_size() const;

private:
    uint32_t compute_crc(const char* data, uint32_t len) const;

    // Write a complete record: [crc32:4][total_len:4][type:1][payload:N]
    // Returns the offset in the file where this record starts.
    uint64_t write_record(WALRecord::Type type,
                          const char* payload, uint32_t payload_len,
                          uint32_t row_id = 0, uint32_t page_id = 0, uint32_t row_slot = 0);

    // Build a single contiguous buffer for the record body (everything after the CRC).
    // body = [record_len:4][type:1][row_id:4][page_id:4][row_slot:4][data:N]
    // For non-INSERT records row_id/page_id/row_slot are 0 and data is empty.
    std::vector<char> build_record_body(WALRecord::Type type,
                                        const char* row_data, uint32_t row_data_len,
                                        uint32_t row_id, uint32_t page_id, uint32_t row_slot) const;

    void ensure_buffer_space(size_t needed);
    void flush_write_buffer();
    void fsync_if_dirty();

    std::string path_;
    int         fd_ = -1;
    bool        open_ = false;

    // Write buffer: accumulates records, flushed on checkpoint/close or when full
    std::vector<char> write_buf_;
    bool              buf_dirty_ = false; // true if buffer has been written to but not fsync'd

    // Running file offset to avoid lseek(SEEK_END) per record
    uint64_t current_offset_ = 0;
};

} // namespace xdl
