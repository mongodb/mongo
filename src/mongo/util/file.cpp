// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/file.h"

#include <cstdint>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>

#include <sys/stat.h>
#include <sys/statvfs.h>
#endif

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

#if defined(_WIN32)

File::File() : _bad(true), _handle(INVALID_HANDLE_VALUE) {}

File::~File() {
    if (is_open()) {
        CloseHandle(_handle);
    }
    _handle = INVALID_HANDLE_VALUE;
}

intmax_t File::freeSpace(const std::string& path) {
    ULARGE_INTEGER avail;
    if (GetDiskFreeSpaceExW(toWideString(path.c_str()).c_str(),
                            &avail,      // bytes available to caller
                            nullptr,     // ptr to returned total size
                            nullptr)) {  // ptr to returned total free
        return avail.QuadPart;
    }
    auto ec = lastSystemError();
    LOGV2(23140,
          "In File::freeSpace(), GetDiskFreeSpaceEx failed",
          "path"_attr = path,
          "error"_attr = errorMessage(ec));
    return -1;
}

void File::fsync() const {
    if (FlushFileBuffers(_handle) == 0) {
        auto ec = lastSystemError();
        LOGV2(23141,
              "In File::fsync(), FlushFileBuffers failed",
              "fileName"_attr = _name,
              "error"_attr = errorMessage(ec));
    }
}

bool File::is_open() const {
    return _handle != INVALID_HANDLE_VALUE;
}

fileofs File::len() {
    LARGE_INTEGER li;
    if (GetFileSizeEx(_handle, &li)) {
        return li.QuadPart;
    }
    _bad = true;
    auto ec = lastSystemError();
    LOGV2(23142,
          "In File::len(), GetFileSizeEx failed",
          "fileName"_attr = _name,
          "error"_attr = errorMessage(ec));
    return 0;
}

void File::open(const char* filename, bool readOnly, bool direct) {
    _name = filename;
    _handle = CreateFileW(toNativeString(filename).c_str(),               // filename
                          (readOnly ? 0 : GENERIC_WRITE) | GENERIC_READ,  // desired access
                          FILE_SHARE_WRITE | FILE_SHARE_READ,             // share mode
                          nullptr,                                        // security
                          OPEN_ALWAYS,                                    // create or open
                          FILE_ATTRIBUTE_NORMAL,                          // file attributes
                          nullptr);                                       // template
    _bad = !is_open();
    if (_bad) {
        auto ec = lastSystemError();
        LOGV2(23143,
              "In File::open(), CreateFileW failed",
              "fileName"_attr = _name,
              "error"_attr = errorMessage(ec));
    }
}

void File::read(fileofs o, char* data, unsigned len) {
    LARGE_INTEGER li;
    li.QuadPart = o;
    if (SetFilePointerEx(_handle, li, nullptr, FILE_BEGIN) == 0) {
        _bad = true;
        auto ec = lastSystemError();
        LOGV2(23144,
              "In File::read(), SetFilePointerEx failed to set file pointer",
              "fileName"_attr = _name,
              "failPointer"_attr = o,
              "error"_attr = errorMessage(ec));
        return;
    }
    DWORD bytesRead;
    if (!ReadFile(_handle, data, len, &bytesRead, 0)) {
        _bad = true;
        auto ec = lastSystemError();
        LOGV2(23145,
              "In File::read(), ReadFile failed",
              "fileName"_attr = _name,
              "error"_attr = errorMessage(ec));
    } else if (bytesRead != len) {
        _bad = true;
        masserted(10438,
                  str::stream() << "In File::read(), ReadFile for '" << _name << "' read "
                                << bytesRead << " bytes while trying to read " << len
                                << " bytes starting at offset " << o << ", truncated file?");
    }
}

void File::truncate(fileofs size) {
    if (len() <= size) {
        return;
    }
    LARGE_INTEGER li;
    li.QuadPart = size;
    if (SetFilePointerEx(_handle, li, nullptr, FILE_BEGIN) == 0) {
        _bad = true;
        auto ec = lastSystemError();
        LOGV2(23146,
              "In File::truncate(), SetFilePointerEx failed to set file pointer",
              "fileName"_attr = _name,
              "filePointer"_attr = size,
              "error"_attr = errorMessage(ec));
        return;
    }
    if (SetEndOfFile(_handle) == 0) {
        _bad = true;
        auto ec = lastSystemError();
        LOGV2(23147,
              "In File::truncate(), SetEndOfFile failed",
              "fileName"_attr = _name,
              "error"_attr = errorMessage(ec));
    }
}

