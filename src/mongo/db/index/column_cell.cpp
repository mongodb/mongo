/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/db/index/column_cell.h"

#include "mongo/db/storage/column_store.h"

namespace mongo {
namespace column_keygen {
namespace {
void appendInt(int64_t value, bool isLong, BufBuilder* cellBuffer) {
    using Bytes = ColumnStore::Bytes;
    using TinyNum = ColumnStore::Bytes::TinyNum;
    if (value >= TinyNum::kMinVal && value <= TinyNum::kMaxVal) {
        cellBuffer->appendUChar((isLong ? TinyNum::kTinyLongZero : TinyNum::kTinyIntZero) + value);
    } else if (int8_t small = value; small == value) {
        cellBuffer->appendUChar(isLong ? Bytes::kLong1 : Bytes::kInt1);
        cellBuffer->appendChar(small);
    } else if (int16_t small = value; small == value) {  // Intentionally shadowing earlier name.
        cellBuffer->appendUChar(isLong ? Bytes::kLong2 : Bytes::kInt2);
        cellBuffer->appendNum(small);  // Little-endian write.
    } else if (int32_t small = value; small == value) {
        cellBuffer->appendUChar(isLong ? Bytes::kLong4 : Bytes::kInt4);
        cellBuffer->appendNum(small);  // Little-endian write.
    } else {
        invariant(isLong);
        cellBuffer->appendUChar(Bytes::kLong8);
        cellBuffer->appendNum(value);  // Little-endian write.
    }
}

template <typename T>
boost::optional<T> downCastToIntType(double value) {
    if ((value == 0 && std::signbit(value)) || std::isnan(value)) {
        // Storing -0 or NaN as an integer type would lose information.
        return {};
    }

    if (value >= std::numeric_limits<T>::lowest() && value <= std::numeric_limits<T>::max()) {
        T downCastValue = value;
        if (value == downCastValue) {
            return downCastValue;
        }
    }

    return {};
}

void appendDouble(double value, BufBuilder* cellBuffer) {
    using Bytes = ColumnStore::Bytes;
    if (auto small = downCastToIntType<int8_t>(value); small) {
        cellBuffer->appendUChar(Bytes::kInt1Double);
        cellBuffer->appendChar(*small);
    } else if (float small = value; small == value) {
        char serialized[sizeof(small)];
        DataView(serialized).write<LittleEndian<float>>(small);
        cellBuffer->appendUChar(Bytes::kShortDouble);
        cellBuffer->appendBuf(serialized, sizeof(small));
    } else if (auto cents = downCastToIntType<int32_t>(std::round(value * 100));
               cents && (double(*cents) / 100 == value)) {
        if (int8_t small = *cents; small == *cents) {
            cellBuffer->appendUChar(Bytes::kCents1Double);
            cellBuffer->appendChar(small);
        } else if (int16_t small = *cents; small == *cents) {
            cellBuffer->appendUChar(Bytes::kCents2Double);
            cellBuffer->appendNum(small);  // Little-endian write.
        } else {
            cellBuffer->appendUChar(Bytes::kCents4Double);
            cellBuffer->appendNum(*cents);  // Little-endian write.
        }
    } else {
        cellBuffer->appendUChar(Bytes::kDouble);
        cellBuffer->appendNum(value);  // Little-endian write.
    }
}
}  // namespace

void appendElementToCell(const BSONElement& element, BufBuilder* cellBuffer) {
    using Bytes = ColumnStore::Bytes;
    switch (element.type()) {
        case jstNULL:
            return cellBuffer->appendUChar(Bytes::kNull);
        case MinKey:
            return cellBuffer->appendUChar(Bytes::kMinKey);
        case MaxKey:
            return cellBuffer->appendUChar(Bytes::kMaxKey);
        case Bool:
            return cellBuffer->appendUChar(element.boolean() ? Bytes::kTrue : Bytes::kFalse);
        case Object:
            // NB: This conversion is used by the "shredding" algorithm, which we expect to
            // decompose any non-empty objects. In the future, we may support partial shredding,
            // which would leave deeply nested objects intact and would require a way to store a
            // non-empty object in a cell.
            invariant(element.embeddedObject().isEmpty());
            return cellBuffer->appendUChar(Bytes::kEmptyObj);
        case Array:
            // NB: As with objects (above), we do not currently need to store non-empty arrays but
            // may need to in the future.
            invariant(element.embeddedObject().isEmpty());
            return cellBuffer->appendUChar(Bytes::kEmptyArr);
        case jstOID:
            cellBuffer->appendUChar(Bytes::kOID);
            cellBuffer->appendBuf(element.OID().view().view(), OID::kOIDSize);
            return;
        case String: {
            auto sd = element.valueStringData();
            if (sd.size() > Bytes::TinySize::kStringMax)
                break;  // Store large strings as BSON.

            cellBuffer->appendUChar(Bytes::kStringSizeMin + sd.size());
            cellBuffer->appendStr(sd, /*nul*/ false);
            return;
        }
        case NumberInt:
            return appendInt(element._numberInt(), /*long*/ false, cellBuffer);
        case NumberLong:
            return appendInt(element._numberLong(), /*long*/ true, cellBuffer);
        case NumberDecimal:
            cellBuffer->appendUChar(Bytes::kDecimal128);

            // This overload writes the on-disk format of its Decimal128 argument.
            cellBuffer->appendNum(element._numberDecimal());
            return;
        case NumberDouble:
            return appendDouble(element._numberDouble(), cellBuffer);

        default:
            // Fall through to storing as BSON.
            break;
    }

    // There is no columnar-specific way to format this value. We can still include it in a cell,
    // however, because column cells support directly appending BSONElement-formated values.
    cellBuffer->appendChar(element.type());
    cellBuffer->appendChar('\0');
    cellBuffer->appendBuf(element.value(), element.valuesize());
}

void writeEncodedCell(const UnencodedCellView& cell, BufBuilder* cellBuffer) {
    using Bytes = ColumnStore::Bytes;

    // The 'hasDuplicateFields' flag indicates an ill-formed document. We encode the values from all
    // instances of the multiply defined path, but we do not store an 'arrayInfo' or otherwise
    // attempt to encode the structure of the encoded values.
    if (cell.hasDuplicateFields) {
        cellBuffer->appendUChar(Bytes::kDuplicateFieldsMarker);
    }

    // Encode meaningful flags.
    if (cell.hasSubPaths && cell.vals.size() > 0) {
        // The 'SubPathsMarker' flag is unnecessary when there are no values. The reader will assume
        // the existence of sub objects.
        cellBuffer->appendUChar(Bytes::kSubPathsMarker);
    }
    if (cell.isSparse) {
        cellBuffer->appendUChar(Bytes::kSparseMarker);
    }
    if (cell.hasDoubleNestedArrays) {
        cellBuffer->appendUChar(Bytes::kDoubleNestedArraysMarker);
    }

    // Encode the 'arrayInfo' if it exists. An 'arrayInfo' is "tiny" iff its size can be encoded
    // directly within the tag byte that identifies its type. For larger 'arrayInfo' values, the
    // size gets encoded separately.
    auto arrayInfoSize = cell.arrayInfo.size();
    bool writeArrayInfo = arrayInfoSize > 0 && !cell.hasDuplicateFields;
    if (writeArrayInfo) {
        constexpr uint8_t maxTinySizeForArrayInfo =
            Bytes::kArrInfoSizeTinyMax - Bytes::kArrInfoSizeTinyMin + 1;
        if (arrayInfoSize <= maxTinySizeForArrayInfo) {
            uint8_t arrayInfoTag = Bytes::kArrInfoSizeTinyMin + (arrayInfoSize - 1);
            invariant(arrayInfoTag >= Bytes::kArrInfoSizeTinyMin &&
                      arrayInfoTag <= Bytes::kArrInfoSizeTinyMax);
            cellBuffer->appendUChar(arrayInfoTag);
        } else if (uint8_t smallSize = arrayInfoSize; smallSize == arrayInfoSize) {
            cellBuffer->appendUChar(Bytes::kArrInfoSize1);
            cellBuffer->appendUChar(smallSize);
        } else if (uint16_t smallSize = arrayInfoSize; smallSize == arrayInfoSize) {
            // Note: BufBuilder does not have a uint16_t append function.
            char serialized[sizeof(smallSize)];
            DataView(serialized).write<LittleEndian<uint16_t>>(smallSize);
            cellBuffer->appendUChar(Bytes::kArrInfoSize2);
            cellBuffer->appendBuf(serialized, sizeof(smallSize));
        } else if (uint32_t smallSize = arrayInfoSize; smallSize == arrayInfoSize) {
            // Note: BufBuilder does not have a uint32_t append function.
            char serialized[sizeof(smallSize)];
            DataView(serialized).write<LittleEndian<uint32_t>>(smallSize);
            cellBuffer->appendUChar(Bytes::kArrInfoSize4);
            cellBuffer->appendBuf(serialized, sizeof(smallSize));
        } else {
            // No path in a BSON document that fits in the storage limit could generate an arrayInfo
            // of this size.
            MONGO_UNREACHABLE;
        }
    }

    for (auto&& value : cell.vals) {
        appendElementToCell(value, cellBuffer);
    }

    if (writeArrayInfo) {
        cellBuffer->appendBuf(cell.arrayInfo.rawData(), cell.arrayInfo.size());
    }
}
}  // namespace column_keygen
}  // namespace mongo
