// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/sorter/sorter_stats.h"
#include "mongo/util/modules.h"

#include <fstream>
#include <ios>
#include <system_error>

#include <boost/filesystem/path.hpp>

[[MONGO_MOD_PUBLIC]];
namespace mongo::sorter {

/**
 * Represents the file that a Sorter can use to spill to disk. Supports reading and writing
 * (append-only).
 */
class File {
public:
    File(boost::filesystem::path path, SorterFileStats* stats);
    ~File();

    const boost::filesystem::path& path() const {
        return _path;
    }

    /**
     * Signals that the on-disk file should not be cleaned up.
     */
    void keep() {
        _keep = true;
    };

    /**
     * Reads the requested data from the file. Cannot write more to the file once this has
     * been called.
     */
    void read(std::streamoff offset, std::streamsize size, void* out);

    /**
     * Writes the given data to the end of the file. Cannot be called after reading.
     */
    void write(const char* data, std::streamsize size);

    /**
     * Returns the current offset of the end of the file. Cannot be called after reading.
     */
    std::streamoff currentOffset();

    /**
     * Returns the fileStats ptr for the current file.
     */
    SorterFileStats* getFileStats();

private:
    void _open();

    /**
     * Ensures that the file is open and that _offset is set to the end of the file.
     */
    void _ensureOpenForWriting();

    /**
     * Returns lastPosixError() or a generic iostream error code if no posix error set.
     */
    std::error_code _getErrorCode();

    // The current offset of the end of the file if there may be unflushed data, or -1 if the
    // file either has not yet been opened or has been flushed.
    std::streamoff _offset = -1;

    std::fstream _file;

    // Whether to keep the on-disk file even after this in-memory object has been destructed.
    bool _keep = false;

    // If set, this points to an external metrics holder for tracking storage open/close
    // activity.
    SorterFileStats* _stats;

    boost::filesystem::path _path;
};

}  // namespace mongo::sorter
