// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range_cursor.h"

#include <cstddef>
#include <cstdint>

namespace mongo {

/**
 * Simple struct that helps parsing op_compressed header.
 *
 * Should be replaced by a proper MsgCompressionHeader::View and MsgCompressionHeader::ConstView
 * class just like message.h does.
 *
 * More about OP_COMPRESSED can be found in the documentation:
 * https://github.com/mongodb/specifications/blob/master/source/compression/OP_COMPRESSED.md
 *
 * TODO: SERVER-106196 Replace this struct with a View/ConstView class
 */
struct CompressionHeader {
    std::int32_t originalOpCode;
    std::int32_t uncompressedSize;
    std::uint8_t compressorId;

    void serialize(DataRangeCursor* cursor) {
        cursor->writeAndAdvance<LittleEndian<std::int32_t>>(originalOpCode);
        cursor->writeAndAdvance<LittleEndian<std::int32_t>>(uncompressedSize);
        cursor->writeAndAdvance<LittleEndian<std::uint8_t>>(compressorId);
    }

    CompressionHeader(std::int32_t _opcode, std::int32_t _size, std::uint8_t _id)
        : originalOpCode{_opcode}, uncompressedSize{_size}, compressorId{_id} {}

    CompressionHeader(ConstDataRangeCursor* cursor) {
        originalOpCode = cursor->readAndAdvance<LittleEndian<std::int32_t>>();
        uncompressedSize = cursor->readAndAdvance<LittleEndian<std::int32_t>>();
        compressorId = cursor->readAndAdvance<LittleEndian<std::uint8_t>>();
    }

    static constexpr std::size_t size() {
        return sizeof(originalOpCode) + sizeof(uncompressedSize) + sizeof(compressorId);
    }
};

}  // namespace mongo
