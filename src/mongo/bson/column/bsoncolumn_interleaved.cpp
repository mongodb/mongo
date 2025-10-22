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

#include "mongo/bson/column/bsoncolumn_interleaved.h"

namespace mongo::bsoncolumn {

BlockBasedInterleavedDecompressor::BlockBasedInterleavedDecompressor(BSONElementStorage& allocator,
                                                                     const char* control,
                                                                     const char* end)
    : _allocator(allocator),
      _control(control),
      _end(end),
      _rootType(*control == bsoncolumn::kInterleavedStartArrayRootControlByte ? BSONType::array
                                                                              : BSONType::object),
      _traverseArrays(*control == bsoncolumn::kInterleavedStartControlByte ||
                      *control == bsoncolumn::kInterleavedStartArrayRootControlByte) {
    invariant(bsoncolumn::isInterleavedStartControlByte(*control),
              "request to do interleaved decompression on non-interleaved data");
}

void BlockBasedInterleavedDecompressor::DecodingState::Decoder64::writeToElementStorage(
    BSONElementStorage& allocator,
    BSONType type,
    int64_t value,
    BSONElement lastLiteral,
    StringData fieldName) const {
    switch (type) {
        case BSONType::numberInt: {
            BSONElementStorage::Element esElem = allocator.allocate(type, fieldName, 4);
            DataView(esElem.value()).write<LittleEndian<int32_t>>(value);
        } break;
        case BSONType::timestamp:
        case BSONType::date:
        case BSONType::numberLong: {
            BSONElementStorage::Element esElem = allocator.allocate(type, fieldName, 8);
            DataView(esElem.value()).write<LittleEndian<int64_t>>(value);
        } break;
        case BSONType::boolean: {
            BSONElementStorage::Element esElem = allocator.allocate(type, fieldName, 1);
            DataView(esElem.value()).write<LittleEndian<bool>>(value);
        } break;
        case BSONType::oid: {
            BSONElementStorage::Element esElem = allocator.allocate(type, fieldName, 12);
            Simple8bTypeUtil::decodeObjectIdInto(
                esElem.value(), value, lastLiteral.__oid().getInstanceUnique());
        } break;
        case BSONType::numberDouble: {
            BSONElementStorage::Element esElem = allocator.allocate(type, fieldName, 8);
            DataView(esElem.value())
                .write<LittleEndian<double>>(Simple8bTypeUtil::decodeDouble(value, scaleIndex));
        } break;
        default:
            uassert(8784700, "attempt to materialize unsupported type", false);
    }
}

void BlockBasedInterleavedDecompressor::DecodingState::Decoder128::writeToElementStorage(
    BSONElementStorage& allocator,
    BSONType type,
    int128_t value,
    BSONElement lastLiteral,
    StringData fieldName) const {
    switch (type) {
        case BSONType::string:
        case BSONType::code: {
            Simple8bTypeUtil::SmallString ss = Simple8bTypeUtil::decodeString(value);
            // Add 5 bytes to size, strings begin with a 4 byte count and ends with a
            // null terminator
            BSONElementStorage::Element esElem = allocator.allocate(type, fieldName, ss.size + 5);
            // Write count, size includes null terminator
            DataView(esElem.value()).write<LittleEndian<int32_t>>(ss.size + 1);
            // Write string value
            memcpy(esElem.value() + sizeof(int32_t), ss.str.data(), ss.size);
            // Write null terminator
            DataView(esElem.value()).write<char>('\0', ss.size + sizeof(int32_t));
        } break;
        case BSONType::numberDecimal: {
            BSONElementStorage::Element esElem = allocator.allocate(type, fieldName, 16);
            Decimal128 dec128 = Simple8bTypeUtil::decodeDecimal128(value);
            Decimal128::Value dec128Val = dec128.getValue();
            DataView(esElem.value()).write<LittleEndian<long long>>(dec128Val.low64);
            DataView(esElem.value() + sizeof(long long))
                .write<LittleEndian<long long>>(dec128Val.high64);
        } break;
        case BSONType::binData: {
            // Layout of a binary element:
            // - 4-byte length of binary data
            // - 1-byte binary subtype
            // - The binary data
            BSONElementStorage::Element esElem =
                allocator.allocate(type, fieldName, lastLiteral.valuesize());
            // The first 5 bytes in binData is a count and subType, copy them from
            // previous
            memcpy(esElem.value(), lastLiteral.value(), 5);
            uassert(8690003,
                    "BinData length should not exceed 16 in a delta encoding",
                    lastLiteral.valuestrsize() <= 16);
            Simple8bTypeUtil::decodeBinary(value, esElem.value() + 5, lastLiteral.valuestrsize());
        } break;
        default:
            uassert(8784701, "attempt to materialize unsupported type", false);
    }
}

/**
 * Given an element that is being materialized as part of a sub-object, write it to the allocator as
 * a BSONElement with the appropriate field name.
 */
void BlockBasedInterleavedDecompressor::writeToElementStorage(BSONElement bsonElem,
                                                              StringData fieldName) {
    if (!bsonElem.eoo()) {
        BSONElementStorage::Element esElem =
            _allocator.allocate(bsonElem.type(), fieldName, bsonElem.valuesize());
        memcpy(esElem.value(), bsonElem.value(), bsonElem.valuesize());
    }
}


/**
 * Initializes a decoder given an uncompressed BSONElement in the BSONColumn bytes.
 */
void BlockBasedInterleavedDecompressor::DecodingState::loadUncompressed(const BSONElement& elem) {
    BSONType type = elem.type();
    if (uses128bit(type)) {
        auto& d128 = decoder.template emplace<Decoder128>();
        switch (type) {
            case BSONType::string:
            case BSONType::code:
                d128.lastEncodedValue = Simple8bTypeUtil::encodeString(elem.valueStringData());
                break;
            case BSONType::binData: {
                int size;
                const char* binary = elem.binData(size);
                d128.lastEncodedValue = Simple8bTypeUtil::encodeBinary(binary, size);
                break;
            }
            case BSONType::numberDecimal:
                d128.lastEncodedValue = Simple8bTypeUtil::encodeDecimal128(elem._numberDecimal());
                break;
            default:
                MONGO_UNREACHABLE;
        }
    } else {
        auto& d64 = decoder.template emplace<Decoder64>();
        d64.deltaOfDelta = usesDeltaOfDelta(type);
        switch (type) {
            case BSONType::oid:
                d64.lastEncodedValue = Simple8bTypeUtil::encodeObjectId(elem.__oid());
                break;
            case BSONType::date:
                d64.lastEncodedValue = elem.date().toMillisSinceEpoch();
                break;
            case BSONType::boolean:
                d64.lastEncodedValue = elem.boolean();
                break;
            case BSONType::numberInt:
                d64.lastEncodedValue = elem._numberInt();
                break;
            case BSONType::numberLong:
                d64.lastEncodedValue = elem._numberLong();
                break;
            case BSONType::numberDouble:
                // We don't have an encoding for doubles until we get the scale index from the next
                // delta control byte.
                d64.lastEncodedValue = boost::none;
                break;
            case BSONType::timestamp:
                d64.lastEncodedValue = elem.timestampValue();
                break;
            default:
                // Not all types have an encoded version.
                break;
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
BlockBasedInterleavedDecompressor::DecodingState::loadControl(BSONElementStorage& allocator,
                                                              const char* buffer) {
    uint8_t control = *buffer;
    if (isUncompressedLiteralControlByte(control)) {
        BSONElement literalElem(buffer, 1, BSONElement::TrustedInitTag{});
        loadUncompressed(literalElem);
        return {literalElem, literalElem.size()};
    }

    uint8_t blocks = numSimple8bBlocksForControlByte(control);
    int size = sizeof(uint64_t) * blocks;

    Elem deltaElem;
    visit(OverloadedVisitor{
              [&](DecodingState::Decoder64& d64) {
                  // Simple-8b delta block, load its scale factor and validate for sanity
                  uint8_t newScaleIndex = bsoncolumn::scaleIndexForControlByte(control);
                  uassert(8690002,
                          "Invalid control byte in BSON Column",
                          newScaleIndex != bsoncolumn::kInvalidScaleIndex);

                  // If Double, scale last value according to this scale factor
                  auto type = _lastLiteral.type();
                  if (type == BSONType::numberDouble) {
                      // Get the current double value, decoding with the old scale index if needed
                      double val = d64.lastEncodedValue
                          ? Simple8bTypeUtil::decodeDouble(*d64.lastEncodedValue, d64.scaleIndex)
                          : _lastLiteral.Double();

                      auto encoded = Simple8bTypeUtil::encodeDouble(val, newScaleIndex);
                      uassert(8690001, "Invalid double encoding in BSON Column", encoded);
                      d64.lastEncodedValue = *encoded;
                  } else {
                      uassert(8915500,
                              "Unexpected control for type in BSONColumn",
                              newScaleIndex == Simple8bTypeUtil::kMemoryAsInteger);
                  }

                  d64.scaleIndex = newScaleIndex;

                  // We can read the last known value from the decoder iterator even as it has
                  // reached end.
                  boost::optional<uint64_t> lastSimple8bValue = d64.pos.valid() ? *d64.pos : 0;
                  d64.pos = Simple8b<uint64_t>(buffer + 1, size, lastSimple8bValue).begin();
                  deltaElem = loadDelta(allocator, d64);
                  ++d64.pos;
              },
              [&](DecodingState::Decoder128& d128) {
                  // We can read the last known value from the decoder iterator even as it has
                  // reached end.
                  uassert(8915501,
                          "Invalid control byte in BSON Column",
                          bsoncolumn::scaleIndexForControlByte(control) ==
                              Simple8bTypeUtil::kMemoryAsInteger);

                  boost::optional<uint128_t> lastSimple8bValue =
                      d128.pos.valid() ? *d128.pos : uint128_t(0);
                  d128.pos = Simple8b<uint128_t>(buffer + 1, size, lastSimple8bValue).begin();
                  deltaElem = loadDelta(allocator, d128);
                  ++d128.pos;
              }},
          decoder);
    return LoadControlResult{deltaElem, size + 1};
}

/**
 * Apply a delta to an encoded representation to get a new element value. May also apply a 0 delta
 * to an uncompressed literal, simply returning the literal.
 */
BlockBasedInterleavedDecompressor::DecodingState::Elem
BlockBasedInterleavedDecompressor::DecodingState::loadDelta(BSONElementStorage& allocator,
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
BlockBasedInterleavedDecompressor::DecodingState::loadDelta(BSONElementStorage& allocator,
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

    // 'String' and 'Code' can have unencodable values that are followed by non-zero deltas.
    uassert(8690000,
            "attempt to expand delta for type that does not have encoded representation",
            d128.lastEncodedValue || _lastLiteral.type() == BSONType::string ||
                _lastLiteral.type() == BSONType::code);

    // Expand delta as last encoded. If the last value is unencodable it will be set to 0.
    d128.lastEncodedValue =
        expandDelta(d128.lastEncodedValue.value_or(0), Simple8bTypeUtil::decodeInt128(*delta));
    return std::pair{_lastLiteral.type(), *d128.lastEncodedValue};
}

}  // namespace mongo::bsoncolumn