void File::write(fileofs o, const char* data, unsigned len) {
    LARGE_INTEGER li;
    li.QuadPart = o;
    if (SetFilePointerEx(_handle, li, nullptr, FILE_BEGIN) == 0) {
        _bad = true;
        auto ec = lastSystemError();
        LOGV2(23148,
              "In File::write(), SetFilePointerEx failed to set file pointer",
              "fileName"_attr = _name,
              "filePointer"_attr = o,
              "error"_attr = errorMessage(ec));
        return;
    }
    DWORD bytesWritten;
    if (WriteFile(_handle, data, len, &bytesWritten, nullptr) == 0) {
        _bad = true;
        auto ec = lastSystemError();
        LOGV2(23149,
              "In File::write(), WriteFile failed",
              "fileName"_attr = _name,
              "bytesToWrite"_attr = len,
              "bytesWritten"_attr = bytesWritten,
              "error"_attr = errorMessage(ec));
    }
}

#else  // _WIN32

File::File() : _bad(true), _fd(-1) {}

File::~File() {
    if (is_open()) {
        ::close(_fd);
    }
    _fd = -1;
}

intmax_t File::freeSpace(const std::string& path) {
    struct statvfs info;
    if (statvfs(path.c_str(), &info) == 0) {
        return static_cast<intmax_t>(info.f_bavail) * info.f_frsize;
    }
    auto ec = lastSystemError();
    LOGV2(23150,
          "In File::freeSpace(), statvfs failed",
          "path"_attr = path,
          "error"_attr = errorMessage(ec));
    return -1;
}

void File::fsync() const {
    if (::fsync(_fd)) {
        auto ec = lastSystemError();
        LOGV2(23151,
              "In File::fsync(), ::fsync failed",
              "fileName"_attr = _name,
              "error"_attr = errorMessage(ec));
    }
}

bool File::is_open() const {
    return _fd > 0;
}

fileofs File::len() {
    off_t o = lseek(_fd, 0, SEEK_END);
    if (o != static_cast<off_t>(-1)) {
        return o;
    }
    _bad = true;
    auto ec = lastSystemError();
    LOGV2(23152,
          "In File::len(), lseek failed",
          "fileName"_attr = _name,
          "error"_attr = errorMessage(ec));
    return 0;
}

#ifndef O_NOATIME
#define O_NOATIME 0
#endif

void File::open(const char* filename, bool readOnly, bool direct) {
    _name = filename;
    _fd = ::open(filename,
                 (readOnly ? O_RDONLY : (O_CREAT | O_RDWR | O_NOATIME))
#if defined(O_DIRECT)
                     | (direct ? O_DIRECT : 0)
#endif
                     ,
                 S_IRUSR | S_IWUSR);
    _bad = !is_open();
    if (_bad) {
        auto ec = lastSystemError();
        LOGV2(23153,
              "In File::open(), ::open failed",
              "fileName"_attr = _name,
              "error"_attr = errorMessage(ec));
    }
}

void File::read(fileofs o, char* data, unsigned len) {
    ssize_t bytesRead = ::pread(_fd, data, len, o);
    if (bytesRead == -1) {
        auto ec = lastSystemError();
        _bad = true;
        LOGV2(23154,
              "In File::read(), ::pread failed",
              "fileName"_attr = _name,
              "error"_attr = errorMessage(ec));
    } else if (bytesRead != static_cast<ssize_t>(len)) {
        _bad = true;
        masserted(16569,
                  str::stream() << "In File::read(), ::pread for '" << _name << "' read "
                                << bytesRead << " bytes while trying to read " << len
                                << " bytes starting at offset " << o << ", truncated file?");
    }
}

void File::truncate(fileofs size) {
    if (len() <= size) {
        return;
    }
    if (ftruncate(_fd, size) != 0) {
        auto ec = lastSystemError();
        _bad = true;
        LOGV2(23155,
              "In File::truncate(), ftruncate failed to set file pointer",
              "fileName"_attr = _name,
              "filePointer"_attr = size,
              "error"_attr = errorMessage(ec));
        return;
    }
}

void File::write(fileofs o, const char* data, unsigned len) {
    ssize_t bytesWritten = ::pwrite(_fd, data, len, o);
    if (bytesWritten != static_cast<ssize_t>(len)) {
        std::error_code ec;
        if (bytesWritten == -1)
            ec = lastSystemError();
        _bad = true;
        LOGV2(23156,
              "In File::write(), ::pwrite failed",
              "fileName"_attr = _name,
              "bytesToWrite"_attr = len,
              "bytesWritten"_attr = bytesWritten,
              "error"_attr = errorMessage(ec));
    }
}

#endif  // _WIN32
}  // namespace mongo
