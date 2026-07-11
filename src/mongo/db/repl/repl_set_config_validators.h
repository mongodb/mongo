// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
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
#include <string_view>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] repl {

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
inline void smallExactInt64Append(std::int64_t value,
                                  std::string_view fieldName,
                                  BSONObjBuilder* bob) {
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
inline void serializeTermField(std::int64_t value,
                               std::string_view fieldName,
                               BSONObjBuilder* bob) {
    if (value != OpTime::kUninitializedTerm) {
        smallExactInt64Append(value, fieldName, bob);
    }
}

inline void serializeOptionalBoolIfTrue(const OptionalBool& value,
                                        std::string_view fieldName,
                                        BSONObjBuilder* bob) {
    if (value) {
        value.serializeToBSON(fieldName, bob);
    }
}

}  // namespace repl
}  // namespace mongo
