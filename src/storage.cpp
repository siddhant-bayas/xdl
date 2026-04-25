#include "xdl/storage.h"
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>

namespace xdl {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool file_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

template<typename T>
static void write_pod(std::fstream& f, const T& v) {
    if (!f.write(reinterpret_cast<const char*>(&v), sizeof(T)))
        throw IOError("Write failed");
}

template<typename T>
static void read_pod(std::fstream& f, T& v) {
    if (!f.read(reinterpret_cast<char*>(&v), sizeof(T)))
        throw IOError("Read failed");
}

// ─────────────────────────────────────────────────────────────────────────────
// StorageEngine
// ─────────────────────────────────────────────────────────────────────────────

StorageEngine::StorageEngine(const std::string& path, CompressionType compression)
    : path_(path), compression_(compression) {}

StorageEngine::~StorageEngine() {
    if (open_) close();
}

void StorageEngine::open() {
    if (open_) return;

    bool exists = file_exists(path_);

    if (exists) {
        file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
    } else {
        // Create new file
        file_.open(path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!file_) throw IOError("Cannot create database file: " + path_);

        DBHeader hdr{};
        hdr.magic       = XDL_MAGIC;
        hdr.version     = XDL_VERSION;
        hdr.page_size   = PAGE_SIZE;
        hdr.page_count  = 0;
        hdr.compression = static_cast<uint8_t>(compression_);
        write_db_header(hdr);
    }

    if (!file_) throw IOError("Cannot open database file: " + path_);
    open_ = true;
}

void StorageEngine::close() {
    if (!open_) return;
    file_.flush();
    file_.close();
    open_ = false;
}

bool StorageEngine::is_open() const { return open_; }

DBHeader StorageEngine::read_db_header() {
    file_.seekg(0);
    DBHeader hdr{};
    read_pod(file_, hdr);
    if (hdr.magic != XDL_MAGIC)
        throw CorruptionError("Invalid database magic: " + path_);
    if (hdr.version != XDL_VERSION)
        throw CorruptionError("Unsupported DB version: " + std::to_string(hdr.version));
    return hdr;
}

void StorageEngine::write_db_header(const DBHeader& hdr) {
    file_.seekp(0);
    write_pod(file_, hdr);
    file_.flush();
}

void StorageEngine::read_page_raw(uint64_t offset, PageHeader& header, std::vector<char>& blob) {
    file_.seekg(static_cast<std::streamoff>(offset));
    if (!file_) throw IOError("Seek failed at offset " + std::to_string(offset));

    read_pod(file_, header);

    blob.resize(header.compressed_size);
    if (!file_.read(blob.data(), static_cast<std::streamsize>(header.compressed_size)))
        throw IOError("Failed to read page blob");
}

uint64_t StorageEngine::append_page_raw(const PageHeader& header, const std::vector<char>& blob) {
    file_.seekp(0, std::ios::end);
    uint64_t offset = static_cast<uint64_t>(file_.tellp());

    write_pod(file_, header);
    if (!file_.write(blob.data(), static_cast<std::streamsize>(blob.size())))
        throw IOError("Failed to write page blob");
    file_.flush();
    return offset;
}

void StorageEngine::overwrite_page_raw(
    uint64_t offset, const PageHeader& header, const std::vector<char>& blob)
{
    file_.seekp(static_cast<std::streamoff>(offset));
    if (!file_) throw IOError("Seek failed for overwrite at offset " + std::to_string(offset));

    write_pod(file_, header);
    if (!file_.write(blob.data(), static_cast<std::streamsize>(blob.size())))
        throw IOError("Failed to overwrite page blob");
    file_.flush();
}

uint64_t StorageEngine::file_size() const {
    auto& mf = const_cast<std::fstream&>(file_);
    auto cur = mf.tellg();
    mf.seekg(0, std::ios::end);
    auto sz = static_cast<uint64_t>(mf.tellg());
    mf.seekg(cur);
    return sz;
}

} // namespace xdl