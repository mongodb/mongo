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
#include <boost/optional.hpp>
#include <fstream>
#include <stddef.h>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/ftdc/decompressor.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/db/jsobj.h"

namespace mongo {

/**
 * Reads a file, either an archive stream or interim file
 *
 * Does not recover interim files into archive files.
 */
class FTDCFileReader {
    MONGO_DISALLOW_COPYING(FTDCFileReader);

public:
    FTDCFileReader() : _state(State::kNeedsDoc) {}
    ~FTDCFileReader();

    /**
     * Open the specified file
     */
    Status open(const boost::filesystem::path& file);

    /**
     * Returns true if their are more records in the file.
     * Returns false if the end of the file has been reached.
     * Return other error codes if the file is corrupt.
     */
    StatusWith<bool> hasNext();

    /**
     * Returns the next document.
     * Metadata documents are unowned.
     * Metric documents are owned.
     */
    std::tuple<FTDCBSONUtil::FTDCType, const BSONObj&, Date_t> next();

private:
    /**
     * Read a document from the file. If the file is corrupt, returns an appropriate status.
     */
    StatusWith<BSONObj> readDocument();

private:
    FTDCDecompressor _decompressor;

    /**
     * Internal state of file reading state machine.
     *
     *      +--------------+      +--------------+
     *   +> |  kNeedsDoc   | <--> | kMetadataDoc |
     *   |  +--------------+      +--------------+
     *   |
     *   |    +----------+
     *   |    v          |
     *   |  +--------------+
     *   +> | kMetricChunk |
     *      +--------------+
     */
    enum class State {

        /**
         * Indicates that we need to read another document from disk
         */
        kNeedsDoc,

        /**
         * Indicates that are processing a metric chunk, and have one or more documents to return.
         */
        kMetricChunk,

        /**
         * Indicates that we need to read another document from disk
         */
        kMetadataDoc,
    };

    State _state{State::kNeedsDoc};

    // Current position in the _docs array.
    std::size_t _pos{0};

    // Current set of metrics documents
    std::vector<BSONObj> _docs;

    // _id of current metadata or metric chunk
    Date_t _dateId;

    // Current metadata document - unowned
    BSONObj _metadata;

    // Parent document
    BSONObj _parent;

    // Buffer of data read from disk
    std::vector<char> _buffer;

    // File name
    boost::filesystem::path _file;

    // Size of file on disk
    std::size_t _fileSize{0};

    // Input file stream
    std::ifstream _stream;
};

}  // namespace mongo
