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

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/platform/atomic_word.h"

#include <type_traits>

namespace mongo {
enum class MessageCompressor : uint8_t {
    kNoop = 0,
    kSnappy = 1,
    kZlib = 2,
    kExtended = 255,
};

StringData getMessageCompressorName(MessageCompressor id);
using MessageCompressorId = std::underlying_type<MessageCompressor>::type;

class MessageCompressorBase {
    MONGO_DISALLOW_COPYING(MessageCompressorBase);

public:
    virtual ~MessageCompressorBase() = default;

    /*
     * Returns the name for subclass compressors (e.g. "snappy", "zlib", or "noop")
     */
    const std::string& getName() const {
        return _name;
    }

    /*
     * Returns the numeric ID for subclass compressors (e.g. 1 or 0)
     */
    MessageCompressorId getId() const {
        return _id;
    }

    /*
     * This returns the maximum output size of a call to compressData. It is used
     * by the MessageCompressorManager to determine how big a buffer to allocate.
     */
    virtual std::size_t getMaxCompressedSize(size_t inputSize) = 0;

    /*
     * This method compresses the data in the input ConstDataRange into the output DataRange.
     * It returns the number of bytes actually compressed into the output range, or an error
     * status.
     */
    virtual StatusWith<std::size_t> compressData(ConstDataRange input, DataRange output) = 0;

    /*
     * This method decompresses the data in the input ConstDataRange into the output DataRange.
     * It returns the number of bytes actually decompressed into the output range, or an error
     * status.
     */
    virtual StatusWith<std::size_t> decompressData(ConstDataRange input, DataRange output) = 0;

    /*
     * This returns the number of bytes passed in the input for compressData
     */
    int64_t getCompressedBytesIn() const {
        return _compressBytesIn.loadRelaxed();
    }

    /*
     * This returns the number of bytes written to output for compressData
     */
    int64_t getCompressedBytesOut() const {
        return _compressBytesOut.loadRelaxed();
    }

    /*
     * This returns the number of bytes passed in the input for decompressData
     */
    int64_t getDecompressedBytesIn() const {
        return _decompressBytesIn.loadRelaxed();
    }

    /*
     * This returns the number of bytes written to output for decompressData
     */
    int64_t getDecompressedBytesOut() const {
        return _decompressBytesOut.loadRelaxed();
    }


protected:
    /*
     * This is called by sub-classes to intialize their ID/name fields.
     */
    MessageCompressorBase(MessageCompressor id)
        : _id{static_cast<MessageCompressorId>(id)},
          _name{getMessageCompressorName(id).toString()} {}

    /*
     * Called by sub-classes to bump their bytesIn/bytesOut counters for compression
     */
    void counterHitCompress(int64_t bytesIn, int64_t bytesOut) {
        _compressBytesIn.addAndFetch(bytesIn);
        _compressBytesOut.addAndFetch(bytesOut);
    }

    /*
     * Called by sub-classes to bump their bytesIn/bytesOut counters for decompression
     */
    void counterHitDecompress(int64_t bytesIn, int64_t bytesOut) {
        _decompressBytesIn.addAndFetch(bytesIn);
        _decompressBytesOut.addAndFetch(bytesOut);
    }

private:
    const MessageCompressorId _id;
    const std::string _name;

    AtomicInt64 _compressBytesIn;
    AtomicInt64 _compressBytesOut;

    AtomicInt64 _decompressBytesIn;
    AtomicInt64 _decompressBytesOut;
};
}  // namespace mongo
