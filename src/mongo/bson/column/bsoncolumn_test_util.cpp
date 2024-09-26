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

#include "mongo/bson/column/bsoncolumn_test_util.h"

namespace mongo::bsoncolumn {

bool areSBEBinariesEqual(sbe::bsoncolumn::SBEColumnMaterializer::Element& actual,
                         sbe::bsoncolumn::SBEColumnMaterializer::Element& expected) {
    // We should have already have checked the tags are equal or are expected values. Tags for
    // strings can differ based on how the SBE element is created, and thus should be verified
    // before.
    using namespace sbe::value;
    switch (actual.first) {
        // Values that are stored in 'Value' can be compared directly.
        case TypeTags::Nothing:
        case TypeTags::NumberInt32:
        case TypeTags::NumberInt64:
        case TypeTags::Boolean:
        case TypeTags::Null:
        case TypeTags::bsonUndefined:
        case TypeTags::MinKey:
        case TypeTags::MaxKey:
        case TypeTags::Date:
        case TypeTags::Timestamp:
            return actual.second == expected.second;
            break;
        case TypeTags::NumberDouble:
            if (isNaN(actual.first, actual.second)) {
                return isNaN(expected.first, expected.second);
            }
            return actual.second == expected.second;
            break;
        // The following types store pointers in 'Value'.
        case TypeTags::NumberDecimal:
            return bitcastTo<Decimal128>(actual.second)
                .isBinaryEqual(bitcastTo<Decimal128>(expected.second));
            break;
        case TypeTags::bsonObjectId:
            return memcmp(bitcastTo<uint8_t*>(actual.second),
                          bitcastTo<uint8_t*>(expected.second),
                          sizeof(ObjectIdType)) == 0;
            break;
        // For strings we can retrieve the strings and compare them directly.
        case TypeTags::bsonJavascript: {
            return getBsonJavascriptView(actual.second) == getBsonJavascriptView(expected.second);
            break;
        }
        case TypeTags::StringSmall:
        case TypeTags::bsonString:
            // Generic conversion won't produce StringSmall from BSONElements, but the
            // SBEColumnMaterializer will. So we can't compare the raw pointers since they are
            // different lengths, but we can compare the string values.
            return getStringView(actual.first, actual.second) ==
                getStringView(expected.first, expected.second);
            break;
        // We can read the raw pointer for these types, since the 32-bit 'length' at the
        // beginning of pointer holds the full length of the value.
        case TypeTags::bsonCodeWScope:
        case TypeTags::bsonSymbol:
        case TypeTags::bsonObject:
        case TypeTags::bsonArray: {
            auto actualPtr = getRawPointerView(actual.second);
            auto expectedPtr = getRawPointerView(expected.second);
            auto actSize = ConstDataView(actualPtr).read<LittleEndian<uint32_t>>();
            bool sizesEq = actSize == ConstDataView(expectedPtr).read<LittleEndian<uint32_t>>();
            return sizesEq && memcmp(actualPtr, expectedPtr, actSize) == 0;
            break;
        }
        // For these types we must find the correct number of bytes to read.
        case TypeTags::bsonRegex: {
            auto actualPtr = getRawPointerView(actual.second);
            auto expectedPtr = getRawPointerView(expected.second);
            auto numBytes = BsonRegex(actualPtr).byteSize();
            bool sizesEq = BsonRegex(expectedPtr).byteSize() == numBytes;
            return sizesEq && memcmp(actualPtr, expectedPtr, numBytes) == 0;
            break;
        }
        case TypeTags::bsonBinData: {
            // The 32-bit 'length' at the beginning of a BinData does _not_ account for the
            // 'length' field itself or the 'subtype' field.
            auto actualSize = getBSONBinDataSize(actual.first, actual.second);
            auto expectedSize = getBSONBinDataSize(expected.first, expected.second);
            bool sizesEq = actualSize == expectedSize;
            // We add 1 to compare the subtype and binData payload in one pass.
            return sizesEq &&
                memcmp(getRawPointerView(actual.second),
                       getRawPointerView(expected.second),
                       actualSize + 1) == 0;
            break;
        }
        case TypeTags::bsonDBPointer: {
            auto actualPtr = getRawPointerView(actual.second);
            auto expectedPtr = getRawPointerView(expected.second);
            auto numBytes = BsonDBPointer(actualPtr).byteSize();
            bool sizesEq = BsonDBPointer(expectedPtr).byteSize() == numBytes;
            return sizesEq && memcmp(actualPtr, expectedPtr, numBytes) == 0;
            break;
        }
        default:
            MONGO_UNREACHABLE
            return false;
            break;
    }
}

}  // namespace mongo::bsoncolumn
