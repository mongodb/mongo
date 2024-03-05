/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/util/bsoncolumn_interleaved.h"

namespace mongo::bsoncolumn {

BlockBasedInterleavedDecompressor::BlockBasedInterleavedDecompressor(ElementStorage& allocator,
                                                                     const char* control,
                                                                     const char* end)
    : _allocator(allocator),
      _control(control),
      _end(end),
      _rootType(*control == bsoncolumn::kInterleavedStartArrayRootControlByte ? Array : Object),
      _traverseArrays(*control == bsoncolumn::kInterleavedStartControlByte ||
                      *control == bsoncolumn::kInterleavedStartArrayRootControlByte) {
    invariant(bsoncolumn::isInterleavedStartControlByte(*control),
              "request to do interleaved decompression on non-interleaved data");
}

/**
 * Given an element that is being materialized as part of a sub-object, write it to the allocator as
 * a BSONElement with the appropriate field name.
 */
void BlockBasedInterleavedDecompressor::writeToElementStorage(DecodingState::Elem elem,
                                                              StringData fieldName) {
    visit(OverloadedVisitor{
              [&](BSONElement& bsonElem) {
                  if (!bsonElem.eoo()) {
                      ElementStorage::Element esElem =
                          _allocator.allocate(bsonElem.type(), fieldName, bsonElem.valuesize());
                      memcpy(esElem.value(), bsonElem.value(), bsonElem.valuesize());
                  }
              },
              [&](std::pair<BSONType, int64_t> elem) {
                  switch (elem.first) {
                      case NumberInt: {
                          ElementStorage::Element esElem =
                              _allocator.allocate(elem.first, fieldName, 4);
                          DataView(esElem.value()).write<LittleEndian<int32_t>>(elem.second);
                      } break;
                      case NumberLong: {
                          ElementStorage::Element esElem =
                              _allocator.allocate(elem.first, fieldName, 8);
                          DataView(esElem.value()).write<LittleEndian<int64_t>>(elem.second);
                      } break;
                      case Bool: {
                          ElementStorage::Element esElem =
                              _allocator.allocate(elem.first, fieldName, 1);
                          DataView(esElem.value()).write<LittleEndian<bool>>(elem.second);
                      } break;
                      default:
                          invariant(false, "attempt to materialize unsupported type");
                  }
              },
              [&](std::pair<BSONType, int128_t>) {
                  invariant(false, "tried to materialize a 128-bit type");
              },
          },
          elem);
}


/**
 * Initializes a decoder given an uncompressed BSONElement in the BSONColumn bytes.
 */
void BlockBasedInterleavedDecompressor::DecodingState::loadUncompressed(const BSONElement& elem) {
    BSONType type = elem.type();
    invariant(!uses128bit(type));
    invariant(!usesDeltaOfDelta(type));
    auto& d64 = decoder.template emplace<Decoder64>();
    switch (type) {
        case Bool:
            d64.lastEncodedValue = elem.boolean();
            break;
        case NumberInt:
            d64.lastEncodedValue = elem._numberInt();
            break;
        case NumberLong:
            d64.lastEncodedValue = elem._numberLong();
            break;
        default:
            invariant(false, "unsupported type");
    }

    _lastLiteral = elem;
}

/**
 * Assuming that buffer points at the next control byte, takes the appropriate action:
 * - If the control byte begins an uncompressed literal: initializes a decoder, and returns the
 *   literal.
 * - If the control byte precedes blocks of deltas, applies the first delta and returns the new
 *   expanded element. In both cases, the "size" field will contain the number of bytes to the next
 *   control byte.
 */
BlockBasedInterleavedDecompressor::DecodingState::LoadControlResult
BlockBasedInterleavedDecompressor::DecodingState::loadControl(ElementStorage& allocator,
                                                              const char* buffer) {
    uint8_t control = *buffer;
    if (isUncompressedLiteralControlByte(control)) {
        BSONElement literalElem(buffer, 1, -1);
        return {literalElem, literalElem.size()};
    }

    uint8_t blocks = numSimple8bBlocksForControlByte(control);
    int size = sizeof(uint64_t) * blocks;

    auto& d64 = std::get<DecodingState::Decoder64>(decoder);
    // We can read the last known value from the decoder iterator even as it has
    // reached end.
    boost::optional<uint64_t> lastSimple8bValue = d64.pos.valid() ? *d64.pos : 0;
    d64.pos = Simple8b<uint64_t>(buffer + 1, size, lastSimple8bValue).begin();
    Elem deltaElem = loadDelta(allocator, d64);
    return LoadControlResult{deltaElem, size + 1};
}

/**
 * Apply a delta to an encoded representation to get a new element value. May also apply a 0 delta
 * to an uncompressed literal, simply returning the literal.
 */
BlockBasedInterleavedDecompressor::DecodingState::Elem
BlockBasedInterleavedDecompressor::DecodingState::loadDelta(ElementStorage& allocator,
                                                            Decoder64& d64) {
    invariant(d64.pos.valid());
    const auto& delta = *d64.pos;
    if (!delta) {
        // boost::none represents skip, just return an EOO BSONElement.
        return BSONElement{};
    }

    // Note: delta-of-delta not handled here yet.
    if (*delta == 0) {
        // If we have an encoded representation of the last value, return it.
        if (d64.lastEncodedValue) {
            return std::pair{_lastLiteral.type(), *d64.lastEncodedValue};
        }
        // Otherwise return the last uncompressed value we found.
        return _lastLiteral;
    }

    uassert(8625729,
            "attempt to expand delta for type that does not have encoded representation",
            d64.lastEncodedValue);
    d64.lastEncodedValue =
        expandDelta(*d64.lastEncodedValue, Simple8bTypeUtil::decodeInt64(*delta));

    return std::pair{_lastLiteral.type(), *d64.lastEncodedValue};
}

}  // namespace mongo::bsoncolumn
