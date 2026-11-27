// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/message_compressor_snappy.h"

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/rpc/op_compressed.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/message_compressor_registry.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>

#include <snappy.h>

#include <boost/move/utility_core.hpp>

namespace mongo {

StatusWith<uint32_t> readSnappyOpMsgFlags(const Message& message) {
    if (message.empty() || message.capacity() < MsgData::MsgDataHeaderSize) {
        return {ErrorCodes::BadValue, "Missing compressed message header"};
    }

    const auto header = message.header();
    const auto messageSize = header.getLen();
    if (messageSize <
            MsgData::MsgDataHeaderSize + static_cast<int32_t>(CompressionHeader::size()) ||
        static_cast<size_t>(messageSize) > message.capacity() ||
        static_cast<size_t>(messageSize) > MaxMessageSizeBytes ||
        header.getNetworkOp() != dbCompressed) {
        return {ErrorCodes::BadValue, "Invalid compressed message header"};
    }

    ConstDataRangeCursor input(header.data(), header.data() + header.dataLen());
    const CompressionHeader compressionHeader(&input);
    if (compressionHeader.originalOpCode != dbMsg ||
        compressionHeader.compressorId !=
            static_cast<MessageCompressorId>(MessageCompressor::kSnappy)) {
        return {ErrorCodes::BadValue, "Expected a Snappy-compressed OP_MSG"};
    }
    if (compressionHeader.uncompressedSize < static_cast<int32_t>(sizeof(uint32_t)) ||
        static_cast<size_t>(compressionHeader.uncompressedSize) >
            MaxMessageSizeBytes - MsgData::MsgDataHeaderSize) {
        return {ErrorCodes::BadValue, "Invalid uncompressed OP_MSG size"};
    }

    const auto* data = reinterpret_cast<const uint8_t*>(input.data());
    const auto inputSize = input.length();
    size_t position = 0;
    uint32_t uncompressedSize = 0;
    bool lengthComplete = false;
    for (size_t i = 0; i < 5; ++i) {
        if (position == inputSize) {
            return {ErrorCodes::BadValue, "Truncated Snappy length"};
        }
        const auto byte = data[position++];
        if (i == 4 && byte >= 16) {
            return {ErrorCodes::BadValue, "Snappy length exceeds uint32_t"};
        }
        uncompressedSize |= uint32_t(byte & 0x7f) << (7 * i);
        if (!(byte & 0x80)) {
            lengthComplete = true;
            break;
        }
    }
    if (!lengthComplete ||
        uncompressedSize != static_cast<uint32_t>(compressionHeader.uncompressedSize)) {
        return {ErrorCodes::BadValue, "Snappy length does not match compressed message header"};
    }

    std::array<uint8_t, sizeof(uint32_t)> prefix{};
    size_t produced = 0;
    while (produced < prefix.size()) {
        if (position == inputSize) {
            return {ErrorCodes::BadValue, "Truncated Snappy flags prefix"};
        }
        const auto tag = data[position++];
        const auto tagType = tag & 3;
        uint64_t length = 0;
        if (tagType == 0) {
            const auto encodedLength = tag >> 2;
            if (encodedLength < 60) {
                length = encodedLength + 1;
            } else {
                const size_t lengthBytes = encodedLength - 59;
                if (lengthBytes > inputSize - position) {
                    return {ErrorCodes::BadValue, "Truncated Snappy literal length"};
                }
                for (size_t i = 0; i < lengthBytes; ++i) {
                    length |= uint64_t(data[position++]) << (8 * i);
                }
                ++length;
            }
            if (length > uncompressedSize - produced || length > inputSize - position) {
                return {ErrorCodes::BadValue, "Invalid or truncated Snappy literal"};
            }
            const auto prefixLength = std::min<size_t>(length, prefix.size() - produced);
            std::copy_n(data + position, prefixLength, prefix.data() + produced);
            position += static_cast<size_t>(length);
            produced += prefixLength;
        } else {
            uint32_t offset = 0;
            const size_t offsetBytes = tagType == 1 ? 1 : (tagType == 2 ? 2 : 4);
            if (offsetBytes > inputSize - position) {
                return {ErrorCodes::BadValue, "Truncated Snappy copy offset"};
            }
            for (size_t i = 0; i < offsetBytes; ++i) {
                offset |= uint32_t(data[position++]) << (8 * i);
            }
            if (tagType == 1) {
                length = 4 + ((tag >> 2) & 7);
                offset |= uint32_t(tag & 0xe0) << 3;
            } else {
                length = 1 + (tag >> 2);
            }
            if (offset == 0 || offset > produced || length > uncompressedSize - produced) {
                return {ErrorCodes::BadValue, "Invalid Snappy copy in flags prefix"};
            }
            const auto prefixLength = std::min<size_t>(length, prefix.size() - produced);
            for (size_t i = 0; i < prefixLength; ++i) {
                prefix[produced] = prefix[produced - offset];
                ++produced;
            }
        }
    }

    const auto flags =
        ConstDataView(reinterpret_cast<const char*>(prefix.data())).read<LittleEndian<uint32_t>>();
    if (OpMsg::hasUnknownRequiredFlags(flags)) {
        return {ErrorCodes::IllegalOpMsgFlag, "Snappy-compressed OP_MSG contains illegal flags"};
    }
    return flags;
}

SnappyMessageCompressor::SnappyMessageCompressor()
    : MessageCompressorBase(MessageCompressor::kSnappy) {}

std::size_t SnappyMessageCompressor::getMaxCompressedSize(size_t inputSize) {
    return snappy::MaxCompressedLength(inputSize);
}

boost::optional<std::size_t> SnappyMessageCompressor::getMaxDecompressedSize(ConstDataRange input) {
    size_t length = 0;
    if (snappy::GetUncompressedLength(input.data(), input.length(), &length)) {
        return length;
    }
    return boost::none;
}

StatusWith<std::size_t> SnappyMessageCompressor::doCompressData(ConstDataRange input,
                                                                DataRange output) {
    size_t outLength = output.length();
    if (output.length() < getMaxCompressedSize(input.length())) {
        return {ErrorCodes::BadValue, "Output too small for max size of compressed input"};
    }
    snappy::RawCompress(input.data(), input.length(), const_cast<char*>(output.data()), &outLength);

    return {outLength};
}

StatusWith<std::size_t> SnappyMessageCompressor::doDecompressData(ConstDataRange input,
                                                                  DataRange output) {
    size_t expectedLength = 0;
    if (!snappy::GetUncompressedLength(input.data(), input.length(), &expectedLength) ||
        expectedLength != output.length()) {
        return {ErrorCodes::BadValue, "Compressed message was invalid or corrupted"};
    }

    if (!snappy::RawUncompress(input.data(), input.length(), const_cast<char*>(output.data()))) {
        return Status{ErrorCodes::BadValue, "Compressed message was invalid or corrupted"};
    }

    return output.length();
}


MONGO_INITIALIZER_GENERAL(SnappyMessageCompressorInit,
                          ("EndStartupOptionHandling"),
                          ("AllCompressorsRegistered"))
(InitializerContext* context) {
    auto& compressorRegistry = MessageCompressorRegistry::get();
    compressorRegistry.registerImplementation(std::make_unique<SnappyMessageCompressor>());
}
}  // namespace mongo
