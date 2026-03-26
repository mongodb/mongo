/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/column/bsoncolumn_expressions.h"

#include "mongo/bson/bsontypes.h"

namespace mongo::bsoncolumn {
namespace {
const char* countImpl(size_t& outCount, const char* pos, const char* e) {
    // Iterate over binary. We should never reach the end for a valid binary, the exit condition for
    // the loop is when the end sentinel is encountered.
    while (pos <= e) {
        uint8_t control = *pos;

        // Null control byte is end sentinel, we are done.
        if (control == 0) {
            return pos + 1;
        }

        // Uncompressed element, increment count and skip over it.
        if (bsoncolumn::isUncompressedLiteralControlByte(control)) {
            // Load BSONElement from the literal so we can determine its size.
            BSONElement elem(pos, 1, BSONElement::TrustedInitTag{});
            // Uncompressed element is 1 count. Advance pointer past this element.
            ++outCount;
            pos += elem.size();
            continue;
        }

        // Interleaved mode. To calculate the count we do not need to associate every control byte
        // with the correct substream. We can rather use the exact same implementation for count
        // outside of interleaved mode but divide the count result from interleaved mode by the
        // number of interleaved streams. This is faster than maintaining a heap which is needed for
        // true decompression, the tradeoff here is that this function will not be able to detect
        // all forms of invalid BSONColumn. This is acceptible for the intended use case of this
        // function.
        if (bsoncolumn::isInterleavedStartControlByte(control)) {
            // Obtain reference object and calculate number of streams.
            BSONObj interleavedReferenceObj = BSONObj(++pos);
            // This function will never return zero, not even for invalid BSONColumn.
            uint32_t streams = numInterleavedStreams(interleavedReferenceObj, control);
            // Advance the binary past the reference object, from now on the position is pointing at
            // control bytes which we can treat as regular BSONColumn without considering
            // interleaved mode.
            pos += interleavedReferenceObj.objsize();

            // Recursively calculate the count within interleaved mode as-if we're not in
            // interleaved mode. We do not have to consider nested interleaved mode as this is an
            // invalid encoding that is checked by BSON validate.
            size_t interleavedCount = 0;
            pos = countImpl(interleavedCount, pos, e);

            // Report correct count within interleaved mode.
            uassert(7667504, "Invalid BSON Column encoding", interleavedCount % streams == 0);
            outCount += interleavedCount / streams;
            continue;
        }

        // Regular case with simple8b blocks.
        uint8_t blocks = bsoncolumn::numSimple8bBlocksForControlByte(control);
        uassert(7667503, "Invalid BSON Column encoding", pos + sizeof(uint64_t) * blocks + 1 < e);
        ++pos;

        // Calculate number of elements in this control block
        outCount += simple8b::count(pos, blocks * sizeof(uint64_t));
        // Advance pointer past the block
        pos += blocks * sizeof(uint64_t);
    }
    // We should not exit the loop due to buffer exhaustion for valid BSONColumn binaries.
    uasserted(7667502, "Invalid BSON Column encoding");
}
}  // namespace


size_t count(const char* buffer, size_t size) {
    const char* e = buffer + size;
    size_t cnt = 0;
    const char* pos = countImpl(cnt, buffer, e);
    uassert(7667501, "Invalid BSON Column encoding", pos == e);
    return cnt;
}

size_t count(BSONBinData bin) {
    tassert(7667500, "Invalid BSON type for column", bin.type == BinDataType::Column);
    return count(reinterpret_cast<const char*>(bin.data), bin.length);
}
}  // namespace mongo::bsoncolumn
