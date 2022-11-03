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

#pragma once

#include "mongo/db/catalog/virtual_collection_options.h"
#include "mongo/db/storage/input_stream.h"
#include "mongo/db/storage/named_pipe.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {
class MultiBsonStreamCursor : public SeekableRecordCursor {
public:
    MultiBsonStreamCursor(const VirtualCollectionOptions& vopts)
        : _numStreams(vopts.dataSources.size()), _vopts(vopts) {
        tassert(6968310, "_numStreams {} <= 0"_format(_numStreams), _numStreams > 0);
        _streamReader = getInputStream(_vopts.dataSources[_streamIdx].url);
    }

    boost::optional<Record> next() override;

    // This block of overrides are all essentially no-ops as they are for lock yielding but the
    // external data source is read-only.
    void save() override {}
    void saveUnpositioned() override {}
    bool restore(bool tolerateCappedRepositioning) override {
        return true;
    }
    void detachFromOperationContext() override {}
    void reattachToOperationContext(OperationContext* opCtx) override {}
    void setSaveStorageCursorOnDetachFromOperationContext(bool) override {}

    // Seeking is not currently supported.
    boost::optional<Record> seekExact(const RecordId& id) {
        tasserted(6968300, "MultiBsonStreamCursor::seekExact is not supported");
        return boost::none;
    }

    // Seeking is not currently supported.
    boost::optional<Record> seekNear(const RecordId& start) {
        tasserted(6968301, "MultiBsonStreamCursor::seekNear is not supported");
        return boost::none;
    }

private:
    void expandBuffer(int32_t bsonSize);
    boost::optional<Record> nextFromCurrentStream();
    static std::unique_ptr<InputStream<NamedPipeInput>> getInputStream(const std::string& url);

    // The size in bytes of a BSON object's "size" prefix.
    static constexpr int kSizeSize = static_cast<int>(sizeof(int32_t));

    // The buffer starts small and doubles as needed to have room to read the rest of an object that
    // spans across the half-way point. Block reads are done at offset 0 and get only half the
    // buffer size, so worst case is the last byte of this read is the first byte of the size of the
    // following object. (There will always be room to finish reading the size.) The buffer will
    // never grow larger than (2 * BSONObjMaxUserSize) as that size is large enough to satisfy any
    // stream of objects using the defined algorithm. Except when the buffer is enlarged, none of
    // its contents are ever copied.
    //
    // Buffer starting size should be a power of two since BSONObjMaxUserSize is one. The smallest
    // valid starting size in the current implementation is 8 bytes, as a half-buffer block read
    // expects to be able to get the 4-byte size of the first BSON object. We start somewhat larger
    // to avoid memory fragmentation, as most BSON objects will not be so tiny, however, many may be
    // quite small. The starting size is chosen as 8K because at the time of writing the most common
    // Linux filesystem block size is 4K, and our block read size is half the buffer size.
    int _bufSize = 8 * 1024;                              // current buffer size
    int _blockReadSize = _bufSize / 2;                    // size of a block read
    std::unique_ptr<char[]> _buffer{new char[_bufSize]};  // the buffer itself
    int _bufBegin = 0;  // index of first unconsumed byte in '_buffer'
    int _bufEnd = 0;    // index one past the last valid byte in '_buffer'

    int64_t _nextRecordId = 0;  // for artificially generating the record IDs
    int _numStreams;            // number of streams in '_vopts'
    int _streamIdx = 0;         // index in' _vopts' of stream being consumed in '_streamReader'

    // Reader for the current stream.
    std::unique_ptr<InputStream<NamedPipeInput>> _streamReader = nullptr;

    const VirtualCollectionOptions& _vopts;  // metadata containing the pipe URLs
};
}  // namespace mongo
