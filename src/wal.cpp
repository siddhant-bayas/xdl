#include "xdl/wal.h"
#include "xdl/types.h"

#include <cstring>
#include <stdexcept>
#include <mutex>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#define xdl_open    _open
#define xdl_close    _close
#define xdl_write    _write
#define xdl_read     _read
#define xdl_fstat    _fstat
#define xdl_fsync    _commit
#define xdl_unlink   _unlink
#define xdl_stat     struct _stat
#define xdl_oflags   (O_BINARY)
#else
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#define xdl_open    ::open
#define xdl_close    ::close
#define xdl_write    ::write
#define xdl_read     ::read
#define xdl_fstat    ::fstat
#define xdl_fsync    ::fsync
#define xdl_unlink   ::unlink
#define xdl_stat     struct stat
#define xdl_oflags   (0)
#endif

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// Local helpers
// ─────────────────────────────────────────────────────────────────────────────

static void safe_write(int fd, const char* data, uint32_t len) {
    uint32_t written = 0;
    while (written < len) {
        int n = xdl_write(fd, data + written, len - written);
        if (n <= 0)
            throw IOError("WAL write failed");
        written += static_cast<uint32_t>(n);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CRC32 Lookup Table (thread-safe lazy init)
// ─────────────────────────────────────────────────────────────────────────────

static uint32_t crc32_table[256];
static std::once_flag crc32_table_flag;

static void init_crc32_table() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320u;
            else
                crc >>= 1;
        }
        crc32_table[i] = crc;
    }
}

static const uint32_t* get_crc32_table() {
    std::call_once(crc32_table_flag, init_crc32_table);
    return crc32_table;
}

