// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/message_compressor_zlib.h"

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
#include "mongo/util/scopeguard.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>

#include <zconf.h>
#include <zlib.h>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {

StatusWith<uint32_t> readZlibOpMsgFlags(const Message& message) {
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
        compression.compressorId != static_cast<MessageCompressorId>(MessageCompressor::kZlib) ||
        compression.uncompressedSize < static_cast<int32_t>(sizeof(uint32_t)) ||
        static_cast<size_t>(compression.uncompressedSize) >
            MaxMessageSizeBytes - MsgData::MsgDataHeaderSize) {
        return {ErrorCodes::BadValue, "Expected a zlib-compressed OP_MSG with valid output size"};
    }

    auto workspace = std::make_unique<std::max_align_t[]>(
        (kZlibOpMsgPrefixWorkspaceBytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    struct Arena {
        void* next;
        size_t remaining;
    } arena{workspace.get(), kZlibOpMsgPrefixWorkspaceBytes};
    z_stream stream{};
    stream.opaque = &arena;
    stream.zalloc = [](void* opaque, uInt items, uInt size) -> void* {
        auto& arena = *static_cast<Arena*>(opaque);
        if (items == 0 || size == 0 || items > arena.remaining / size) {
            return nullptr;
        }
        const size_t bytes = static_cast<size_t>(items) * size;
        auto* allocated = std::align(alignof(std::max_align_t), bytes, arena.next, arena.remaining);
        if (!allocated) {
            return nullptr;
        }
        arena.next = static_cast<char*>(allocated) + bytes;
        arena.remaining -= bytes;
        return allocated;
    };
    // A prefix decoder is initialized once, used briefly, then its whole arena is released.
    stream.zfree = [](void*, void*) {
    };
    if (inflateInit(&stream) != Z_OK) {
        return {ErrorCodes::BadValue, "Could not initialize zlib flags prefix decoder"};
    }
    ON_BLOCK_EXIT([&] { inflateEnd(&stream); });
    std::array<char, sizeof(uint32_t)> prefix{};
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    stream.avail_in = static_cast<uInt>(std::min<size_t>(input.length(), 256 * 1024));
    stream.next_out = reinterpret_cast<Bytef*>(prefix.data());
    stream.avail_out = static_cast<uInt>(prefix.size());
    for (size_t attempt = 0; attempt < 64; ++attempt) {
        const auto previousInput = stream.avail_in;
        const auto previousOutput = stream.avail_out;
        const auto result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            return {ErrorCodes::BadValue, "Could not decode zlib flags prefix"};
        }
        if (stream.avail_out == 0) {
            const auto flags = ConstDataView(prefix.data()).read<LittleEndian<uint32_t>>();
            if (OpMsg::hasUnknownRequiredFlags(flags)) {
                return {ErrorCodes::IllegalOpMsgFlag,
                        "zlib-compressed OP_MSG contains illegal flags"};
            }
            return flags;
        }
        if (result == Z_STREAM_END ||
            (stream.avail_in == previousInput && stream.avail_out == previousOutput)) {
            break;
        }
    }
    return {ErrorCodes::BadValue, "Truncated zlib flags prefix or prefix work limit exceeded"};
}

ZlibMessageCompressor::ZlibMessageCompressor() : MessageCompressorBase(MessageCompressor::kZlib) {}

std::size_t ZlibMessageCompressor::getMaxCompressedSize(size_t inputSize) {
    return ::compressBound(inputSize);
}

boost::optional<std::size_t> ZlibMessageCompressor::getMaxDecompressedSize(ConstDataRange input) {
    return boost::none;
}

StatusWith<std::size_t> ZlibMessageCompressor::doCompressData(ConstDataRange input,
                                                              DataRange output) {
    size_t outLength = output.length();
    int ret = ::compress2(const_cast<Bytef*>(reinterpret_cast<const Bytef*>(output.data())),
                          reinterpret_cast<uLongf*>(&outLength),
                          reinterpret_cast<const Bytef*>(input.data()),
                          input.length(),
                          Z_DEFAULT_COMPRESSION);

    if (ret != Z_OK) {
        return Status{ErrorCodes::BadValue, "Could not compress input"};
    }
    return {outLength};
}

StatusWith<std::size_t> ZlibMessageCompressor::doDecompressData(ConstDataRange input,
                                                                DataRange output) {
    if (input.length() < 8)  // header + minimal data + checksum
        return Status{ErrorCodes::BadValue, "Compressed data too small"};
    // Per RFC1950, validate first two bytes: CMF, FLG.
    uint8_t cmf = input.data()[0];
    uint8_t flg = input.data()[1];
    // `CM` (compression method) is `CMF[0:3]`.
    if ((cmf & 0xf) != Z_DEFLATED)
        return Status{ErrorCodes::BadValue, "zlib compression method != Z_DEFLATED"};
    // `FCHECK` is `FLG[0:4]`:
    //     The FCHECK value must be such that CMF and FLG, when viewed as
    //     a 16-bit unsigned integer stored in MSB order (CMF*256 + FLG),
    //     is a multiple of 31.
    if ((cmf * 256 + flg) % 31)
        return Status{ErrorCodes::BadValue, "zlib header checksum fail"};

    uLongf length = output.length();
    int ret = ::uncompress(const_cast<Bytef*>(reinterpret_cast<const Bytef*>(output.data())),
                           &length,
                           reinterpret_cast<const Bytef*>(input.data()),
                           input.length());

    if (ret != Z_OK) {
        return Status{ErrorCodes::BadValue, "Compressed message was invalid or corrupted"};
    }

    return {length};
}


MONGO_INITIALIZER_GENERAL(ZlibMessageCompressorInit,
                          ("EndStartupOptionHandling"),
                          ("AllCompressorsRegistered"))
(InitializerContext* context) {
    auto& compressorRegistry = MessageCompressorRegistry::get();
    compressorRegistry.registerImplementation(std::make_unique<ZlibMessageCompressor>());
}
}  // namespace mongo
