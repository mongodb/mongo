/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/virtual_collection/multi_bson_stream_cursor.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/local_catalog/virtual_collection_options.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/platform/compiler.h"

#include <cstring>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {

/**
 * Expands '_buffer' by a multiple of two of its current size that is large enough to contain two
 * objects of size 'bsonSize'. Copies the contents of the old buffer to position 0 of the new buffer
 * and updates bookkeeping. This can never expand the buffer larger than (2 * BSONObjMaxUserSize).
 */
void MultiBsonStreamCursor::expandBuffer(int32_t bsonSize) {
    uassert(6968308,
            fmt::format("bsonSize {} > BSONObjMaxUserSize {}", bsonSize, BSONObjMaxUserSize),
            (bsonSize <= BSONObjMaxUserSize));
    uassert(6968309, fmt::format("bsonSize {} < 0", bsonSize), (bsonSize >= 0));

    int newSizeTarget = 2 * bsonSize;
    do {
        _bufSize *= 2;
    } while (_bufSize < newSizeTarget);
    std::unique_ptr<char[]> newBuffer{new char[_bufSize]};

    _bufEnd -= _bufBegin;
    std::memcpy(newBuffer.get(), (_buffer.get() + _bufBegin), _bufEnd);
    _buffer = std::move(newBuffer);
    _blockReadSize = _bufSize / 2;
    _bufBegin = 0;
}

/**
 * Returns the next record from the current stream or boost::none if exhausted or error.
 */
boost::optional<Record> MultiBsonStreamCursor::nextFromCurrentStream() {
    int32_t bsonSize;  // size of the next BSON object
    int readBytes;     // number of bytes just read
    int remBytes;      // number of remainder bytes to read for either size field or object body
    int availBytes;    // number of unconsumed bytes in '_buffer'

    // The while loop enables dynamically expanding the buffer as needed. If the buffer ever reaches
    // (2 * BSONObjMaxUserSize) bytes it will not need to expand any more.
    while (true) {
        // There are four cases, but for performance they are not fully independent in code:
        //   1. Next full object (size and data) is already in the buffer.
        //   2. Next size is in the buffer but not all of the next object.
        //   3. Next size is only partly present in the buffer.
        //   4. No part of the next object is in the buffer. Reset buffer and read a big block.
        availBytes = _bufEnd - _bufBegin;
        if (availBytes > 0) {  // Cases 1-3
            // Cases 3: get the rest of size. This collapses case 3 into case 2.
            if (availBytes < kSizeSize) {
                remBytes = kSizeSize - availBytes;
                // TODO SERVER-111117 It seems like we should check that we have enough room in the
                // buffer to accomodate the rest of the size.
                readBytes = _streamReader->readBytes(remBytes, (_buffer.get() + _bufEnd));
                if (MONGO_unlikely(readBytes < remBytes)) {
                    uasserted(
                        6968303,
                        fmt::format("Truncated file: {}", _vopts.dataSources[_streamIdx].url));
                    return boost::none;
                }
                _bufEnd += readBytes;
                availBytes += readBytes;
            }
            bsonSize = ConstDataView(_buffer.get() + _bufBegin).read<LittleEndian<int32_t>>();

            // Cases 2-3: get the rest of the object. This collapses cases 2-3 into case 1.
            if (availBytes < bsonSize) {
                remBytes = bsonSize - availBytes;
                if (MONGO_likely(remBytes <= _bufSize - _bufEnd)) {  // 'remBytes' will fit
                    readBytes = _streamReader->readBytes(remBytes, (_buffer.get() + _bufEnd));
                    if (MONGO_unlikely(readBytes < remBytes)) {
                        uasserted(
                            6968304,
                            fmt::format("Truncated file: {}", _vopts.dataSources[_streamIdx].url));
                        return boost::none;
                    }
                    _bufEnd += readBytes;
                    // Not used again: availBytes += readBytes;
                } else {
                    expandBuffer(bsonSize);
                    continue;
                }
            }
        } else {  // Case 4: availBytes == 0; do a block read
            _bufBegin = 0;
            _bufEnd = _streamReader->readBytes(_blockReadSize, _buffer.get());
            if (_bufEnd == 0) {  // EOF: okay here as the pipe ended at an object boundary
                return boost::none;
            }
            if (MONGO_unlikely(_bufEnd < kSizeSize)) {
                uasserted(6968305,
                          fmt::format("Truncated file: {}", _vopts.dataSources[_streamIdx].url));
                return boost::none;
            }
            // Not used again: availBytes += _bufEnd;
            bsonSize = ConstDataView(_buffer.get()).read<LittleEndian<int32_t>>();

            if (MONGO_unlikely(_bufEnd < bsonSize)) {
                if (MONGO_likely(_bufEnd == _blockReadSize)) {  // got all the bytes we requested
                    expandBuffer(bsonSize);
                    continue;
                }
                uasserted(6968306,
                          fmt::format("Truncated file: {}", _vopts.dataSources[_streamIdx].url));
                return boost::none;
            }
        }
        break;  // reaching here means we have the whole object in '_buffer' now
    }

    // All cases are now collapsed to Case 1: the full object is in the buffer at '_bufBegin'.
    // 'recordData.data' includes the size in the first four bytes.
    boost::optional<RecordData> recordData = RecordData{(_buffer.get() + _bufBegin), bsonSize};
    _bufBegin += bsonSize;
    tassert(6968307,
            fmt::format("_bufBegin {} > _bufSize {}", _bufBegin, _bufSize),
            (_bufBegin <= _bufSize));

    return {{RecordId{_nextRecordId++}, std::move(*recordData)}};
}

/**
 * Returns an input stream for a named pipe mapped from 'url'.
 *
 * While creating an input stream, it strips off the file protocol part from the 'url'.
 */
std::unique_ptr<InputStream<NamedPipeInput>> MultiBsonStreamCursor::getInputStream(
    const std::string& url) {
    auto filePathPos = url.find(std::string{ExternalDataSourceMetadata::kUrlProtocolFile});
    tassert(ErrorCodes::BadValue,
            fmt::format("Invalid file url: {}", url),
            filePathPos != std::string::npos);

    auto filePathStr =
        url.substr(filePathPos + ExternalDataSourceMetadata::kUrlProtocolFile.size());

    return std::make_unique<InputStream<NamedPipeInput>>(filePathStr);
}

/**
 * Returns the next record from the vector of streams or boost::none if exhausted or error.
 * '_streamReader' is initialized to the first stream, if there is one, in the constructor.
 */
boost::optional<Record> MultiBsonStreamCursor::next() {
    while (_streamIdx < _numStreams) {
        auto record = nextFromCurrentStream();
        if (MONGO_likely(record)) {
            return record;
        }
        ++_streamIdx;
        if (_streamIdx < _numStreams) {
            _streamReader = getInputStream(_vopts.dataSources[_streamIdx].url);
        }
    }
    return boost::none;
}
}  // namespace mongo
