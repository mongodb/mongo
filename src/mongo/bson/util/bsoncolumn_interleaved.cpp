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
                                                              BSONElement lastLiteral,
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
                      case Date:
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
                      case jstOID: {
                          ElementStorage::Element esElem =
                              _allocator.allocate(elem.first, fieldName, 12);
                          Simple8bTypeUtil::decodeObjectIdInto(
                              esElem.value(), elem.second, lastLiteral.__oid().getInstanceUnique());
                      } break;
                      case bsonTimestamp: {
                          ElementStorage::Element esElem =
                              _allocator.allocate(elem.first, fieldName, 8);
                          DataView(esElem.value()).write<LittleEndian<long long>>(elem.second);
                      } break;
                      default:
                          invariant(false, "attempt to materialize unsupported type");
                  }
              },
              [&](std::pair<BSONType, int128_t> elem) {
                  switch (elem.first) {
                      case String:
                      case Code: {
                          Simple8bTypeUtil::SmallString ss =
                              Simple8bTypeUtil::decodeString(elem.second);
                          // Add 5 bytes to size, strings begin with a 4 byte count and ends with a
                          // null terminator
                          ElementStorage::Element esElem =
                              _allocator.allocate(elem.first, fieldName, ss.size + 5);
                          // Write count, size includes null terminator
                          DataView(esElem.value()).write<LittleEndian<int32_t>>(ss.size + 1);
                          // Write string value
                          memcpy(esElem.value() + sizeof(int32_t), ss.str.data(), ss.size);
                          // Write null terminator
                          DataView(esElem.value()).write<char>('\0', ss.size + sizeof(int32_t));
                      } break;
                      case NumberDecimal: {
                          ElementStorage::Element esElem =
                              _allocator.allocate(elem.first, fieldName, 16);
                          Decimal128 dec128 = Simple8bTypeUtil::decodeDecimal128(elem.second);
                          Decimal128::Value dec128Val = dec128.getValue();
                          DataView(esElem.value()).write<LittleEndian<long long>>(dec128Val.low64);
                          DataView(esElem.value() + sizeof(long long))
                              .write<LittleEndian<long long>>(dec128Val.high64);
                      } break;
                      case BinData: {
                          // Layout of a binary element:
                          // - 4-byte length of binary data
                          // - 1-byte binary subtype
                          // - The binary data
                          ElementStorage::Element esElem =
                              _allocator.allocate(elem.first, fieldName, lastLiteral.valuesize());
                          // The first 5 bytes in binData is a count and subType, copy them from
                          // previous
                          memcpy(esElem.value(), lastLiteral.value(), 5);
                          uassert(8690003,
                                  "BinData length should not exceed 16 in a delta encoding",
                                  lastLiteral.valuestrsize() <= 16);
                          Simple8bTypeUtil::decodeBinary(
                              elem.second, esElem.value() + 5, lastLiteral.valuestrsize());
                      } break;
                      default:
                          invariant(false, "attempted to materialize unsupported type");
                  }
              },
          },
          elem);
}


/**
 * Initializes a decoder given an uncompressed BSONElement in the BSONColumn bytes.
 */
