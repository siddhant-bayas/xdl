#include "xdl/storage.h"
#include "xdl/migrate.h"
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <BaseTsd.h>
#include <mutex>
typedef SSIZE_T ssize_t;
#define xdl_open    _open
#define xdl_close   _close
#define xdl_write   _write
#define xdl_read    _read
#define xdl_fstat   _fstat
#define xdl_fsync   _commit
#define xdl_ftruncate _chsize
#define xdl_stat    struct _stat
#define xdl_oflags  O_BINARY

// Windows lacks pread/pwrite.  lseek+read/write is NOT atomic, so we
// serialise around a per-fd offset with a mutex to stay thread-safe.
static std::mutex pread_pwrite_mtx;
static ssize_t xdl_pread_compat(int fd, void* buf, size_t count, uint64_t offset) {
    std::lock_guard<std::mutex> lk(pread_pwrite_mtx);
    _lseek(fd, static_cast<long>(offset), SEEK_SET);
    return _read(fd, buf, static_cast<unsigned int>(count));
}
static ssize_t xdl_pwrite_compat(int fd, const void* buf, size_t count, uint64_t offset) {
    std::lock_guard<std::mutex> lk(pread_pwrite_mtx);
    _lseek(fd, static_cast<long>(offset), SEEK_SET);
    return _write(fd, buf, static_cast<unsigned int>(count));
}
#define xdl_pread   xdl_pread_compat
#define xdl_pwrite  xdl_pwrite_compat
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#define xdl_open    ::open
#define xdl_close   ::close
#define xdl_write   ::write
#define xdl_read    ::read
#define xdl_fstat   ::fstat
#define xdl_fsync   ::fsync
#define xdl_ftruncate ::ftruncate
#define xdl_stat    struct stat
#define xdl_oflags  (0)
#define xdl_pread   ::pread
#define xdl_pwrite  ::pwrite
#endif

namespace xdl {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#ifdef _WIN32
static bool file_exists(const std::string& path) {
    struct _stat st{};
    return ::_stat(path.c_str(), &st) == 0;
}
#else
static bool file_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}
#endif

template<typename T>
static void write_pod(int fd, const T& v) {
    if (xdl_write(fd, reinterpret_cast<const char*>(&v), sizeof(T)) != sizeof(T))
        throw IOError("Write failed");
}

template<typename T>
static void read_pod(int fd, T& v) {
    if (xdl_read(fd, reinterpret_cast<char*>(&v), sizeof(T)) != sizeof(T))
        throw IOError("Read failed");
}

static void safe_pread(int fd, void* buf, size_t count, uint64_t offset) {
    size_t done = 0;
    while (done < count) {
        ssize_t n = xdl_pread(fd, reinterpret_cast<char*>(buf) + done, count - done,
                              static_cast<off_t>(offset + done));
        if (n <= 0) throw IOError("pread failed at offset " + std::to_string(offset + done));
        done += static_cast<size_t>(n);
    }
}

static void safe_pwrite(int fd, const void* buf, size_t count, uint64_t offset) {
    size_t done = 0;
    while (done < count) {
        ssize_t n = xdl_pwrite(fd, reinterpret_cast<const char*>(buf) + done, count - done,
                               static_cast<off_t>(offset + done));
        if (n <= 0) throw IOError("pwrite failed at offset " + std::to_string(offset + done));
        done += static_cast<size_t>(n);
    }
}

// ---------------------------------------------------------------------------
// StorageEngine
// ---------------------------------------------------------------------------

StorageEngine::StorageEngine(const std::string& path, CompressionType compression)
    : path_(path), compression_(compression) {}

StorageEngine::~StorageEngine() {
    if (open_) close();
}

void StorageEngine::open() {
    if (open_) return;

    bool exists = file_exists(path_);

    if (exists) {
        fd_ = xdl_open(path_.c_str(), O_RDWR | xdl_oflags, 0644);
    } else {
        fd_ = xdl_open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC | xdl_oflags, 0644);
        if (fd_ < 0) throw IOError("Cannot create database file: " + path_);

        DBHeader hdr{};
        hdr.magic       = XDL_MAGIC;
        hdr.version     = XDL_VERSION;
        hdr.page_size   = PAGE_SIZE;
        hdr.page_count  = 0;
        hdr.compression = static_cast<uint8_t>(compression_);
        write_db_header(hdr);
    }

    if (fd_ < 0) throw IOError("Cannot open database file: " + path_);
    open_ = true;
}

