// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/message_compressor_base.h"

#include "mongo/db/stats/counters.h"

namespace mongo {
namespace {

NetworkCounter::ConnectionType connectionType(MessageCompressionDirection direction) {
    switch (direction) {
        case MessageCompressionDirection::kIngress:
            return NetworkCounter::ConnectionType::kIngress;
        case MessageCompressionDirection::kEgress:
            return NetworkCounter::ConnectionType::kEgress;
        case MessageCompressionDirection::kUncounted:
            break;
    }
    MONGO_UNREACHABLE;
}

}  // namespace

StatusWith<std::size_t> MessageCompressorBase::compressData(ConstDataRange input,
                                                            DataRange output,
                                                            MessageCompressionDirection direction) {
    auto sws = doCompressData(input, output);
    if (!sws.isOK() || direction == MessageCompressionDirection::kUncounted) {
        return sws;
    }

    const int64_t bytesIn = input.length();
    const int64_t bytesOut = sws.getValue();
    _counterHitCompress(direction, bytesIn, bytesOut);
    globalNetworkCounter().hitCompressor(connectionType(direction), _name, bytesIn, bytesOut);
    return sws;
}

StatusWith<std::size_t> MessageCompressorBase::decompressData(
    ConstDataRange input, DataRange output, MessageCompressionDirection direction) {
    auto sws = doDecompressData(input, output);
    if (!sws.isOK() || direction == MessageCompressionDirection::kUncounted) {
        return sws;
    }

    const int64_t bytesIn = input.length();
    const int64_t bytesOut = sws.getValue();
    _counterHitDecompress(direction, bytesIn, bytesOut);
    globalNetworkCounter().hitDecompressor(connectionType(direction), _name, bytesIn, bytesOut);
    return sws;
}

}  // namespace mongo
