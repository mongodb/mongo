// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/status_with.h"
#include "mongo/platform/atomic.h"
#include "mongo/transport/message_compressor_name.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <array>
#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class MessageCompressorBase {
    MessageCompressorBase(const MessageCompressorBase&) = delete;
    MessageCompressorBase& operator=(const MessageCompressorBase&) = delete;

public:
    virtual ~MessageCompressorBase() = default;

    /*
     * Returns the name for subclass compressors (e.g. "snappy", "zlib", "zstd" or "noop")
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
     * status. The byte counts are attributed to 'direction', which may be kUncounted for
     * compression that is not wire-protocol traffic.
     */
    StatusWith<std::size_t> compressData(
        ConstDataRange input,
        DataRange output,
        MessageCompressionDirection direction = MessageCompressionDirection::kUncounted);

    /*
     * This method decompresses the data in the input ConstDataRange into the output DataRange.
     * It returns the number of bytes actually decompressed into the output range, or an error
     * status. The byte counts are attributed to 'direction', which may be kUncounted for
     * decompression that is not wire-protocol traffic.
     */
    StatusWith<std::size_t> decompressData(
        ConstDataRange input,
        DataRange output,
        MessageCompressionDirection direction = MessageCompressionDirection::kUncounted);

    /*
     * Returns the max uncompressed length of the data in the input ConstDataRange as given by the
     * header, if available.
     */
    virtual boost::optional<std::size_t> getMaxDecompressedSize(ConstDataRange input) = 0;

    /*
     * Returns the number of bytes passed in the input to compressData. With no direction,
     * returns the total across all counted directions.
     */
    int64_t getCompressorBytesIn(
        boost::optional<MessageCompressionDirection> direction = boost::none) const {
        return _sum(_compressBytesIn, direction);
    }

    /*
     * Returns the number of bytes written to the output by compressData. With no direction,
     * returns the total across all counted directions.
     */
    int64_t getCompressorBytesOut(
        boost::optional<MessageCompressionDirection> direction = boost::none) const {
        return _sum(_compressBytesOut, direction);
    }

    /*
     * Returns the number of bytes passed in the input to decompressData. With no direction,
     * returns the total across all counted directions.
     */
    int64_t getDecompressorBytesIn(
        boost::optional<MessageCompressionDirection> direction = boost::none) const {
        return _sum(_decompressBytesIn, direction);
    }

    /*
     * Returns the number of bytes written to the output by decompressData. With no direction,
     * returns the total across all counted directions.
     */
    int64_t getDecompressorBytesOut(
        boost::optional<MessageCompressionDirection> direction = boost::none) const {
        return _sum(_decompressBytesOut, direction);
    }

protected:
    /*
     * This is called by sub-classes to intialize their ID/name fields.
     */
    MessageCompressorBase(MessageCompressor id)
        : _id{static_cast<MessageCompressorId>(id)},
          _name{std::string{getMessageCompressorName(id)}} {}

    /*
     * Sub-classes implement the actual (de)compression here; called by the public
     * compressData/decompressData.
     */
    virtual StatusWith<std::size_t> doCompressData(ConstDataRange input, DataRange output) = 0;
    virtual StatusWith<std::size_t> doDecompressData(ConstDataRange input, DataRange output) = 0;

private:
    using DirectionCounters = std::array<Atomic<long long>, kNumCountedCompressionDirections>;

    /*
     * Bumps the compress byte counters for one compression.
     */
    void _counterHitCompress(MessageCompressionDirection direction,
                             int64_t bytesIn,
                             int64_t bytesOut) {
        _compressBytesIn[size_t(direction)].addAndFetch(bytesIn);
        _compressBytesOut[size_t(direction)].addAndFetch(bytesOut);
    }

    /*
     * Bumps the decompress byte counters for one decompression.
     */
    void _counterHitDecompress(MessageCompressionDirection direction,
                               int64_t bytesIn,
                               int64_t bytesOut) {
        _decompressBytesIn[size_t(direction)].addAndFetch(bytesIn);
        _decompressBytesOut[size_t(direction)].addAndFetch(bytesOut);
    }

    int64_t _sum(const DirectionCounters& counters,
                 boost::optional<MessageCompressionDirection> direction) const {
        if (direction) {
            invariant(size_t(*direction) < kNumCountedCompressionDirections);
            return counters[size_t(*direction)].loadRelaxed();
        }
        int64_t total = 0;
        for (auto&& c : counters) {
            total += c.loadRelaxed();
        }
        return total;
    }

    const MessageCompressorId _id;
    const std::string _name;

    // All four counter arrays are indexed by MessageCompressionDirection.
    DirectionCounters _compressBytesIn;
    DirectionCounters _compressBytesOut;

    DirectionCounters _decompressBytesIn;
    DirectionCounters _decompressBytesOut;
};
}  // namespace mongo
