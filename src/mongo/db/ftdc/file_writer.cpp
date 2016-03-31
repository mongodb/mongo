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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kFTDC

#include "mongo/platform/basic.h"

#include "mongo/db/ftdc/file_writer.h"

#include <boost/filesystem.hpp>
#include <fstream>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/ftdc/compressor.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

FTDCFileWriter::~FTDCFileWriter() {
    close();
}

Status FTDCFileWriter::open(const boost::filesystem::path& file) {
    if (_archiveStream.is_open()) {
        return {ErrorCodes::FileAlreadyOpen, "FTDCFileWriter is already open."};
    }

    _archiveFile = file;

    // Disable file buffering
    _archiveStream.rdbuf()->pubsetbuf(0, 0);

    // Ideally, we create a file from scratch via O_CREAT but there is not portable way via C++
    // iostreams to do this.
    _archiveStream.open(_archiveFile.c_str(),
                        std::ios_base::out | std::ios_base::binary | std::ios_base::app);

    if (!_archiveStream.is_open()) {
        return Status(ErrorCodes::FileNotOpen,
                      "Failed to open archive file " + file.generic_string());
    }

    // Set internal size tracking to reflect the current file size
    _size = boost::filesystem::file_size(file);

    _sizeInterim = 0;

    _interimFile = FTDCUtil::getInterimFile(file);
    _interimTempFile = FTDCUtil::getInterimTempFile(file);

    _compressor.reset();

    return Status::OK();
}

Status FTDCFileWriter::writeInterimFileBuffer(ConstDataRange buf) {
    // Fixed size interim stream
    std::ofstream interimStream;

    // Disable file buffering
    interimStream.rdbuf()->pubsetbuf(0, 0);

    // Open up a temporary interim file
    interimStream.open(_interimTempFile.c_str(),
                       std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);

    if (!interimStream.is_open()) {
        return Status(ErrorCodes::FileNotOpen,
                      "Failed to open interim file " + _interimTempFile.generic_string());
    }

    interimStream.write(buf.data(), buf.length());

    if (interimStream.fail()) {
        return {
            ErrorCodes::FileStreamFailed,
            str::stream()
                << "Failed to write to interim file buffer for full-time diagnostic data capture: "
                << _interimTempFile.generic_string()};
    }

    interimStream.close();

    // Now that the temp interim file is closed, rename the temp interim file to the real one.
    boost::system::error_code ec;
    boost::filesystem::rename(_interimTempFile, _interimFile, ec);
    if (ec) {
        return Status(ErrorCodes::FileRenameFailed, ec.message());
    }

    _sizeInterim = buf.length();

    return Status::OK();
}

Status FTDCFileWriter::writeArchiveFileBuffer(ConstDataRange buf) {
    _archiveStream.write(buf.data(), buf.length());

    if (_archiveStream.fail()) {
        return {
            ErrorCodes::FileStreamFailed,
            str::stream()
                << "Failed to write to archive file buffer for full-time diagnostic data capture: "
                << _archiveFile.generic_string()};
    }

    _size += buf.length();

    return Status::OK();
}

Status FTDCFileWriter::writeMetadata(const BSONObj& metadata, Date_t date) {
    BSONObj wrapped = FTDCBSONUtil::createBSONMetadataDocument(metadata, date);

    return writeArchiveFileBuffer({wrapped.objdata(), static_cast<size_t>(wrapped.objsize())});
}

Status FTDCFileWriter::writeSample(const BSONObj& sample, Date_t date) {
    auto ret = _compressor.addSample(sample, date);

    if (!ret.isOK()) {
        return ret.getStatus();
    }

    if (ret.getValue().is_initialized()) {
        return flush(std::get<0>(ret.getValue().get()), std::get<2>(ret.getValue().get()));
    }

    if (_compressor.getSampleCount() != 0 &&
        (_compressor.getSampleCount() % _config->maxSamplesPerInterimMetricChunk) == 0) {
        // Check if we want to do a partial write to the interim buffer
        auto swBuf = _compressor.getCompressedSamples();
        if (!swBuf.isOK()) {
            return swBuf.getStatus();
        }

        BSONObj o = FTDCBSONUtil::createBSONMetricChunkDocument(std::get<0>(swBuf.getValue()),
                                                                std::get<1>(swBuf.getValue()));
        return writeInterimFileBuffer({o.objdata(), static_cast<size_t>(o.objsize())});
    }

    return Status::OK();
}

Status FTDCFileWriter::flush(const boost::optional<ConstDataRange>& range, Date_t date) {
    if (!range.is_initialized()) {
        if (_compressor.hasDataToFlush()) {
            auto swBuf = _compressor.getCompressedSamples();

            if (!swBuf.isOK()) {
                return swBuf.getStatus();
            }

            BSONObj o = FTDCBSONUtil::createBSONMetricChunkDocument(std::get<0>(swBuf.getValue()),
                                                                    std::get<1>(swBuf.getValue()));
            Status s = writeArchiveFileBuffer({o.objdata(), static_cast<size_t>(o.objsize())});

            if (!s.isOK()) {
                return s;
            }
        }
    } else {
        BSONObj o = FTDCBSONUtil::createBSONMetricChunkDocument(range.get(), date);
        Status s = writeArchiveFileBuffer({o.objdata(), static_cast<size_t>(o.objsize())});

        if (!s.isOK()) {
            return s;
        }
    }

    boost::filesystem::remove(_interimFile);

    return Status::OK();
}

Status FTDCFileWriter::close() {
    if (_archiveStream.is_open()) {
        Status s = flush(boost::none, Date_t());

        _archiveStream.close();

        return s;
    }

    return Status::OK();
}

void FTDCFileWriter::closeWithoutFlushForTest() {
    _archiveStream.close();
}

}  // namespace mongo
