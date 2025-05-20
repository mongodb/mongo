/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/idl/error_status_idl.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/rpc/get_status_from_command_result.h"

namespace mongo::idl {

void serializeErrorStatus(const Status& status, StringData fieldName, BSONObjBuilder* builder) {
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

}  // namespace mongo::idl