void BlockBasedInterleavedDecompressor::DecodingState::loadUncompressed(const BSONElement& elem) {
    BSONType type = elem.type();
    if (uses128bit(type)) {
        auto& d128 = decoder.template emplace<Decoder128>();
        switch (type) {
            case String:
            case Code:
                d128.lastEncodedValue =
                    Simple8bTypeUtil::encodeString(elem.valueStringData()).value_or(0);
                break;
            case BinData: {
                int size;
                const char* binary = elem.binData(size);
                d128.lastEncodedValue = Simple8bTypeUtil::encodeBinary(binary, size).value_or(0);
                break;
            }
            case NumberDecimal:
                d128.lastEncodedValue = Simple8bTypeUtil::encodeDecimal128(elem._numberDecimal());
                break;
            default:
                invariant(false, "unsupported type");
        }
    } else {
        auto& d64 = decoder.template emplace<Decoder64>();
        d64.deltaOfDelta = usesDeltaOfDelta(type);
        switch (type) {
            case jstOID:
                d64.lastEncodedValue = Simple8bTypeUtil::encodeObjectId(elem.__oid());
                break;
            case Date:
                d64.lastEncodedValue = elem.date().toMillisSinceEpoch();
                break;
            case Bool:
                d64.lastEncodedValue = elem.boolean();
                break;
            case NumberInt:
                d64.lastEncodedValue = elem._numberInt();
                break;
            case NumberLong:
                d64.lastEncodedValue = elem._numberLong();
                break;
            case bsonTimestamp:
                d64.lastEncodedValue = elem.timestampValue();
                break;
            default:
                invariant(false, "unsupported type");
        }
        if (d64.deltaOfDelta) {
            d64.lastEncodedValueForDeltaOfDelta = d64.lastEncodedValue.get();
            d64.lastEncodedValue = 0;
        }
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

    Elem deltaElem;
    visit(OverloadedVisitor{
              [&](DecodingState::Decoder64& d64) {
                  // Simple-8b delta block, load its scale factor and validate for sanity
                  d64.scaleIndex = bsoncolumn::scaleIndexForControlByte(control);
                  uassert(8690002,
                          "Invalid control byte in BSON Column",
                          d64.scaleIndex != bsoncolumn::kInvalidScaleIndex);
                  // If Double, scale last value according to this scale factor
                  auto type = _lastLiteral.type();
                  if (type == NumberDouble) {
                      auto encoded = Simple8bTypeUtil::encodeDouble(_lastLiteral._numberDouble(),
                                                                    d64.scaleIndex);
                      uassert(8690001, "Invalid double encoding in BSON Column", encoded);
                      d64.lastEncodedValue = *encoded;
                  }
                  // We can read the last known value from the decoder iterator even as it has
                  // reached end.
                  boost::optional<uint64_t> lastSimple8bValue = d64.pos.valid() ? *d64.pos : 0;
                  d64.pos = Simple8b<uint64_t>(buffer + 1, size, lastSimple8bValue).begin();
                  deltaElem = loadDelta(allocator, d64);
              },
              [&](DecodingState::Decoder128& d128) {
                  // We can read the last known value from the decoder iterator even as it has
                  // reached end.
                  boost::optional<uint128_t> lastSimple8bValue =
                      d128.pos.valid() ? *d128.pos : uint128_t(0);
                  d128.pos = Simple8b<uint128_t>(buffer + 1, size, lastSimple8bValue).begin();
                  deltaElem = loadDelta(allocator, d128);
              }},
          decoder);
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

    if (!d64.deltaOfDelta && *delta == 0) {
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

    // Expand delta or delta-of-delta as last encoded.
    d64.lastEncodedValue =
        expandDelta(*d64.lastEncodedValue, Simple8bTypeUtil::decodeInt64(*delta));
    if (d64.deltaOfDelta) {
        d64.lastEncodedValueForDeltaOfDelta =
            expandDelta(d64.lastEncodedValueForDeltaOfDelta, *d64.lastEncodedValue);
        return std::pair{_lastLiteral.type(), d64.lastEncodedValueForDeltaOfDelta};
    }

    return std::pair{_lastLiteral.type(), *d64.lastEncodedValue};
}

BlockBasedInterleavedDecompressor::DecodingState::Elem
BlockBasedInterleavedDecompressor::DecodingState::loadDelta(ElementStorage& allocator,
                                                            Decoder128& d128) {
    invariant(d128.pos.valid());
    const auto& delta = *d128.pos;

    if (!delta) {
        return BSONElement();
    }

    // If we have a zero delta no need to allocate a new Element, we can just use previous.
    if (*delta == 0) {
        // If we have an encoded representation of the last value, return it.
        if (d128.lastEncodedValue) {
            return std::pair{_lastLiteral.type(), *d128.lastEncodedValue};
        }
        // Otherwise return the last uncompressed value we found.
        return _lastLiteral;
    }

    uassert(8690000,
            "attempt to expand delta for type that does not have encoded representation",
            d128.lastEncodedValue);

    // Expand delta as last encoded.
    d128.lastEncodedValue =
        expandDelta(*d128.lastEncodedValue, Simple8bTypeUtil::decodeInt128(*delta));
    return std::pair{_lastLiteral.type(), *d128.lastEncodedValue};
}

}  // namespace mongo::bsoncolumn
