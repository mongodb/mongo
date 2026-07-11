// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/find_command_idl_utils.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo::find_command_idl_utils {

namespace {
void assertType(BSONElement element, BSONType type) {
    uassert(ErrorCodes::TypeMismatch,
            fmt::format("Expected field {} to be of type {}",
                        element.fieldNameStringData(),
                        typeName(type)),
            element.type() == type);
}
}  // namespace

void noOpSerializer(bool, std::string_view fieldName, BSONObjBuilder* bob) {}

void serializeBSONWhenNotEmpty(BSONObj obj, std::string_view fieldName, BSONObjBuilder* bob) {
    if (!obj.isEmpty()) {
        bob->append(fieldName, obj);
    }
}

BSONObj parseOwnedBSON(BSONElement element) {
    assertType(element, BSONType::object);
    return element.Obj().getOwned();
}

bool parseBoolean(BSONElement element) {
    assertType(element, BSONType::boolean);
    return element.boolean();
}

}  // namespace mongo::find_command_idl_utils
