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

#pragma once

#if defined(_WIN32) && defined(_MSC_VER)

#include <fstream>

#include "mongo/base/string_data.h"

namespace mongo {
// Helper class to open a FILE* with shared access. Used by the stream classes below.
class Win32SharedAccessFileDescriptor {
protected:
    Win32SharedAccessFileDescriptor(const wchar_t* filename,
                                    std::ios_base::openmode mode,
                                    bool sharedWriteAccess);
    Win32SharedAccessFileDescriptor(const char* filename,
                                    std::ios_base::openmode mode,
                                    bool sharedWriteAccess);
    Win32SharedAccessFileDescriptor(const std::string& filename,
                                    std::ios_base::openmode mode,
                                    bool sharedWriteAccess);
    Win32SharedAccessFileDescriptor(const std::filesystem::path& filename,
                                    std::ios_base::openmode mode,
                                    bool sharedWriteAccess);

    static FILE* _open(const wchar_t* filename,
                       std::ios_base::openmode mode,
                       bool sharedWriteAccess);
    static FILE* _open(StringData filename, std::ios_base::openmode mode, bool sharedWriteAccess);

    FILE* _file = nullptr;
};

// File stream classes that extends the regular file streams with two capabilities:
// (1) Opens files with shared delete and read access with optional shared write access.
// (2) Uses UTF-8 as the encoding for the filename parameter
class Win32SharedAccessOfstream : private Win32SharedAccessFileDescriptor, public std::ofstream {
public:
    explicit Win32SharedAccessOfstream(const char* filename,
                                       ios_base::openmode mode = ios_base::out,
                                       bool sharedWriteAccess = false);
    explicit Win32SharedAccessOfstream(const wchar_t* filename,
                                       ios_base::openmode mode = ios_base::out,
                                       bool sharedWriteAccess = false);
    explicit Win32SharedAccessOfstream(const std::string& filename,
                                       ios_base::openmode mode = ios_base::out,
                                       bool sharedWriteAccess = false);
    explicit Win32SharedAccessOfstream(const std::filesystem::path& filename,
                                       ios_base::openmode mode = ios_base::out,
                                       bool sharedWriteAccess = false);

    // The Visual Studio extension that operates on FILE* does not have an open overload. Needs to
    // open with constructor.
    void open(const char* filename, ios_base::openmode mode) = delete;
    void open(const wchar_t* filename, ios_base::openmode mode) = delete;
    void open(const std::string& filename, ios_base::openmode mode) = delete;
    void open(const std::filesystem::path& filename, ios_base::openmode mode) = delete;
};

class Win32SharedAccessIfstream : private Win32SharedAccessFileDescriptor, public std::ifstream {
public:
    explicit Win32SharedAccessIfstream(const char* filename,
                                       ios_base::openmode mode = ios_base::in,
                                       bool sharedWriteAccess = false);
    explicit Win32SharedAccessIfstream(const wchar_t* filename,
                                       ios_base::openmode mode = ios_base::in,
                                       bool sharedWriteAccess = false);
    explicit Win32SharedAccessIfstream(const std::string& filename,
                                       ios_base::openmode mode = ios_base::in,
                                       bool sharedWriteAccess = false);
    explicit Win32SharedAccessIfstream(const std::filesystem::path& filename,
                                       ios_base::openmode mode = ios_base::in,
                                       bool sharedWriteAccess = false);

    // The Visual Studio extension that operates on FILE* does not have an open overload. Needs to
    // open with constructor.
    void open(const char* filename, ios_base::openmode mode) = delete;
    void open(const wchar_t* filename, ios_base::openmode mode) = delete;
    void open(const std::string& filename, ios_base::openmode mode) = delete;
    void open(const std::filesystem::path& filename, ios_base::openmode mode) = delete;
};

class Win32SharedAccessFstream : private Win32SharedAccessFileDescriptor, public std::fstream {
public:
    explicit Win32SharedAccessFstream(const char* filename,
                                      ios_base::openmode mode = ios_base::in | ios_base::out,
                                      bool sharedWriteAccess = false);
    explicit Win32SharedAccessFstream(const wchar_t* filename,
                                      ios_base::openmode mode = ios_base::in | ios_base::out,
                                      bool sharedWriteAccess = false);
    explicit Win32SharedAccessFstream(const std::string& filename,
                                      ios_base::openmode mode = ios_base::in | ios_base::out,
                                      bool sharedWriteAccess = false);
    explicit Win32SharedAccessFstream(const std::filesystem::path& filename,
                                      ios_base::openmode mode = ios_base::in | ios_base::out,
                                      bool sharedWriteAccess = false);

    // The Visual Studio extension that operates on FILE* does not have an open overload. Needs to
    // open with constructor.
    void open(const char* filename, ios_base::openmode mode) = delete;
    void open(const wchar_t* filename, ios_base::openmode mode) = delete;
    void open(const std::string& filename, ios_base::openmode mode) = delete;
    void open(const std::filesystem::path& filename, ios_base::openmode mode) = delete;
};

}  // namespace mongo

#endif  // _WIN32
