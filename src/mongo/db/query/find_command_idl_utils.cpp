/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/find_command_idl_utils.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"

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

void noOpSerializer(bool, StringData fieldName, BSONObjBuilder* bob) {}

void serializeBSONWhenNotEmpty(BSONObj obj, StringData fieldName, BSONObjBuilder* bob) {
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
