/**   Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/util/file.h"

#include <boost/filesystem/operations.hpp>
#include <cstdint>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#endif

#include "mongo/platform/basic.h"
#include "mongo/util/allocator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/text.h"

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
                            &avail,   // bytes available to caller
                            NULL,     // ptr to returned total size
                            NULL)) {  // ptr to returned total free
        return avail.QuadPart;
    }
    DWORD dosError = GetLastError();
    log() << "In File::freeSpace(), GetDiskFreeSpaceEx for '" << path << "' failed with "
          << errnoWithDescription(dosError) << std::endl;
    return -1;
}

void File::fsync() const {
    if (FlushFileBuffers(_handle) == 0) {
        DWORD dosError = GetLastError();
        log() << "In File::fsync(), FlushFileBuffers for '" << _name << "' failed with "
              << errnoWithDescription(dosError) << std::endl;
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
    DWORD dosError = GetLastError();
    log() << "In File::len(), GetFileSizeEx for '" << _name << "' failed with "
          << errnoWithDescription(dosError) << std::endl;
    return 0;
}

void File::open(const char* filename, bool readOnly, bool direct) {
    _name = filename;
    _handle = CreateFileW(toNativeString(filename).c_str(),               // filename
                          (readOnly ? 0 : GENERIC_WRITE) | GENERIC_READ,  // desired access
                          FILE_SHARE_WRITE | FILE_SHARE_READ,             // share mode
                          NULL,                                           // security
                          OPEN_ALWAYS,                                    // create or open
                          FILE_ATTRIBUTE_NORMAL,                          // file attributes
                          NULL);                                          // template
    _bad = !is_open();
    if (_bad) {
        DWORD dosError = GetLastError();
        log() << "In File::open(), CreateFileW for '" << _name << "' failed with "
              << errnoWithDescription(dosError) << std::endl;
    }
}

void File::read(fileofs o, char* data, unsigned len) {
    LARGE_INTEGER li;
    li.QuadPart = o;
    if (SetFilePointerEx(_handle, li, NULL, FILE_BEGIN) == 0) {
        _bad = true;
        DWORD dosError = GetLastError();
        log() << "In File::read(), SetFilePointerEx for '" << _name
              << "' tried to set the file pointer to " << o << " but failed with "
              << errnoWithDescription(dosError) << std::endl;
        return;
    }
    DWORD bytesRead;
    if (!ReadFile(_handle, data, len, &bytesRead, 0)) {
        _bad = true;
        DWORD dosError = GetLastError();
        log() << "In File::read(), ReadFile for '" << _name << "' failed with "
              << errnoWithDescription(dosError) << std::endl;
    } else if (bytesRead != len) {
        _bad = true;
        msgasserted(10438,
                    mongoutils::str::stream() << "In File::read(), ReadFile for '" << _name
                                              << "' read "
                                              << bytesRead
                                              << " bytes while trying to read "
                                              << len
                                              << " bytes starting at offset "
                                              << o
                                              << ", truncated file?");
    }
}

void File::truncate(fileofs size) {
    if (len() <= size) {
        return;
    }
    LARGE_INTEGER li;
    li.QuadPart = size;
    if (SetFilePointerEx(_handle, li, NULL, FILE_BEGIN) == 0) {
        _bad = true;
        DWORD dosError = GetLastError();
        log() << "In File::truncate(), SetFilePointerEx for '" << _name
              << "' tried to set the file pointer to " << size << " but failed with "
              << errnoWithDescription(dosError) << std::endl;
        return;
    }
    if (SetEndOfFile(_handle) == 0) {
        _bad = true;
        DWORD dosError = GetLastError();
        log() << "In File::truncate(), SetEndOfFile for '" << _name << "' failed with "
              << errnoWithDescription(dosError) << std::endl;
    }
}

void File::write(fileofs o, const char* data, unsigned len) {
    LARGE_INTEGER li;
    li.QuadPart = o;
    if (SetFilePointerEx(_handle, li, NULL, FILE_BEGIN) == 0) {
        _bad = true;
        DWORD dosError = GetLastError();
        log() << "In File::write(), SetFilePointerEx for '" << _name
              << "' tried to set the file pointer to " << o << " but failed with "
              << errnoWithDescription(dosError) << std::endl;
        return;
    }
    DWORD bytesWritten;
    if (WriteFile(_handle, data, len, &bytesWritten, NULL) == 0) {
        _bad = true;
        DWORD dosError = GetLastError();
        log() << "In File::write(), WriteFile for '" << _name << "' tried to write " << len
              << " bytes but only wrote " << bytesWritten << " bytes, failing with "
              << errnoWithDescription(dosError) << std::endl;
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
    log() << "In File::freeSpace(), statvfs for '" << path << "' failed with "
          << errnoWithDescription() << std::endl;
    return -1;
}

void File::fsync() const {
    if (::fsync(_fd)) {
        log() << "In File::fsync(), ::fsync for '" << _name << "' failed with "
              << errnoWithDescription() << std::endl;
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
    log() << "In File::len(), lseek for '" << _name << "' failed with " << errnoWithDescription()
          << std::endl;
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
                     |
                     (direct ? O_DIRECT : 0)
#endif
                     ,
                 S_IRUSR | S_IWUSR);
    _bad = !is_open();
    if (_bad) {
        log() << "In File::open(), ::open for '" << _name << "' failed with "
              << errnoWithDescription() << std::endl;
    }
}

void File::read(fileofs o, char* data, unsigned len) {
    ssize_t bytesRead = ::pread(_fd, data, len, o);
    if (bytesRead == -1) {
        _bad = true;
        log() << "In File::read(), ::pread for '" << _name << "' failed with "
              << errnoWithDescription() << std::endl;
    } else if (bytesRead != static_cast<ssize_t>(len)) {
        _bad = true;
        msgasserted(16569,
                    mongoutils::str::stream() << "In File::read(), ::pread for '" << _name
                                              << "' read "
                                              << bytesRead
                                              << " bytes while trying to read "
                                              << len
                                              << " bytes starting at offset "
                                              << o
                                              << ", truncated file?");
    }
}

void File::truncate(fileofs size) {
    if (len() <= size) {
        return;
    }
    if (ftruncate(_fd, size) != 0) {
        _bad = true;
        log() << "In File::truncate(), ftruncate for '" << _name
              << "' tried to set the file pointer to " << size << " but failed with "
              << errnoWithDescription() << std::endl;
        return;
    }
}

void File::write(fileofs o, const char* data, unsigned len) {
    ssize_t bytesWritten = ::pwrite(_fd, data, len, o);
    if (bytesWritten != static_cast<ssize_t>(len)) {
        _bad = true;
        log() << "In File::write(), ::pwrite for '" << _name << "' tried to write " << len
              << " bytes but only wrote " << bytesWritten << " bytes, failing with "
              << errnoWithDescription() << std::endl;
    }
}

#endif  // _WIN32
}
