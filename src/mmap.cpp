#include "xdl/mmap.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace xdl {

MmapFile::MmapFile() = default;

MmapFile::~MmapFile() {
    if (is_open()) close();
}

#ifdef _WIN32

void MmapFile::open(const std::string& path, bool create, size_t initial_size) {
    path_ = path;

    if (create) {
        DWORD creation = create ? CREATE_ALWAYS : OPEN_EXISTING;
        HANDLE h = CreateFileA(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            throw IOError("Cannot create mmap file: " + path_);
        fd_ = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_BINARY);
        if (fd_ < 0) {
            CloseHandle(h);
            throw IOError("Cannot create file handle for: " + path_);
        }
        if (initial_size > 0) {
            if (_chsize(fd_, static_cast<long>(initial_size)) != 0)
                throw IOError("Cannot set size for mmap file: " + path_);
            remap(initial_size);
        } else {
            remap(1);
        }
    } else {
        struct _stat st{};
        if (::_stat(path_.c_str(), &st) != 0)
            throw IOError("Cannot stat mmap file: " + path_);
        HANDLE h = CreateFileA(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            throw IOError("Cannot open mmap file: " + path_);
        fd_ = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_BINARY);
        if (fd_ < 0) {
            CloseHandle(h);
            throw IOError("Cannot open mmap file: " + path_);
        }
        if (static_cast<size_t>(st.st_size) > 0) {
            remap(static_cast<size_t>(st.st_size));
        } else {
            size_ = 0;
            addr_ = nullptr;
        }
    }
}

void MmapFile::close() {
    if (!is_open()) return;
    if (addr_ && size_ > 0) {
        UnmapViewOfFile(addr_);
    }
    addr_ = nullptr;
    size_ = 0;
    if (fd_ >= 0) {
        _close(fd_);
        fd_ = -1;
    }
}

void MmapFile::read(uint64_t offset, void* dst, size_t len) const {
    if (offset + len > size_)
        throw IOError("MmapFile read out of range");
    std::memcpy(dst, static_cast<char*>(addr_) + offset, len);
}

void MmapFile::write(uint64_t offset, const void* src, size_t len) {
    if (offset + len > size_) {
        ensure_size(static_cast<size_t>(offset + len));
    }
    std::memcpy(static_cast<char*>(addr_) + offset, src, len);
    FlushViewOfFile(addr_, size_);
}

void MmapFile::ensure_size(size_t min_size) {
    if (min_size <= size_) return;
    size_t new_size = size_;
    if (new_size == 0) new_size = 1;
    while (new_size < min_size) {
        new_size *= 2;
    }
    size_t page_sz = 4096;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    page_sz = si.dwAllocationGranularity;
    new_size = ((new_size + page_sz - 1) / page_sz) * page_sz;
    remap(new_size);
}

void MmapFile::flush() {
    if (addr_ && size_ > 0) {
        FlushViewOfFile(addr_, size_);
    }
}

void MmapFile::remap(size_t new_size) {
    if (addr_ && size_ > 0) {
        UnmapViewOfFile(addr_);
        addr_ = nullptr;
        size_ = 0;
    }
    if (_chsize(fd_, static_cast<long>(new_size)) != 0)
        throw IOError("Cannot grow mmap file: " + path_);
    if (new_size == 0) {
        addr_ = nullptr;
        size_ = 0;
        return;
    }
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd_));
    HANDLE mapping = CreateFileMappingA(h, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (!mapping)
        throw IOError("CreateFileMapping failed for: " + path_);
    void* p = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, new_size);
    CloseHandle(mapping);
    if (!p)
        throw IOError("MapViewOfFile failed for: " + path_);
    addr_ = p;
    size_ = new_size;
}

#else // POSIX

void MmapFile::open(const std::string& path, bool create, size_t initial_size) {
    path_ = path;

    if (create) {
        int flags = O_RDWR | O_CREAT | O_TRUNC;
        fd_ = ::open(path_.c_str(), flags, 0644);
        if (fd_ < 0)
            throw IOError("Cannot create mmap file: " + path_);
        if (initial_size > 0) {
            if (::ftruncate(fd_, static_cast<off_t>(initial_size)) != 0)
                throw IOError("Cannot set size for mmap file: " + path_);
            remap(initial_size);
        } else {
            remap(1); // minimum 1 byte mapping
        }
    } else {
        struct stat st{};
        if (::stat(path_.c_str(), &st) != 0)
            throw IOError("Cannot stat mmap file: " + path_);
        fd_ = ::open(path_.c_str(), O_RDWR);
        if (fd_ < 0)
            throw IOError("Cannot open mmap file: " + path_);
        if (st.st_size > 0) {
            remap(static_cast<size_t>(st.st_size));
        } else {
            size_ = 0;
            addr_ = nullptr;
        }
    }
}

void MmapFile::close() {
    if (!is_open()) return;
    if (addr_ && size_ > 0) {
        ::munmap(addr_, size_);
    }
    addr_ = nullptr;
    size_ = 0;
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void MmapFile::read(uint64_t offset, void* dst, size_t len) const {
    if (offset + len > size_)
        throw IOError("MmapFile read out of range");
    std::memcpy(dst, static_cast<char*>(addr_) + offset, len);
}

void MmapFile::write(uint64_t offset, const void* src, size_t len) {
    if (offset + len > size_) {
        ensure_size(static_cast<size_t>(offset + len));
    }
    std::memcpy(static_cast<char*>(addr_) + offset, src, len);
    uintptr_t page_size = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    uintptr_t start = (reinterpret_cast<uintptr_t>(static_cast<char*>(addr_) + offset) & ~(page_size - 1));
    uintptr_t end = reinterpret_cast<uintptr_t>(static_cast<char*>(addr_) + offset + len);
    size_t sync_len = static_cast<size_t>(end - start);
    ::msync(reinterpret_cast<void*>(start), sync_len, MS_SYNC);
}

void MmapFile::ensure_size(size_t min_size) {
    if (min_size <= size_) return;
    size_t new_size = size_;
    if (new_size == 0) new_size = 1;
    while (new_size < min_size) {
        new_size *= 2;
    }
    size_t page_sz = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    new_size = ((new_size + page_sz - 1) / page_sz) * page_sz;
    remap(new_size);
}

void MmapFile::flush() {
    if (addr_ && size_ > 0) {
        ::msync(addr_, size_, MS_SYNC);
    }
}

void MmapFile::remap(size_t new_size) {
    if (addr_ && size_ > 0) {
        ::munmap(addr_, size_);
        addr_ = nullptr;
        size_ = 0;
    }
    if (::ftruncate(fd_, static_cast<off_t>(new_size)) != 0)
        throw IOError("Cannot grow mmap file: " + path_);
    if (new_size == 0) {
        addr_ = nullptr;
        size_ = 0;
        return;
    }
    void* p = ::mmap(nullptr, new_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED)
        throw IOError("mmap failed for: " + path_);
    addr_ = p;
    size_ = new_size;
}

#endif

} // namespace xdl
