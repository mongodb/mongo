/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/util/bsoncolumnbuilder.h"
#include "mongo/bson/util/bsoncolumn_util.h"

#include "mongo/bson/util/simple8b_type_util.h"

#include <memory>

namespace mongo {
using namespace bsoncolumn;

namespace {
static constexpr uint8_t kMaxCount = 16;
static constexpr uint8_t kCountMask = 0x0F;
static constexpr uint8_t kControlMask = 0xF0;

static constexpr std::array<uint8_t, Simple8bTypeUtil::kMemoryAsInteger + 1>
    kControlByteForScaleIndex = {0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0x80};

// Encodes the double with the lowest possible scale index. In worst case we will interpret the
// memory as integer which is guaranteed to succeed.
std::pair<int64_t, uint8_t> scaleAndEncodeDouble(double value, uint8_t minScaleIndex) {
    boost::optional<int64_t> encoded;
    for (; !encoded; ++minScaleIndex) {
        encoded = Simple8bTypeUtil::encodeDouble(value, minScaleIndex);
    }

    // Subtract the last scale that was added in the loop before returning
    return {*encoded, minScaleIndex - 1};
}

}  // namespace

BSONColumnBuilder::BSONColumnBuilder(StringData fieldName)
    : _simple8bBuilder64(_createBufferWriter()),
      _simple8bBuilder128(_createBufferWriter()),
      _scaleIndex(Simple8bTypeUtil::kMemoryAsInteger),
      _fieldName(fieldName) {
    // Leave space for element count at the beginning
    static_assert(sizeof(_elementCount) == kElementCountBytes,
                  "Element count for BSONColumn should be 4 bytes");
    _bufBuilder.skip(kElementCountBytes);
    // Store EOO type with empty field name as previous.
    _storePrevious(BSONElement());
}

BSONElement BSONColumnBuilder::_previous() const {
    return {_prev.get(), 1, _prevSize, BSONElement::CachedSizeTag{}};
}

BSONColumnBuilder& BSONColumnBuilder::append(BSONElement elem) {
    auto type = elem.type();
    uassert(ErrorCodes::InvalidBSONType,
            "MinKey or MaxKey is not supported by BSON Column (subtype 7)",
            type != MinKey && type != MaxKey);

    auto previous = _previous();
    ++_elementCount;

    // If we detect a type change (or this is first value). Flush all pending values in Simple-8b
    // and write uncompressed literal. Reset all default values.
    if (previous.type() != elem.type()) {
        _storePrevious(elem);
        _simple8bBuilder128.flush();
        _simple8bBuilder64.flush();
        _storeWith128 = uses128bit(type);
        _writeLiteralFromPrevious();
        return *this;
    }

    // Store delta in Simple-8b if types match
    bool compressed = !usesDeltaOfDelta(type) && elem.binaryEqualValues(previous);
    if (compressed) {
        if (_storeWith128) {
            _simple8bBuilder128.append(0);
        } else {
            _simple8bBuilder64.append(0);
        }
    }

    if (!compressed) {
        if (_storeWith128) {
            auto appendEncoded = [&](int128_t encoded) {
                compressed = _simple8bBuilder128.append(
                    Simple8bTypeUtil::encodeInt128(calcDelta(encoded, _prevEncoded128)));
                _prevEncoded128 = encoded;
            };

            switch (type) {
                case String:
                    if (auto encoded = Simple8bTypeUtil::encodeString(elem.valueStringData())) {
                        appendEncoded(*encoded);
                    }
                    break;
                case BinData: {
                    int size;
                    const char* binary = elem.binData(size);
                    // We only do delta encoding of binary if the binary size is exactly the same.
                    // To support size difference we'd need to add a count to be able to reconstruct
                    // binaries starting with zero bytes. We don't want to waste bits for this.
                    if (size != previous.valuestrsize())
                        break;

                    if (auto encoded = Simple8bTypeUtil::encodeBinary(binary, size)) {
                        appendEncoded(*encoded);
                    }
                } break;
                case NumberDecimal:
                    appendEncoded(Simple8bTypeUtil::encodeDecimal128(elem._numberDecimal()));
                    break;
                default:
                    MONGO_UNREACHABLE;
            };
        } else if (type == NumberDouble) {
            compressed = _appendDouble(elem._numberDouble(), previous._numberDouble());
        } else {
            // Variable to indicate that it was possible to encode this BSONElement as an integer
            // for storage inside Simple8b. If encoding is not possible the element is stored as
            // uncompressed.
            bool encodingPossible = true;
            // Value to store in Simple8b if encoding is possible.
            int64_t value = 0;
            switch (type) {
                case NumberInt:
                    value = calcDelta(elem._numberInt(), previous._numberInt());
                    break;
                case NumberLong:
                    value = calcDelta(elem._numberLong(), previous._numberLong());
                    break;
                case jstOID: {
                    encodingPossible = _objectIdDeltaPossible(elem, previous);
                    if (!encodingPossible)
                        break;

                    int64_t curEncoded = Simple8bTypeUtil::encodeObjectId(elem.OID());
                    value = calcDelta(curEncoded, _prevEncoded64);
                    _prevEncoded64 = curEncoded;
                    break;
                }
                case bsonTimestamp: {
                    int64_t currTimestampDelta =
                        calcDelta(elem.timestamp().asULL(), previous.timestamp().asULL());
                    value = calcDelta(currTimestampDelta, _prevDelta);
                    _prevDelta = currTimestampDelta;
                    break;
                }
                case Date:
                    value = calcDelta(elem.date().toMillisSinceEpoch(),
                                      previous.date().toMillisSinceEpoch());
                    break;
                case Bool:
                    value = calcDelta(elem.boolean(), previous.boolean());
                    break;
                case Undefined:
                case jstNULL:
                    value = 0;
                    break;
                case RegEx:
                case DBRef:
                case CodeWScope:
                case Symbol:
                case Object:
                case Array:
                    encodingPossible = false;
                    break;
                default:
                    MONGO_UNREACHABLE;
            };
            if (encodingPossible) {
                compressed = _simple8bBuilder64.append(Simple8bTypeUtil::encodeInt64(value));
            }
        }
    }
    _storePrevious(elem);

    // Store uncompressed literal if value is outside of range of encodable values.
    if (!compressed) {
        _simple8bBuilder128.flush();
        _simple8bBuilder64.flush();
        _writeLiteralFromPrevious();
    }

    return *this;
}

boost::optional<Simple8bBuilder<uint64_t>> BSONColumnBuilder::_tryRescalePending(
    int64_t encoded, uint8_t newScaleIndex) {
    // Encode last value in the previous block with old and new scale index. We know that scaling
    // with the old index is possible.
    int64_t prev = *Simple8bTypeUtil::encodeDouble(_lastValueInPrevBlock, _scaleIndex);
    boost::optional<int64_t> prevRescaled =
        Simple8bTypeUtil::encodeDouble(_lastValueInPrevBlock, newScaleIndex);

    // Fail if we could not rescale
    bool possible = prevRescaled.has_value();
    if (!possible)
        return boost::none;

    // Create a new Simple8bBuilder for the rescaled values. If any Simple8b block is finalized when
    // adding the new values then rescaling is less optimal than flushing with the current scale. So
    // we just record if this happens in our write callback.
    Simple8bBuilder<uint64_t> builder([&possible](uint64_t block) { possible = false; });

    // Iterate over our pending values, decode them back into double, rescale and append to our new
    // Simple8b builder
    for (const auto& pending : _simple8bBuilder64) {
        if (!pending) {
            builder.skip();
            continue;
        }

        // Apply delta to previous, decode to double and rescale
        prev = expandDelta(prev, Simple8bTypeUtil::decodeInt64(*pending));
        auto rescaled = Simple8bTypeUtil::encodeDouble(
            Simple8bTypeUtil::decodeDouble(prev, _scaleIndex), newScaleIndex);

        // Fail if we could not rescale
        if (!rescaled || !prevRescaled)
            return boost::none;

        // Append the scaled delta
        auto appended =
            builder.append(Simple8bTypeUtil::encodeInt64(calcDelta(*rescaled, *prevRescaled)));

        // Fail if are out of range for Simple8b or a block was written
        if (!appended || !possible)
            return boost::none;

        // Remember previous for next value
        prevRescaled = rescaled;
    }

    // Last add our new value
    auto appended =
        builder.append(Simple8bTypeUtil::encodeInt64(calcDelta(encoded, *prevRescaled)));
    if (!appended || !possible)
        return boost::none;

    // We managed to add all re-scaled values, this will thus compress better. Set write callback to
    // our buffer writer and return
    builder.setWriteCallback(_createBufferWriter());
    return builder;
}

bool BSONColumnBuilder::_appendDouble(double value, double previous) {
    // Scale with lowest possible scale index
    auto [encoded, scaleIndex] = scaleAndEncodeDouble(value, _scaleIndex);

    if (scaleIndex != _scaleIndex) {
        // New value need higher scale index. We have two choices:
        // (1) Re-scale pending values to use this larger scale factor
        // (2) Flush pending and start a new block with this higher scale factor
        // We try both options and select the one that compresses best
        auto rescaled = _tryRescalePending(encoded, scaleIndex);
        if (rescaled) {
            // Re-scale possible, use this Simple8b builder
            std::swap(_simple8bBuilder64, *rescaled);
            _prevEncoded64 = encoded;
            _scaleIndex = scaleIndex;
            return true;
        }

        // Re-scale not possible, flush and start new block with the higher scale factor
        _simple8bBuilder64.flush();
        _controlByteOffset = 0;

        // Make sure value and previous are using the same scale factor.
        uint8_t prevScaleIndex;
        std::tie(_prevEncoded64, prevScaleIndex) = scaleAndEncodeDouble(previous, scaleIndex);
        if (scaleIndex != prevScaleIndex) {
            std::tie(encoded, scaleIndex) = scaleAndEncodeDouble(value, prevScaleIndex);
            std::tie(_prevEncoded64, prevScaleIndex) = scaleAndEncodeDouble(previous, scaleIndex);
        }

        // Record our new scale factor
        _scaleIndex = scaleIndex;
    }

    // Append delta and check if we wrote a Simple8b block. If we did we may be able to reduce the
    // scale factor when starting a new block
    auto before = _bufBuilder.len();
    if (!_simple8bBuilder64.append(
            Simple8bTypeUtil::encodeInt64(calcDelta(encoded, _prevEncoded64))))
        return false;

    if (_bufBuilder.len() != before) {
        // Reset the scale factor to 0 and append all pending values to a new Simple8bBuilder. In
        // the worse case we will end up with an identical scale factor.
        auto prevScale = _scaleIndex;
        std::tie(_prevEncoded64, _scaleIndex) = scaleAndEncodeDouble(_lastValueInPrevBlock, 0);

        // Create a new Simple8bBuilder.
        Simple8bBuilder<uint64_t> builder(_createBufferWriter());
        std::swap(_simple8bBuilder64, builder);

        // Iterate over previous pending values and re-add them recursively. That will increase the
        // scale factor as needed.
        auto prev = _lastValueInPrevBlock;
        auto prevEncoded = *Simple8bTypeUtil::encodeDouble(prev, prevScale);
        for (const auto& pending : builder) {
            if (pending) {
                prevEncoded = expandDelta(prevEncoded, Simple8bTypeUtil::decodeInt64(*pending));
                auto val = Simple8bTypeUtil::decodeDouble(prevEncoded, prevScale);
                _appendDouble(val, prev);
                prev = val;
            } else {
                _simple8bBuilder64.skip();
            }
        }
    }

    _prevEncoded64 = encoded;
    return true;
}

BSONColumnBuilder& BSONColumnBuilder::skip() {
    ++_elementCount;

    auto before = _bufBuilder.len();
    if (_storeWith128) {
        _simple8bBuilder128.skip();
    } else {
        _simple8bBuilder64.skip();
    }
    // Rescale previous known value if this skip caused Simple-8b blocks to be written
    if (before != _bufBuilder.len() && _previous().type() == NumberDouble) {
        std::tie(_prevEncoded64, _scaleIndex) = scaleAndEncodeDouble(_lastValueInPrevBlock, 0);
    }
    return *this;
}

BSONBinData BSONColumnBuilder::finalize() {
    _simple8bBuilder128.flush();
    _simple8bBuilder64.flush();

    // Write EOO at the end
    _bufBuilder.appendChar(EOO);

    // Write element count at the beginning
    DataView(_bufBuilder.buf()).write<LittleEndian<uint32_t>>(_elementCount);

    return {_bufBuilder.buf(), _bufBuilder.len(), BinDataType::Column};
}

void BSONColumnBuilder::_storePrevious(BSONElement elem) {
    auto valuesize = elem.valuesize();

    // Add space for type byte and field name null terminator
    auto size = valuesize + 2;

    // Re-allocate buffer if not large enough
    if (size > _prevCapacity) {
        _prevCapacity = size;
        _prev = std::make_unique<char[]>(_prevCapacity);

        // Store null terminator, this byte will never change
        _prev[1] = '\0';
    }

    // Copy element into buffer for previous. Omit field name.
    _prev[0] = elem.type();
    memcpy(_prev.get() + 2, elem.value(), valuesize);
    _prevSize = size;
}

void BSONColumnBuilder::_writeLiteralFromPrevious() {
    // Write literal without field name and reset control byte to force new one to be written when
    // appending next value.
    auto prevElem = _previous();
    _bufBuilder.appendBuf(_prev.get(), _prevSize);

    // Reset state
    _controlByteOffset = 0;
    _scaleIndex = Simple8bTypeUtil::kMemoryAsInteger;
    _prevDelta = 0;

    // Initialize previous encoded when needed
    switch (prevElem.type()) {
        case NumberDouble:
            _lastValueInPrevBlock = prevElem._numberDouble();
            std::tie(_prevEncoded64, _scaleIndex) = scaleAndEncodeDouble(_lastValueInPrevBlock, 0);
            break;
        case String:
            _prevEncoded128 =
                Simple8bTypeUtil::encodeString(prevElem.valueStringData()).value_or(0);
            break;
        case BinData: {
            int size;
            const char* binary = prevElem.binData(size);
            _prevEncoded128 = Simple8bTypeUtil::encodeBinary(binary, size).value_or(0);
        } break;
        case NumberDecimal:
            _prevEncoded128 = Simple8bTypeUtil::encodeDecimal128(prevElem._numberDecimal());
            break;
        case jstOID:
            _prevEncoded64 = Simple8bTypeUtil::encodeObjectId(prevElem.__oid());
            break;
        default:
            break;
    }
}

void BSONColumnBuilder::_incrementSimple8bCount() {
    char* byte;
    uint8_t count;
    uint8_t control = kControlByteForScaleIndex[_scaleIndex];

    if (_controlByteOffset == 0) {
        // Allocate new control byte if we don't already have one. Record its offset so we can find
        // it even if the underlying buffer reallocates.
        byte = _bufBuilder.skip(1);
        _controlByteOffset = std::distance(_bufBuilder.buf(), byte);
        count = 0;
    } else {
        // Read current count from previous control byte
        byte = _bufBuilder.buf() + _controlByteOffset;

        // If previous byte was written with a different control byte then we can't re-use and need
        // to start a new one
        if ((*byte & kControlMask) != control) {
            _controlByteOffset = 0;
            _incrementSimple8bCount();
            return;
        }
        count = (*byte & kCountMask) + 1;
    }

    // Write back new count and clear offset if we have reached max count
    *byte = control | (count & kCountMask);
    if (count + 1 == kMaxCount) {
        _controlByteOffset = 0;
    }
}

Simple8bWriteFn BSONColumnBuilder::_createBufferWriter() {
    return [this](uint64_t block) {
        // Write/update block count
        _incrementSimple8bCount();

        // Write Simple-8b block in little endian byte order
        _bufBuilder.appendNum(block);

        auto previous = _previous();
        if (previous.type() == NumberDouble) {
            _lastValueInPrevBlock = previous._numberDouble();
        }

        return true;
    };
}

bool BSONColumnBuilder::_objectIdDeltaPossible(BSONElement elem, BSONElement prev) {
    return !memcmp(prev.OID().getInstanceUnique().bytes,
                   elem.OID().getInstanceUnique().bytes,
                   OID::kInstanceUniqueSize);
}

}  // namespace mongo
