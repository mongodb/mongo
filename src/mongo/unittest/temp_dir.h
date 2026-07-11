// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>
#include <utility>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
namespace unittest {

/**
 * An RAII temporary directory that deletes itself and all contents files on scope exit.
 */
class TempDir {
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

public:
    /**
     * Creates a new unique temporary directory.
     *
     * Throws if this fails for any reason, such as bad permissions.
     *
     * The leaf of the directory path will start with namePrefix and have
     * unspecified characters added to ensure uniqueness.
     *
     * namePrefix must not contain either / or \
     */
    explicit TempDir(const std::string& namePrefix);

    /**
     * Delete the directory and all contents.
     *
     * This only does best-effort. In particular no new files should be created in the directory
     * once the TempDir goes out of scope. Any errors are logged and ignored.
     */
    ~TempDir();

    /**
     * Release the path encapsulated by this TempDir to be cleaned up by the caller as necessary.
     *
     * A released TempDir is left with an empty path, and its destructor will perform no cleanup.
     */
    std::string release() noexcept {
        return std::exchange(_path, {});
    }

    const std::string& path() const {
        return _path;
    }

    /**
     * Set the path where TempDir() will create temporary directories.
     */
    static void setTempPath(std::string tempPath);

private:
    std::string _path;
};
}  // namespace unittest
}  // namespace mongo