uint32_t WAL::compute_crc(const char* data, uint32_t len) const {
    const uint32_t* table = get_crc32_table();
    uint32_t crc = WAL_CHECKSUM_SEED;
    for (uint32_t i = 0; i < len; ++i) {
        crc = table[(crc ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

// ─────────────────────────────────────────────────────────────────────────────
// WAL
// ─────────────────────────────────────────────────────────────────────────────

WAL::WAL(const std::string& wal_path)
    : path_(wal_path) {}

WAL::~WAL() {
    if (open_) close();
}

void WAL::open() {
    if (open_) return;

    fd_ = xdl_open(path_.c_str(), O_RDWR | O_CREAT | xdl_oflags, 0644);
    if (fd_ < 0)
        throw IOError("Cannot open WAL file: " + path_);

    // If the file is new (write header).
    xdl_stat st{};
    if (xdl_fstat(fd_, &st) != 0) {
        xdl_close(fd_);
        fd_ = -1;
        throw IOError("Cannot stat WAL file: " + path_);
    }

    if (st.st_size == 0) {
        // Write WAL header: magic(8) + version(4) + page_size(4) + checksum(4) + pad(12) = 32 bytes
        char header[WAL_HEADER_SIZE];
        std::memset(header, 0, WAL_HEADER_SIZE);

        std::memcpy(header + 0, &WAL_MAGIC, 8);
        std::memcpy(header + 8, &WAL_VERSION, 4);
        std::memcpy(header + 12, &PAGE_SIZE, 4);
        uint32_t hdr_crc = compute_crc(header, 12);
        std::memcpy(header + 16, &hdr_crc, 4);

        safe_write(fd_, header, WAL_HEADER_SIZE);
        xdl_fsync(fd_);
        current_offset_ = WAL_HEADER_SIZE;
    } else {
        current_offset_ = static_cast<uint64_t>(st.st_size);
#ifdef _WIN32
        _lseek(fd_, static_cast<long>(current_offset_), SEEK_SET);
#else
        ::lseek(fd_, static_cast<long>(current_offset_), SEEK_SET);
#endif
    }

    open_ = true;
}

void WAL::close() {
    if (!open_) return;
    if (fd_ >= 0) {
        flush_write_buffer();
        xdl_close(fd_);
        fd_ = -1;
    }
    open_ = false;
}

bool WAL::is_open() const {
    return open_;
}

uint64_t WAL::file_size() const {
    xdl_stat st{};
    if (xdl_fstat(fd_, &st) != 0)
        throw IOError("Cannot stat WAL file: " + path_);
    return static_cast<uint64_t>(st.st_size);
}

// ─────────────────────────────────────────────────────────────────────────────
// Write buffer management
// ─────────────────────────────────────────────────────────────────────────────

static constexpr size_t WAL_BUFFER_SIZE = 256 * 1024; // 256 KB write buffer

void WAL::ensure_buffer_space(size_t needed) {
    if (write_buf_.size() + needed > WAL_BUFFER_SIZE) {
        flush_write_buffer();
    }
}

void WAL::flush_write_buffer() {
    if (write_buf_.empty()) return;
    safe_write(fd_, write_buf_.data(), static_cast<uint32_t>(write_buf_.size()));
    current_offset_ += write_buf_.size();
    write_buf_.clear();
    buf_dirty_ = true;
}

void WAL::fsync_if_dirty() {
    if (buf_dirty_) {
        xdl_fsync(fd_);
        buf_dirty_ = false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Low-level record writer — buffered
// ─────────────────────────────────────────────────────────────────────────────

uint64_t WAL::write_record(WALRecord::Type type,
                            const char* payload, uint32_t payload_len,
                            uint32_t row_id, uint32_t page_id, uint32_t row_slot) {
    if (!open_)
        throw IOError("WAL is not open");

    // Body layout: [record_len:4][type:1][row_id:4][page_id:4][row_slot:4][row_data:N]
    uint32_t payload_before_data = 4 + 1 + 4 + 4 + 4; // len + type + row + page + slot
    uint32_t total_len = payload_before_data + payload_len;

    // Full record: [crc32:4][body:total_len]
    uint32_t record_size = 4 + total_len;

    // Ensure buffer space (flush if needed)
    ensure_buffer_space(record_size);

    // Use tracked offset instead of lseek(SEEK_END) syscall
    uint64_t record_offset = current_offset_ + write_buf_.size();

    // Compute CRC over body (single-pass: write directly into buffer while computing CRC)
    const uint32_t* table = get_crc32_table();
    uint32_t crc = WAL_CHECKSUM_SEED;
    // CRC the total_len bytes
    char len_buf[4];
    std::memcpy(len_buf, &total_len, 4);
    for (int i = 0; i < 4; i++)
        crc = table[(crc ^ static_cast<uint8_t>(len_buf[i])) & 0xFF] ^ (crc >> 8);
    // CRC the type byte
    uint8_t type_byte = static_cast<uint8_t>(type);
    crc = table[(crc ^ type_byte) & 0xFF] ^ (crc >> 8);
    // CRC row_id, page_id, row_slot
    char meta_buf[12];
    std::memcpy(meta_buf + 0, &row_id, 4);
    std::memcpy(meta_buf + 4, &page_id, 4);
    std::memcpy(meta_buf + 8, &row_slot, 4);
    for (int i = 0; i < 12; i++)
        crc = table[(crc ^ static_cast<uint8_t>(meta_buf[i])) & 0xFF] ^ (crc >> 8);
    // CRC the payload
    for (uint32_t i = 0; i < payload_len; i++)
        crc = table[(crc ^ static_cast<uint8_t>(payload[i])) & 0xFF] ^ (crc >> 8);

    // Append CRC to buffer
    size_t old_size = write_buf_.size();
    write_buf_.resize(old_size + record_size);
    char* dst = write_buf_.data() + old_size;

    // Write CRC
    std::memcpy(dst, &crc, 4);
    dst += 4;
    // Write body
    std::memcpy(dst, &total_len, 4); dst += 4;
    *dst++ = static_cast<char>(type_byte);
    std::memcpy(dst, &row_id, 4); dst += 4;
    std::memcpy(dst, &page_id, 4); dst += 4;
    std::memcpy(dst, &row_slot, 4); dst += 4;
    if (payload_len > 0 && payload) {
        std::memcpy(dst, payload, payload_len);
    }

    return record_offset;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public log methods
// ─────────────────────────────────────────────────────────────────────────────

uint64_t WAL::log_insert(uint32_t row_id, uint32_t page_id, uint32_t row_slot,
                         const char* row_data, uint32_t row_data_len) {
    uint64_t offset = write_record(WALRecord::INSERT, row_data, row_data_len,
                                   row_id, page_id, row_slot);
    // Ensure the WAL record is durable before returning to the caller.
    // Without this, a crash between insert and checkpoint loses WAL records.
    flush_write_buffer();
    fsync_if_dirty();
    return offset;
}

uint64_t WAL::log_insert_direct(uint32_t row_id, uint32_t page_id, uint32_t row_slot,
                                const char* row_data, uint32_t row_data_len) {
    // Identical to log_insert but semantically marks the zero-copy path.
    uint64_t offset = write_record(WALRecord::INSERT, row_data, row_data_len,
                                   row_id, page_id, row_slot);
    flush_write_buffer();
    fsync_if_dirty();
    return offset;
}

void WAL::log_commit() {
    write_record(WALRecord::COMMIT, nullptr, 0, 0, 0, 0);
}

void WAL::log_checkpoint() {
    write_record(WALRecord::CHECKPOINT, nullptr, 0, 0, 0, 0);
    // Checkpoint is the one operation that must be durable
    flush_write_buffer();
    fsync_if_dirty();
}

// ─────────────────────────────────────────────────────────────────────────────
// Replay
// ─────────────────────────────────────────────────────────────────────────────

std::vector<WAL::WALRecord> WAL::replay() {
    if (!open_)
        throw IOError("WAL is not open");

    // Flush any buffered data first so replay sees everything
    flush_write_buffer();
    fsync_if_dirty();

    uint64_t fsize = file_size();

    constexpr uint32_t HEADER_SZ = WAL_HEADER_SIZE;
    if (fsize < HEADER_SZ)
        return {};

    // Seek past header
#ifdef _WIN32
    _lseek(fd_, HEADER_SZ, SEEK_SET);
#else
    ::lseek(fd_, HEADER_SZ, SEEK_SET);
#endif

    std::vector<WALRecord> records;

    // Read entire WAL into memory for faster parsing
    uint64_t data_size = fsize - HEADER_SZ;
    std::vector<char> wal_data(data_size);
    uint64_t got = 0;
    while (got < data_size) {
        int n = xdl_read(fd_, wal_data.data() + got, static_cast<unsigned int>(data_size - got));
        if (n <= 0) break;
        got += static_cast<uint64_t>(n);
    }
    if (got < data_size) return {}; // truncated

    uint64_t pos = 0;
    while (pos < got) {
        // Read CRC (4 bytes)
        if (pos + 4 > got) break;
        uint32_t stored_crc;
        std::memcpy(&stored_crc, wal_data.data() + pos, 4);
        pos += 4;

        // Read record_len (4 bytes)
        if (pos + 4 > got) break;
        uint32_t record_len;
        std::memcpy(&record_len, wal_data.data() + pos, 4);
        pos += 4;

        if (record_len < 17 || pos + record_len - 4 > got) break;

        uint32_t remaining = record_len - 4;

        // Compute CRC over body: [record_len:4][rest...]
        const uint32_t* table = get_crc32_table();
        uint32_t crc = WAL_CHECKSUM_SEED;
        // CRC the record_len bytes we already read
        char* crc_ptr = wal_data.data() + pos - 4; // points to record_len
        for (int i = 0; i < 4; i++)
            crc = table[(crc ^ static_cast<uint8_t>(crc_ptr[i])) & 0xFF] ^ (crc >> 8);
        // CRC the rest
        for (uint32_t i = 0; i < remaining; i++)
            crc = table[(crc ^ static_cast<uint8_t>(wal_data.data()[pos + i])) & 0xFF] ^ (crc >> 8);

        if (crc != stored_crc) break;

        // Parse body
        const char* rest = wal_data.data() + pos;
        uint8_t type_byte = static_cast<uint8_t>(rest[0]);

        WALRecord rec;
        rec.type = static_cast<WALRecord::Type>(type_byte);

        if (type_byte <= 3) {
            std::memcpy(&rec.row_id,   rest + 1, 4);
            std::memcpy(&rec.page_id,  rest + 5, 4);
            std::memcpy(&rec.row_slot, rest + 9, 4);

            if (record_len > 17) {
                uint32_t dlen = record_len - 17;
                rec.row_data.assign(rest + 13, rest + 13 + dlen);
            }
        } else {
            rec.row_id = 0;
            rec.page_id = 0;
            rec.row_slot = 0;
        }

        records.push_back(std::move(rec));
        pos += remaining;
    }

    return records;
}

// ─────────────────────────────────────────────────────────────────────────────
// Clear
// ─────────────────────────────────────────────────────────────────────────────

void WAL::clear() {
    if (open_) {
        flush_write_buffer();
        close();
    }
    if (xdl_unlink(path_.c_str()) != 0 && errno != ENOENT) {
        throw IOError("Cannot remove WAL file: " + path_);
    }
    // Re-open so the WAL is ready for reuse.
    open();
}

} // namespace xdl
