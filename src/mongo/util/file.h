// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

// Cross-platform basic file class. Supports 64-bit offsets and such.

#include "mongo/util/modules.h"

#include <cstdint>
#include <string>

namespace [[MONGO_MOD_PUBLIC]] mongo {

typedef uint64_t fileofs;

// NOTE: not thread-safe. (at least the windows implementation isn't)

class [[MONGO_MOD_NEEDS_REPLACEMENT]] File {
public:
    // NEEDS_REPLACEMENT: This is an old API with design issues (explicit bad() calls, no error
    // reporting)
    File();
    ~File();

    bool bad() const {
        return _bad;
    }
    void fsync() const;
    bool is_open() const;
    fileofs len();
    void open(const char* filename, bool readOnly = false, bool direct = false);
    void read(fileofs o, char* data, unsigned len);
    void truncate(fileofs size);
    void write(fileofs o, const char* data, unsigned len);

    static intmax_t freeSpace(const std::string& path);

private:
    bool _bad;
#ifdef _WIN32
    HANDLE _handle;
#else
    int _fd;
#endif
    std::string _name;
};
}  // namespace mongo
