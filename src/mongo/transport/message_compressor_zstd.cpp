// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/message_compressor_zstd.h"

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
#include "mongo/util/str.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

StatusWith<uint32_t> readZstdOpMsgFlags(const Message& message) {
    if (message.empty() || message.capacity() < MsgData::MsgDataHeaderSize) {
        return {ErrorCodes::BadValue, "Missing compressed message header"};
    }
    const auto header = message.header();
    const auto size = header.getLen();
    if (size < MsgData::MsgDataHeaderSize + static_cast<int32_t>(CompressionHeader::size()) ||
        static_cast<size_t>(size) > message.capacity() ||
        static_cast<size_t>(size) > MaxMessageSizeBytes || header.getNetworkOp() != dbCompressed) {
        return {ErrorCodes::BadValue, "Invalid compressed message header"};
    }
    ConstDataRangeCursor input(header.data(), header.data() + header.dataLen());
    const CompressionHeader compression(&input);
    if (compression.originalOpCode != dbMsg ||
        compression.compressorId != static_cast<MessageCompressorId>(MessageCompressor::kZstd) ||
        compression.uncompressedSize < static_cast<int32_t>(sizeof(uint32_t)) ||
        static_cast<size_t>(compression.uncompressedSize) >
            MaxMessageSizeBytes - MsgData::MsgDataHeaderSize) {
        return {ErrorCodes::BadValue, "Expected a zstd-compressed OP_MSG with valid output size"};
    }

    // No decoder allocation can escape this workspace, even for a hostile frame window size.
    // Bound input and iterations too: empty/skippable frames must not scan an entire message.
    const auto inputSize = std::min<size_t>(input.length(), 256 * 1024);
    const auto workspaceSize = ZSTD_estimateDStreamSize_fromFrame(input.data(), inputSize);
    if (ZSTD_isError(workspaceSize) || workspaceSize == 0 ||
        workspaceSize > kZstdOpMsgPrefixWorkspaceBytes) {
        return {ErrorCodes::BadValue,
                "zstd flags prefix exceeds workspace limit or has invalid header"};
    }
    std::unique_ptr<uint64_t[]> workspace(
        new uint64_t[(workspaceSize + sizeof(uint64_t) - 1) / sizeof(uint64_t)]);
    auto* stream = ZSTD_initStaticDStream(workspace.get(), workspaceSize);
    if (!stream || ZSTD_isError(ZSTD_initDStream(stream))) {
        return {ErrorCodes::BadValue, "Could not initialize zstd flags prefix decoder"};
    }
    std::array<char, sizeof(uint32_t)> prefix{};
    ZSTD_inBuffer source{input.data(), inputSize, 0};
    ZSTD_outBuffer output{prefix.data(), prefix.size(), 0};
    for (size_t attempt = 0; attempt < 64; ++attempt) {
        const auto previousInput = source.pos;
        const auto previousOutput = output.pos;
        const auto result = ZSTD_decompressStream(stream, &output, &source);
        if (ZSTD_isError(result)) {
            return {ErrorCodes::BadValue, "Could not decode zstd flags prefix"};
        }
        if (output.pos == prefix.size()) {
            const auto flags = ConstDataView(prefix.data()).read<LittleEndian<uint32_t>>();
            if (OpMsg::hasUnknownRequiredFlags(flags)) {
                return {ErrorCodes::IllegalOpMsgFlag,
                        "zstd-compressed OP_MSG contains illegal flags"};
            }
            return flags;
        }
        if (source.pos == previousInput && output.pos == previousOutput) {
            break;
        }
    }
    return {ErrorCodes::BadValue, "Truncated zstd flags prefix or prefix work limit exceeded"};
}

ZstdMessageCompressor::ZstdMessageCompressor() : MessageCompressorBase(MessageCompressor::kZstd) {}

std::size_t ZstdMessageCompressor::getMaxCompressedSize(size_t inputSize) {
    return ZSTD_compressBound(inputSize);
}

StatusWith<std::size_t> ZstdMessageCompressor::doCompressData(ConstDataRange input,
                                                              DataRange output) {
    size_t ret = ZSTD_compress(const_cast<char*>(output.data()),
                               output.length(),
                               input.data(),
                               input.length(),
                               ZSTD_CLEVEL_DEFAULT);

    if (ZSTD_isError(ret)) {
        return Status{ErrorCodes::BadValue,
                      str::stream() << "Could not compress input: " << ZSTD_getErrorName(ret)};
    }
    return {ret};
}

StatusWith<std::size_t> ZstdMessageCompressor::doDecompressData(ConstDataRange input,
                                                                DataRange output) {
    size_t ret = ZSTD_decompress(
        const_cast<char*>(output.data()), output.length(), input.data(), input.length());

    if (ZSTD_isError(ret)) {
        return Status{ErrorCodes::BadValue,
                      str::stream() << "Could not decompress message: " << ZSTD_getErrorName(ret)};
    }

    return {ret};
}

boost::optional<std::size_t> ZstdMessageCompressor::getMaxDecompressedSize(ConstDataRange input) {
    auto maxDecompressedSize = ZSTD_getFrameContentSize(input.data(), input.length());
    if (maxDecompressedSize == ZSTD_CONTENTSIZE_UNKNOWN ||
        maxDecompressedSize == ZSTD_CONTENTSIZE_ERROR) {
        return boost::none;
    }
    return static_cast<size_t>(maxDecompressedSize);
}


MONGO_INITIALIZER_GENERAL(ZstdMessageCompressorInit,
                          ("EndStartupOptionHandling"),
                          ("AllCompressorsRegistered"))
(InitializerContext* context) {
    auto& compressorRegistry = MessageCompressorRegistry::get();
    compressorRegistry.registerImplementation(std::make_unique<ZstdMessageCompressor>());
}
}  // namespace mongo
