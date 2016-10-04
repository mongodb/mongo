/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <boost/filesystem/path.hpp>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/ftdc/compressor.h"
#include "mongo/db/jsobj.h"

namespace mongo {

/**
 * Manages writing to an append only archive file, and an interim file.
 *  - The archive file is designed to write complete metric chunks.
 *  - The interim file writes smaller chunks in case of process failure.
 *
 * Note: This class never reads from the interim file. It is the callers responsibility to check
 * for unclean shutdown as this file. An unclean shutdown will mean there is valid data in the
 * interim file. If the shutdown is clean, the interim file will contain zeros as the leading
 * 8 bytes instead of a valid BSON length.
 *
 * The chunks in the archive stream will have better compression since it compresses larger chunks
 * of data.
 *
 * File format is compatible with mongodump as it is just a sequential series of bson documents
 *
 * File rotation and cleanup is not handled by this class.
 */
class FTDCFileWriter {
    MONGO_DISALLOW_COPYING(FTDCFileWriter);

public:
    FTDCFileWriter(const FTDCConfig* config) : _config(config), _compressor(_config) {}
    ~FTDCFileWriter();

    /**
     * Open both an archive file, and interim file.
     */
    Status open(const boost::filesystem::path& file);

    /**
     * Write a BSON document as a metadata type to the archive log.
     */
    Status writeMetadata(const BSONObj& metadata, Date_t date);

    /**
     * Write a sample to interim and/or archive log as needed.
     */
    Status writeSample(const BSONObj& sample, Date_t date);

    /**
     * Close all the files and shutdown cleanly by zeroing the beginning of the interim file.
     */
    Status close();

    /**
     * Get the size of data written to file. Size of file after file is closed due to effects of
     * compression may be different.
     */
    std::size_t getSize() const {
        return _size + _sizeInterim;
    }

public:
    /**
     * Test hook that closes the files without moving interim results to the archive log.
     * Note: OS Buffers are still flushes correctly though.
     */
    void closeWithoutFlushForTest();

private:
    /**
     * Flush all changes to disk.
     */
    Status flush(const boost::optional<ConstDataRange>&, Date_t date);

    /**
     * Write a buffer to the beginning of the interim file.
     */
    Status writeInterimFileBuffer(ConstDataRange buf);

    /**
     * Append a buffer to the archive file.
     */
    Status writeArchiveFileBuffer(ConstDataRange buf);

private:
    // Config
    const FTDCConfig* const _config;

    // Archive file name
    boost::filesystem::path _archiveFile;

    // Interim file name
    boost::filesystem::path _interimFile;

    // Interim temp file name
    boost::filesystem::path _interimTempFile;

    // Append only archive stream
    std::ofstream _archiveStream;

    // FTDC compressor
    FTDCCompressor _compressor;

    // Size of archive file
    std::size_t _size{0};

    // Size of interim file
    std::size_t _sizeInterim{0};
};

}  // namespace mongo
