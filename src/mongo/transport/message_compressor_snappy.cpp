/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/init.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/transport/message_compressor_snappy.h"

#include <snappy-sinksource.h>
#include <snappy.h>

namespace mongo {
namespace {

class SnappySourceSinkException : public DBException {
public:
    SnappySourceSinkException(Status status) : DBException(status.reason(), status.code()) {}
};

// This is a bounds-checking version of snappy::UncheckedByteArraySink.
//
// If the amount of scratch buffer space requested by snappy is larger than the sink
// buffer, than it will allocate a new temporary buffer so that snappy can finish.
// If the amount of scratch buffer space requested by snappy is less than or equal to
// the size of the sink buffer, than it will just return the sink buffer.
//
// If the scratch buffer is the sink buffer, than Append will just advance the buffer
// cursor and do bounds checking without any copying.
//
// Appending data past the end of the sink buffer will throw a SnappySourcesinkexception.
class DataRangeSink final : public snappy::Sink {
public:
    DataRangeSink(DataRange buffer) : _cursor(buffer) {}

    char* GetAppendBuffer(size_t length, char* scratch) final {
        if (length > _cursor.length()) {
            _scratch.resize(length);
            return _scratch.data();
        }

        return const_cast<char*>(_cursor.data());
    }

    void AppendAndTakeOwnership(char* data,
                                size_t n,
                                void (*deleter)(void*, const char*, size_t),
                                void* deleterArg) final {
        Append(data, n);
        if (data != _cursor.data()) {
            (*deleter)(deleterArg, data, n);
        }
    }

    void Append(const char* bytes, size_t n) final {
        Status status = Status::OK();
        if (bytes == _cursor.data()) {
            status = _cursor.advance(n);
        } else {
            ConstDataRange toWrite(bytes, n);
            status = _cursor.writeAndAdvance(toWrite);
        }
        if (!status.isOK()) {
            throw SnappySourceSinkException(std::move(status));
        }
    }

    char* GetAppendBufferVariable(size_t minSize,
                                  size_t desiredSizeHint,
                                  char* scratch,
                                  size_t scratchSize,
                                  size_t* allocatedSize) {
        if (desiredSizeHint > _cursor.length() || minSize > _cursor.length()) {
            _scratch.resize(desiredSizeHint);
            *allocatedSize = _scratch.size();
            return _scratch.data();
        }

        *allocatedSize = _cursor.length();
        return const_cast<char*>(_cursor.data());
    }

private:
    DataRangeCursor _cursor;
    std::vector<char> _scratch;
};

class ConstDataRangeSource final : public snappy::Source {
public:
    ConstDataRangeSource(ConstDataRange buffer) : _cursor(buffer) {}

    size_t Available() const final {
        return _cursor.length();
    }

    const char* Peek(size_t* len) final {
        *len = _cursor.length();
        return _cursor.data();
    }

    void Skip(size_t n) final {
        auto status = _cursor.advance(n);
        if (!status.isOK()) {
            throw SnappySourceSinkException(std::move(status));
        }
    }

private:
    ConstDataRangeCursor _cursor;
};

}  // namespace
SnappyMessageCompressor::SnappyMessageCompressor()
    : MessageCompressorBase(MessageCompressor::kSnappy) {}

std::size_t SnappyMessageCompressor::getMaxCompressedSize(size_t inputSize) {
    // Testing has shown that snappy typically requests two additional bytes of buffer space when
    // compressing beyond what snappy::MaxCompressedLength returns. So by padding this by 2 more
    // bytes, we can avoid additional allocations/copies during compression.
    return snappy::MaxCompressedLength(inputSize) + 2;
}

StatusWith<std::size_t> SnappyMessageCompressor::compressData(ConstDataRange input,
                                                              DataRange output) {
    size_t outLength;
    ConstDataRangeSource source(input);
    DataRangeSink sink(output);

    try {
        outLength = snappy::Compress(&source, &sink);
    } catch (const SnappySourceSinkException& e) {
        return e.toStatus();
    }

    counterHitCompress(input.length(), outLength);
    return {outLength};
}

StatusWith<std::size_t> SnappyMessageCompressor::decompressData(ConstDataRange input,
                                                                DataRange output) {
    try {
        uint32_t expectedLength = 0;
        ConstDataRangeSource lengthCheckSource(input);
        if (!snappy::GetUncompressedLength(&lengthCheckSource, &expectedLength) ||
            expectedLength > output.length()) {
            return {ErrorCodes::BadValue, "Compressed message was invalid or corrupted"};
        }

        ConstDataRangeSource source(input);
        DataRangeSink sink(output);

        bool ret = snappy::Uncompress(&source, &sink);
        if (!ret) {
            return Status{ErrorCodes::BadValue, "Compressed message was invalid or corrupted"};
        }
    } catch (const SnappySourceSinkException& e) {
        return e.toStatus();
    }

    counterHitDecompress(input.length(), output.length());
    return output.length();
}


MONGO_INITIALIZER_GENERAL(SnappyMessageCompressorInit,
                          ("EndStartupOptionHandling"),
                          ("AllCompressorsRegistered"))
(InitializerContext* context) {
    auto& compressorRegistry = MessageCompressorRegistry::get();
    compressorRegistry.registerImplementation(stdx::make_unique<SnappyMessageCompressor>());
    return Status::OK();
}
}  // namespace mongo
