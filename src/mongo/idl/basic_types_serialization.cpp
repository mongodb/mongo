// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/idl/basic_types_serialization.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/rpc/get_status_from_command_result.h"

#include <string_view>

namespace mongo::idl {

void serializeErrorStatus(const Status& status,
                          std::string_view fieldName,
                          BSONObjBuilder* builder) {
    uassert(7418500, "Status must be an error", !status.isOK());
    BSONObjBuilder sub{builder->subobjStart(fieldName)};
    status.serialize(&sub);
}

Status deserializeErrorStatus(const BSONElement& bsonElem) {
    const auto& bsonObj = bsonElem.Obj();
    long long code;
    uassertStatusOK(bsonExtractIntegerField(bsonObj, "code", &code));
    uassert(7418501, "Status must be an error", code != ErrorCodes::OK);
    return getErrorStatusFromCommandResult(bsonElem.Obj());
}

void assertBSONIsTimestamp(const BSONElement& e) {
    uassert(ErrorCodes::TypeMismatch,
            fmt::format("\"{}\" had the wrong type. Expected {}, found {}",
                        e.fieldNameStringData(),
                        BSONType::timestamp,
                        typeName(e.type())),
            e.type() == BSONType::timestamp);
}

LogicalTime deserializeLogicalTime(const BSONElement& e) {
    assertBSONIsTimestamp(e);

    return LogicalTime(Timestamp(e.timestampValue()));
}

Timestamp deserializeTimestamp(const BSONElement& e) {
    assertBSONIsTimestamp(e);

    return Timestamp(e.timestampValue());
}

}  // namespace mongo::idl
