/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/sorter/sorter_stats.h"
#include "mongo/util/modules.h"

#include <fstream>
#include <ios>
#include <system_error>

#include <boost/filesystem/path.hpp>

MONGO_MOD_PUBLIC;
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
