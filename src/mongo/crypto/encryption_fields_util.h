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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/field_ref.h"
#include "mongo/util/assert_util.h"

#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Returns whether the supplied BSON type is supported for FLE2 equality indexed encryption.
 */
inline bool isFLE2EqualityIndexedSupportedType(BSONType type) {
    switch (type) {
        case BSONType::binData:
        case BSONType::code:
        case BSONType::regEx:
        case BSONType::string:

        case BSONType::numberInt:
        case BSONType::numberLong:
        case BSONType::boolean:
        case BSONType::timestamp:
        case BSONType::date:
        case BSONType::oid:

        case BSONType::symbol:
        case BSONType::dbRef:
            return true;

        // Non-deterministic
        case BSONType::codeWScope:
        case BSONType::array:
        case BSONType::object:
        case BSONType::numberDecimal:
        case BSONType::numberDouble:

        // Singletons
        case BSONType::eoo:
        case BSONType::null:
        case BSONType::maxKey:
        case BSONType::minKey:
        case BSONType::undefined:
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
        case BSONType::numberInt:
        case BSONType::numberLong:
        case BSONType::numberDecimal:
        case BSONType::numberDouble:
        case BSONType::date:
            return true;

        // Valid for FLE Equality but not for Range.
        case BSONType::timestamp:
        case BSONType::boolean:
        case BSONType::binData:
        case BSONType::code:
        case BSONType::regEx:
        case BSONType::string:
        case BSONType::oid:
        case BSONType::symbol:
        case BSONType::dbRef:

        // Non-deterministic
        case BSONType::codeWScope:
        case BSONType::array:
        case BSONType::object:

        // Singletons
        case BSONType::eoo:
        case BSONType::null:
        case BSONType::maxKey:
        case BSONType::minKey:
        case BSONType::undefined:
            return false;
    }
    MONGO_UNREACHABLE;
}

/**
 * Returns whether the supplied BSON type is supported for FLE2 unindexed encryption.
 */
inline bool isFLE2UnindexedSupportedType(BSONType type) {
    switch (type) {
        case BSONType::binData:
        case BSONType::code:
        case BSONType::regEx:
        case BSONType::string:

        case BSONType::numberInt:
        case BSONType::numberLong:
        case BSONType::boolean:
        case BSONType::timestamp:
        case BSONType::date:
        case BSONType::oid:

        case BSONType::array:
        case BSONType::object:
        case BSONType::numberDecimal:
        case BSONType::numberDouble:

        // Deprecated
        case BSONType::symbol:
        case BSONType::codeWScope:
        case BSONType::dbRef:
            return true;

        // Singletons
        case BSONType::eoo:
        case BSONType::null:
        case BSONType::maxKey:
        case BSONType::minKey:
        case BSONType::undefined:
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
    return type == BSONType::string;
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
