/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
