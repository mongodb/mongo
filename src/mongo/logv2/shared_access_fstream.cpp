/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/logv2/shared_access_fstream.h"

#if defined(_WIN32) && defined(_MSC_VER)

#include <fcntl.h>
#include <filesystem>
#include <io.h>

#include "mongo/util/text.h"

namespace mongo {
FILE* Win32SharedAccessFileDescriptor::_open(const wchar_t* filename,
                                             std::ios_base::openmode mode,
                                             bool sharedWriteAccess) {
    const char* fdmode = nullptr;
    int openflags = 0;
    uint32_t desiredAccess = 0;
    uint32_t creationDisposition = 0;
    uint32_t shareMode = FILE_SHARE_DELETE | FILE_SHARE_READ;
    if (sharedWriteAccess)
        shareMode |= FILE_SHARE_WRITE;

    // The combinations are taken from https://en.cppreference.com/w/cpp/io/basic_filebuf/open
    auto modeWithoutAte = mode & ~std::ios_base::ate;
    if (modeWithoutAte == std::ios_base::in) {
        fdmode = "r";
        creationDisposition = OPEN_EXISTING;
        openflags |= _O_RDONLY;
    } else if (modeWithoutAte == std::ios_base::out ||
               modeWithoutAte == (std::ios_base::out | std::ios_base::trunc)) {
        fdmode = "w";
        creationDisposition = CREATE_ALWAYS;
        openflags |= _O_WRONLY;
    } else if (modeWithoutAte == std::ios_base::app ||
               modeWithoutAte == (std::ios_base::out | std::ios_base::app)) {

        fdmode = "a";
        creationDisposition = OPEN_ALWAYS;
        openflags |= _O_WRONLY | _O_APPEND;
    } else if (modeWithoutAte == (std::ios_base::out | std::ios_base::in)) {

        fdmode = "r+";
        creationDisposition = OPEN_EXISTING;
        openflags |= _O_RDWR;
    } else if (modeWithoutAte == (std::ios_base::out | std::ios_base::in | std::ios_base::trunc)) {

        fdmode = "w+";
        creationDisposition = CREATE_ALWAYS;
        openflags |= _O_RDWR;
    } else if (modeWithoutAte == (std::ios_base::out | std::ios_base::in | std::ios_base::app) ||
               modeWithoutAte == (std::ios_base::in | std::ios_base::app)) {
        fdmode = "a+";
        creationDisposition = OPEN_ALWAYS;
        openflags |= _O_RDWR;
    } else if (modeWithoutAte == (std::ios_base::binary | std::ios_base::in)) {

        fdmode = "rb";
        creationDisposition = OPEN_EXISTING;
        openflags |= _O_RDONLY;
    } else if (modeWithoutAte == (std::ios_base::binary | std::ios_base::out) ||
               modeWithoutAte ==
                   (std::ios_base::binary | std::ios_base::out | std::ios_base::trunc)) {

        fdmode = "wb";
        creationDisposition = CREATE_ALWAYS;
        openflags |= _O_WRONLY;
    } else if (modeWithoutAte == (std::ios_base::binary | std::ios_base::app) ||
               modeWithoutAte ==
                   (std::ios_base::binary | std::ios_base::out | std::ios_base::app)) {

        fdmode = "ab";
        creationDisposition = OPEN_ALWAYS;
        openflags |= _O_WRONLY;
    } else if (modeWithoutAte == (std::ios_base::binary | std::ios_base::out | std::ios_base::in)) {

        fdmode = "r+b";
        creationDisposition = OPEN_EXISTING;
        openflags |= _O_RDWR;
    } else if (modeWithoutAte ==
               (std::ios_base::binary | std::ios_base::out | std::ios_base::in |
                std::ios_base::trunc)) {

        fdmode = "w+b";
        creationDisposition = CREATE_ALWAYS;
        openflags |= _O_RDWR;
    } else if (modeWithoutAte ==
                   (std::ios_base::binary | std::ios_base::out | std::ios_base::in |
                    std::ios_base::app) ||
               modeWithoutAte == (std::ios_base::binary | std::ios_base::in | std::ios_base::app)) {

        fdmode = "a+b";
        creationDisposition = OPEN_ALWAYS;
        openflags |= _O_RDWR;
    } else
        return nullptr;

    if (mode & std::ios_base::in)
        desiredAccess |= GENERIC_READ;
    if (mode & std::ios_base::out || mode & std::ios_base::app)
        desiredAccess |= GENERIC_WRITE;

    if (mode & std::ios_base::binary)
        openflags |= _O_BINARY;
    else
        openflags |= _O_U8TEXT;

    if (mode & std::ios_base::trunc)
        openflags |= _O_TRUNC;

    // Open file with share access to get a Windows HANDLE
    HANDLE handle = CreateFileW(filename,               // lpFileName
                                desiredAccess,          // dwDesiredAccess
                                shareMode,              // dwShareMode
                                nullptr,                // lpSecurityAttributes
                                creationDisposition,    // dwCreationDisposition
                                FILE_ATTRIBUTE_NORMAL,  // dwFlagsAndAttributes
                                nullptr                 // hTemplateFile
    );
    if (handle == INVALID_HANDLE_VALUE) {
        return nullptr;
    }

    LARGE_INTEGER zero;
    zero.QuadPart = 0LL;
    if (mode & std::ios_base::ate ||
        ((mode & std::ios_base::app) && (openflags & _O_APPEND) == 0)) {
        SetFilePointerEx(handle, zero, nullptr, FILE_END);
    }

    if (mode & std::ios_base::trunc) {
        SetEndOfFile(handle);
    }

    // Convert the HANDLE to a file descriptor
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), openflags);
    if (fd == -1) {
        CloseHandle(handle);
        return nullptr;
    }

    // Open the file descriptor to get a FILE*
    return _fdopen(fd, fdmode);
}

