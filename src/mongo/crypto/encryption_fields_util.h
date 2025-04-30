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
#pragma once

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/field_ref.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * Returns whether the supplied BSON type is supported for FLE2 equality indexed encryption.
 */
inline bool isFLE2EqualityIndexedSupportedType(BSONType type) {
    switch (type) {
        case BinData:
        case Code:
        case RegEx:
        case String:

        case NumberInt:
        case NumberLong:
        case Bool:
        case bsonTimestamp:
        case Date:
        case jstOID:

        case Symbol:
        case DBRef:
            return true;

        // Non-deterministic
        case CodeWScope:
        case Array:
        case Object:
        case NumberDecimal:
        case NumberDouble:

        // Singletons
        case EOO:
        case jstNULL:
        case MaxKey:
        case MinKey:
        case Undefined:
            return false;
        default:
            MONGO_UNREACHABLE;
    }
}

/**
 * Returns whether the supplied BSON type is supported for FLE2 range indexed encryption.
 */
inline bool isFLE2RangeIndexedSupportedType(BSONType type) {
    switch (type) {
        case NumberInt:
        case NumberLong:
        case NumberDecimal:
        case NumberDouble:
        case Date:
            return true;

        // Valid for FLE Equality but not for Range.
        case bsonTimestamp:
        case Bool:
        case BinData:
        case Code:
        case RegEx:
        case String:
        case jstOID:
        case Symbol:
        case DBRef:

        // Non-deterministic
        case CodeWScope:
        case Array:
        case Object:

        // Singletons
        case EOO:
        case jstNULL:
        case MaxKey:
        case MinKey:
        case Undefined:
            return false;
    }
    MONGO_UNREACHABLE;
}

/**
 * Returns whether the supplied BSON type is supported for FLE2 unindexed encryption.
 */
inline bool isFLE2UnindexedSupportedType(BSONType type) {
    switch (type) {
        case BinData:
        case Code:
        case RegEx:
        case String:

        case NumberInt:
        case NumberLong:
        case Bool:
        case bsonTimestamp:
        case Date:
        case jstOID:

        case Array:
        case Object:
        case NumberDecimal:
        case NumberDouble:

        // Deprecated
        case Symbol:
        case CodeWScope:
        case DBRef:
            return true;

        // Singletons
        case EOO:
        case jstNULL:
        case MaxKey:
        case MinKey:
        case Undefined:
            return false;
        default:
            MONGO_UNREACHABLE;
    }
}

/**
 * Returns whether the supplied BSON type is supported for FLE2 substring/suffix/prefix indexed
 * encryption.
 */
inline bool isFLE2TextIndexedSupportedType(BSONType type) {
    return type == BSONType::String;
}

/**
 * Returns whether the supplied BSON type is supported for the encryption algorithm
 * that corresponds to the given FLE2 blob type.
 */
inline bool isFLE2SupportedType(EncryptedBinDataType fleType, BSONType bsonType) {
    switch (fleType) {
        case EncryptedBinDataType::kFLE2UnindexedEncryptedValue:
        case EncryptedBinDataType::kFLE2UnindexedEncryptedValueV2:
            return isFLE2UnindexedSupportedType(bsonType);
        case EncryptedBinDataType::kFLE2EqualityIndexedValue:
        case EncryptedBinDataType::kFLE2EqualityIndexedValueV2:
            return isFLE2EqualityIndexedSupportedType(bsonType);
        case EncryptedBinDataType::kFLE2RangeIndexedValue:
        case EncryptedBinDataType::kFLE2RangeIndexedValueV2:
            return isFLE2RangeIndexedSupportedType(bsonType);
        case EncryptedBinDataType::kFLE2TextIndexedValue:
            return isFLE2TextIndexedSupportedType(bsonType);
        default:
            MONGO_UNREACHABLE;
    }
}

/**
 * Returns whether the query type is for substring, suffix, or prefix indexed encryption.
 */
inline bool isFLE2TextQueryType(QueryTypeEnum qt) {
    return qt == QueryTypeEnum::SubstringPreview || qt == QueryTypeEnum::SuffixPreview ||
        qt == QueryTypeEnum::PrefixPreview;
}

struct EncryptedFieldMatchResult {
    FieldRef encryptedField;
    bool keyIsPrefixOrEqual;
};

/**
 * If key is an exact match or a prefix of a path in encryptedFields, returns the matching
 * FieldRef and true. If there exists a path in encryptedFields that is a prefix of key, returns
 * the FieldRef for that path and false. Otherwise, returns none.
 */
boost::optional<EncryptedFieldMatchResult> findMatchingEncryptedField(
    const FieldRef& key, const std::vector<FieldRef>& encryptedFields);

}  // namespace mongo
