// @file logfile.cpp simple file log writing / journaling

/**
*    Copyright (C) 2008 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/logfile.h"

#include "mongo/db/storage/mmap_v1/mmap.h"
#include "mongo/db/storage/paths.h"
#include "mongo/platform/posix_fadvise.h"
#include "mongo/util/allocator.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/text.h"


using namespace mongoutils;

using std::endl;
using std::string;

#if defined(_WIN32)

namespace mongo {

LogFile::LogFile(const std::string& name, bool readwrite) : _name(name) {
    _fd = CreateFile(toNativeString(name.c_str()).c_str(),
                     (readwrite ? GENERIC_READ : 0) | GENERIC_WRITE,
                     FILE_SHARE_READ,
                     NULL,
                     OPEN_ALWAYS,
                     FILE_FLAG_NO_BUFFERING,
                     NULL);
    if (_fd == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        uasserted(13518,
                  str::stream() << "couldn't open file " << name << " for writing "
                                << errnoWithDescription(e));
    }
    SetFilePointer(_fd, 0, 0, FILE_BEGIN);
}

LogFile::~LogFile() {
    if (_fd != INVALID_HANDLE_VALUE)
        CloseHandle(_fd);
}

void LogFile::truncate() {
    verify(_fd != INVALID_HANDLE_VALUE);

    if (!SetEndOfFile(_fd)) {
        msgasserted(15871, "Couldn't truncate file: " + errnoWithDescription());
    }
}

void LogFile::writeAt(unsigned long long offset, const void* _buf, size_t _len) {
    // TODO 64 bit offsets
    OVERLAPPED o;
    memset(&o, 0, sizeof(o));
    (unsigned long long&)o.Offset = offset;
    BOOL ok = WriteFile(_fd, _buf, _len, 0, &o);
    verify(ok);
}

void LogFile::readAt(unsigned long long offset, void* _buf, size_t _len) {
    // TODO 64 bit offsets
    OVERLAPPED o;
    memset(&o, 0, sizeof(o));
    (unsigned long long&)o.Offset = offset;
    DWORD nr;
    BOOL ok = ReadFile(_fd, _buf, _len, &nr, &o);
    if (!ok) {
        string e = errnoWithDescription();
        // DWORD e = GetLastError();
        log() << "LogFile readAt(" << offset << ") len:" << _len << "errno:" << e << endl;
        verify(false);
    }
}

void LogFile::synchronousAppend(const void* _buf, size_t _len) {
    const size_t BlockSize = 8 * 1024 * 1024;
    verify(_fd);
    verify(_len % minDirectIOSizeBytes == 0);
    const char* buf = (const char*)_buf;
    size_t left = _len;
    while (left) {
        size_t toWrite = std::min(left, BlockSize);
        DWORD written;
        if (!WriteFile(_fd, buf, toWrite, &written, NULL)) {
            DWORD e = GetLastError();
            if (e == 87)
                msgasserted(13519, "error 87 appending to file - invalid parameter");
            else
                uasserted(13517,
                          str::stream() << "error appending to file " << _name << ' ' << _len << ' '
                                        << toWrite
                                        << ' '
                                        << errnoWithDescription(e));
        } else {
            dassert(written == toWrite);
        }
        left -= written;
        buf += written;
    }
}
}

#else

/// posix

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __linux__
#include <linux/fs.h>
#endif

namespace mongo {

LogFile::LogFile(const std::string& name, bool readwrite) : _name(name) {
    int options = O_CREAT | (readwrite ? O_RDWR : O_WRONLY)
#if defined(O_DIRECT)
        | O_DIRECT
#endif
#if defined(O_NOATIME)
        | O_NOATIME
#endif
        ;

    _fd = open(name.c_str(), options, S_IRUSR | S_IWUSR);
    _blkSize = minDirectIOSizeBytes;

#if defined(O_DIRECT)
    _direct = true;
    if (_fd < 0) {
        _direct = false;
        options &= ~O_DIRECT;
        _fd = open(name.c_str(), options, S_IRUSR | S_IWUSR);
    }
#ifdef __linux__
    ssize_t tmpBlkSize = ioctl(_fd, BLKBSZGET);
    // TODO: We need some sanity checking on tmpBlkSize even if ioctl() did not fail.
    if (tmpBlkSize > 0) {
        _blkSize = (size_t)tmpBlkSize;
    }
#endif
#else
    _direct = false;
#endif

    if (_fd < 0) {
        uasserted(13516,
                  str::stream() << "couldn't open file " << name << " for writing "
                                << errnoWithDescription());
    }

    flushMyDirectory(name);
}

LogFile::~LogFile() {
    if (_fd >= 0)
        close(_fd);
    _fd = -1;
}

void LogFile::truncate() {
    verify(_fd >= 0);

    static_assert(sizeof(off_t) == 8, "sizeof(off_t) == 8");  // we don't want overflow here
    const off_t pos = lseek(_fd, 0, SEEK_CUR);                // doesn't actually seek
    if (ftruncate(_fd, pos) != 0) {
        msgasserted(15873, "Couldn't truncate file: " + errnoWithDescription());
    }

    fsync(_fd);
}

void LogFile::writeAt(unsigned long long offset, const void* buf, size_t len) {
    verify(((size_t)buf) % minDirectIOSizeBytes == 0);  // aligned
    ssize_t written = pwrite(_fd, buf, len, offset);
    if (written != (ssize_t)len) {
        log() << "writeAt fails " << errnoWithDescription() << endl;
    }
#if defined(__linux__)
    fdatasync(_fd);
#else
    fsync(_fd);
#endif
}

void LogFile::readAt(unsigned long long offset, void* _buf, size_t _len) {
    verify(((size_t)_buf) % minDirectIOSizeBytes == 0);  // aligned
    ssize_t rd = pread(_fd, _buf, _len, offset);
    verify(rd != -1);
}

void LogFile::synchronousAppend(const void* b, size_t len) {
    const char* buf = static_cast<const char*>(b);
    ssize_t charsToWrite = static_cast<ssize_t>(len);

    fassert(16144, charsToWrite >= 0);
    fassert(16142, _fd >= 0);
    fassert(16143, reinterpret_cast<size_t>(buf) % _blkSize == 0);  // aligned

#ifdef POSIX_FADV_DONTNEED
    const off_t pos = lseek(_fd, 0, SEEK_CUR);  // doesn't actually seek, just get current position
#endif

    while (charsToWrite > 0) {
        const ssize_t written = write(_fd, buf, static_cast<size_t>(charsToWrite));
        if (-1 == written) {
            log() << "LogFile::synchronousAppend failed with " << charsToWrite
                  << " bytes unwritten out of " << len << " bytes;  b=" << b << ' '
                  << errnoWithDescription() << std::endl;
            fassertFailed(13515);
        }
        buf += written;
        charsToWrite -= written;
    }

    if (
#if defined(__linux__)
        fdatasync(_fd) < 0
#else
        fsync(_fd)
#endif
        ) {
        log() << "error appending to file on fsync " << ' ' << errnoWithDescription();
        fassertFailed(13514);
    }

#ifdef POSIX_FADV_DONTNEED
    if (!_direct && pos >= 0)  // current position cannot be negative
        posix_fadvise(_fd, pos, len, POSIX_FADV_DONTNEED);
#endif
}
}

#endif