FILE* Win32SharedAccessFileDescriptor::_open(StringData filename,
                                             std::ios_base::openmode mode,
                                             bool sharedWriteAccess) {
    return _open(toWideStringFromStringData(filename).c_str(), mode, sharedWriteAccess);
}

Win32SharedAccessFileDescriptor::Win32SharedAccessFileDescriptor(const wchar_t* filename,
                                                                 std::ios_base::openmode mode,
                                                                 bool sharedWriteAccess)
    : _file(_open(filename, mode, sharedWriteAccess)) {}
Win32SharedAccessFileDescriptor::Win32SharedAccessFileDescriptor(const char* filename,
                                                                 std::ios_base::openmode mode,
                                                                 bool sharedWriteAccess)
    : _file(_open(filename, mode, sharedWriteAccess)) {}
Win32SharedAccessFileDescriptor::Win32SharedAccessFileDescriptor(const std::string& filename,
                                                                 std::ios_base::openmode mode,
                                                                 bool sharedWriteAccess)
    : _file(_open(filename.c_str(), mode, sharedWriteAccess)) {}
Win32SharedAccessFileDescriptor::Win32SharedAccessFileDescriptor(
    const std::filesystem::path& filename, std::ios_base::openmode mode, bool sharedWriteAccess)
    : _file(_open(filename.c_str(), mode, sharedWriteAccess)) {}

Win32SharedAccessOfstream::Win32SharedAccessOfstream(const char* filename,
                                                     std::ios_base::openmode mode,
                                                     bool sharedWriteAccess)
    : Win32SharedAccessFileDescriptor(filename, mode, sharedWriteAccess), std::ofstream(_file) {
    if (!_file)
        setstate(failbit);
}

Win32SharedAccessOfstream::Win32SharedAccessOfstream(const wchar_t* filename,
                                                     std::ios_base::openmode mode,
                                                     bool sharedWriteAccess)
    : Win32SharedAccessFileDescriptor(filename, mode, sharedWriteAccess), std::ofstream(_file) {
    if (!_file)
        setstate(failbit);
}

Win32SharedAccessOfstream::Win32SharedAccessOfstream(const std::string& filename,
                                                     std::ios_base::openmode mode,
                                                     bool sharedWriteAccess)
    : Win32SharedAccessFileDescriptor(filename, mode, sharedWriteAccess), std::ofstream(_file) {
    if (!_file)
        setstate(failbit);
}

Win32SharedAccessOfstream::Win32SharedAccessOfstream(const std::filesystem::path& filename,
                                                     std::ios_base::openmode mode,
                                                     bool sharedWriteAccess)
    : Win32SharedAccessFileDescriptor(filename, mode, sharedWriteAccess), std::ofstream(_file) {
    if (!_file)
        setstate(failbit);
}

Win32SharedAccessIfstream::Win32SharedAccessIfstream(const char* filename,
                                                     std::ios_base::openmode mode,
                                                     bool sharedWriteAccess)
    : Win32SharedAccessFileDescriptor(filename, mode, sharedWriteAccess), std::ifstream(_file) {
    if (!_file)
        setstate(failbit);
}

Win32SharedAccessIfstream::Win32SharedAccessIfstream(const wchar_t* filename,
                                                     std::ios_base::openmode mode,
                                                     bool sharedWriteAccess)
    : Win32SharedAccessFileDescriptor(filename, mode, sharedWriteAccess), std::ifstream(_file) {
    if (!_file)
        setstate(failbit);
}

Win32SharedAccessIfstream::Win32SharedAccessIfstream(const std::string& filename,
                                                     std::ios_base::openmode mode,
                                                     bool sharedWriteAccess)
    : Win32SharedAccessFileDescriptor(filename, mode, sharedWriteAccess), std::ifstream(_file) {
    if (!_file)
        setstate(failbit);
}

Win32SharedAccessIfstream::Win32SharedAccessIfstream(const std::filesystem::path& filename,
                                                     std::ios_base::openmode mode,
                                                     bool sharedWriteAccess)
    : Win32SharedAccessFileDescriptor(filename, mode, sharedWriteAccess), std::ifstream(_file) {
    if (!_file)
        setstate(failbit);
}

Win32SharedAccessFstream::Win32SharedAccessFstream(const char* filename,
                                                   std::ios_base::openmode mode,
                                                   bool sharedWriteAccess)
    : Win32SharedAccessFileDescriptor(filename, mode, sharedWriteAccess), std::fstream(_file) {
    if (!_file)
        setstate(failbit);
}

Win32SharedAccessFstream::Win32SharedAccessFstream(const wchar_t* filename,
                                                   std::ios_base::openmode mode,
                                                   bool sharedWriteAccess)
    : Win32SharedAccessFileDescriptor(filename, mode, sharedWriteAccess), std::fstream(_file) {
    if (!_file)
        setstate(failbit);
}

Win32SharedAccessFstream::Win32SharedAccessFstream(const std::string& filename,
                                                   std::ios_base::openmode mode,
                                                   bool sharedWriteAccess)
    : Win32SharedAccessFileDescriptor(filename, mode, sharedWriteAccess), std::fstream(_file) {
    if (!_file)
        setstate(failbit);
}

Win32SharedAccessFstream::Win32SharedAccessFstream(const std::filesystem::path& filename,
                                                   std::ios_base::openmode mode,
                                                   bool sharedWriteAccess)
    : Win32SharedAccessFileDescriptor(filename, mode, sharedWriteAccess), std::fstream(_file) {
    if (!_file)
        setstate(failbit);
}

}  // namespace mongo

#endif  // _WIN32 && _MSC_VER
