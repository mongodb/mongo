// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#if defined(_WIN32) && defined(_MSC_VER)

#include "mongo/util/modules.h"

#include <fstream>  // IWYU pragma: keep
#include <string_view>

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
    static FILE* _open(std::string_view filename,
                       std::ios_base::openmode mode,
                       bool sharedWriteAccess);

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
