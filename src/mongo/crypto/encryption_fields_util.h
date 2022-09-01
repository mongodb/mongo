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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/field_ref.h"
#include "mongo/util/assert_util.h"

namespace mongo {

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

// Wrapper of the three helper functions above. Should only be used on FLE type 6, 7, and 9.
inline bool isFLE2SupportedType(EncryptedBinDataType fleType, BSONType bsonType) {
    switch (fleType) {
        case EncryptedBinDataType::kFLE2UnindexedEncryptedValue:
            return isFLE2UnindexedSupportedType(bsonType);
        case EncryptedBinDataType::kFLE2EqualityIndexedValue:
            return isFLE2EqualityIndexedSupportedType(bsonType);
        case EncryptedBinDataType::kFLE2RangeIndexedValue:
            return isFLE2RangeIndexedSupportedType(bsonType);
        default:
            MONGO_UNREACHABLE;
    }
}

struct EncryptedFieldMatchResult {
    FieldRef encryptedField;
    bool keyIsPrefixOrEqual;
};

boost::optional<EncryptedFieldMatchResult> findMatchingEncryptedField(
    const FieldRef& key, const std::vector<FieldRef>& encryptedFields);

}  // namespace mongo