void StorageEngine::close() {
    if (!open_) return;
    if (fd_ >= 0) {
        xdl_fsync(fd_);
        xdl_close(fd_);
        fd_ = -1;
    }
    open_ = false;
}

bool StorageEngine::is_open() const { return open_; }

DBHeader StorageEngine::read_db_header() {
    DBHeader hdr{};
    safe_pread(fd_, &hdr, sizeof(DBHeader), 0);
    if (hdr.magic != XDL_MAGIC) {
        throw CorruptionError("Invalid database magic: " + path_);
    }
    return hdr;
}

void StorageEngine::write_db_header(const DBHeader& hdr) {
    safe_pwrite(fd_, &hdr, sizeof(DBHeader), 0);
    xdl_fsync(fd_);
}

void StorageEngine::read_page_raw(uint64_t offset, PageHeader& header,
                                  std::vector<char>& blob) {
    safe_pread(fd_, &header, sizeof(PageHeader), offset);

    blob.resize(header.compressed_size);
    safe_pread(fd_, blob.data(), header.compressed_size, offset + sizeof(PageHeader));
}

uint64_t StorageEngine::append_page_raw(const PageHeader& header,
                                         const std::vector<char>& blob) {
    uint64_t offset = file_size();

    // Write header + blob in a single pwritev if possible, or just two pwrites
    safe_pwrite(fd_, &header, sizeof(PageHeader), offset);
    safe_pwrite(fd_, blob.data(), blob.size(), offset + sizeof(PageHeader));
    return offset;
}

void StorageEngine::overwrite_page_raw(uint64_t offset,
                                        const PageHeader& header,
                                        const std::vector<char>& blob) {
    uint64_t needed = offset + sizeof(PageHeader) + blob.size();
    uint64_t cur_sz = file_size();
    if (needed > cur_sz) {
        if (xdl_ftruncate(fd_, static_cast<off_t>(needed)) != 0)
            throw IOError("ftruncate failed");
    }
    safe_pwrite(fd_, &header, sizeof(PageHeader), offset);
    safe_pwrite(fd_, blob.data(), blob.size(), offset + sizeof(PageHeader));
}

uint64_t StorageEngine::file_size() const {
    xdl_stat st{};
    if (xdl_fstat(fd_, &st) != 0)
        throw IOError("Cannot stat database file: " + path_);
    return static_cast<uint64_t>(st.st_size);
}

// ---------------------------------------------------------------------------
// Schema storage
// ---------------------------------------------------------------------------

void StorageEngine::write_schema(const Schema& schema) {
    auto data = serialise_schema(schema);
    uint32_t len = static_cast<uint32_t>(data.size());
    safe_pwrite(fd_, &len, 4, sizeof(DBHeader));
    if (!data.empty()) {
        safe_pwrite(fd_, data.data(), data.size(), sizeof(DBHeader) + 4);
    }
}

Schema StorageEngine::read_schema() {
    uint32_t len = 0;
    safe_pread(fd_, &len, 4, sizeof(DBHeader));
    if (len == 0 || len > 65536) {
        throw CorruptionError("No schema stored in DB file (legacy format)");
    }
    std::vector<char> data(len);
    safe_pread(fd_, data.data(), len, sizeof(DBHeader) + 4);
    return deserialise_schema(data.data(), data.size());
}

uint64_t StorageEngine::data_start_offset() const {
    uint32_t schema_len = 0;
    if (xdl_pread(fd_, &schema_len, 4, sizeof(DBHeader)) != 4)
        return sizeof(DBHeader);
    if (schema_len > 65536) return sizeof(DBHeader);
    return static_cast<uint64_t>(sizeof(DBHeader)) + 4 + schema_len;
}

}  // namespace xdl
