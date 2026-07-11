// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/ftdc/compressor.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/metadata_compressor.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <cstdint>
#include <fstream>  // IWYU pragma: keep
#include <vector>

#include <boost/filesystem/path.hpp>
#include <boost/optional/optional.hpp>

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
    FTDCFileWriter(const FTDCFileWriter&) = delete;
    FTDCFileWriter& operator=(const FTDCFileWriter&) = delete;

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
     * Write a periodic metadata sample to the archive log as needed.
     */
    Status writePeriodicMetadataSample(const BSONObj& sample, Date_t date);

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

    // FTDC periodic metadata compressor
    FTDCMetadataCompressor _metadataCompressor;

    // Size of archive file
    std::size_t _size{0};

    // Size of interim file
    std::size_t _sizeInterim{0};
};

}  // namespace mongo
