/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <cstdint>

namespace mongo {
namespace MONGO_MOD_PUB repl {

/**
 * Validates that the given bool is true.
 */
inline Status validateTrue(bool boolVal) {
    if (!boolVal) {
        return {ErrorCodes::InvalidReplicaSetConfig, "Value must be true if specified"};
    }
    return Status::OK();
}

inline Status validateDefaultWriteConcernHasMember(const WriteConcernOptions& defaultWriteConcern) {
    if (defaultWriteConcern.isUnacknowledged()) {
        return Status(ErrorCodes::BadValue,
                      "Default write concern mode must wait for at least 1 member");
    }

    return Status::OK();
}

Status validateReplicaSetIdNotNull(OID replicaSetId);

/**
 * For serialization and deserialization of certain values in the IDL.
 */
inline void smallExactInt64Append(std::int64_t value, StringData fieldName, BSONObjBuilder* bob) {
    bob->appendNumber(fieldName, static_cast<long long>(value));
}

inline std::int64_t parseSmallExactInt64(const BSONElement& element) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Expected a number, got value of type " << typeName(element.type())
                          << " for field " << element.fieldName(),
            element.isNumber());
    std::int64_t result = element.safeNumberLong();
    uassert(4708900,
            str::stream() << "Expected field \"" << element.fieldName()
                          << "\" to have a value "
                             "exactly representable as a 64-bit integer, but found "
                          << element,
            result == element.numberDouble());
    return result;
}

// The term field is serialized like a smallExactInt except that -1 is not serialized.
inline void serializeTermField(std::int64_t value, StringData fieldName, BSONObjBuilder* bob) {
    if (value != OpTime::kUninitializedTerm) {
        smallExactInt64Append(value, fieldName, bob);
    }
}

inline void serializeOptionalBoolIfTrue(const OptionalBool& value,
                                        StringData fieldName,
                                        BSONObjBuilder* bob) {
    if (value) {
        value.serializeToBSON(fieldName, bob);
    }
}

}  // namespace MONGO_MOD_PUB repl
}  // namespace mongo
