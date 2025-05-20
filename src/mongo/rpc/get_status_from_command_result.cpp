/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/rpc/get_status_from_command_result.h"

#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/str.h"

namespace mongo {

Status getStatusFromCommandResult(const BSONObj& result) {
    BSONElement okElement = result["ok"];

    // StaleConfig doesn't pass "ok" in legacy servers
    BSONElement dollarErrElement = result["$err"];

    if (okElement.eoo() && dollarErrElement.eoo()) {
        return Status(ErrorCodes::CommandResultSchemaViolation,
                      str::stream() << "No \"ok\" field in command result " << result);
    }
    if (okElement.trueValue()) {
        return Status::OK();
    }
    return getErrorStatusFromCommandResult(result);
}

Status getErrorStatusFromCommandResult(const BSONObj& result) {
    BSONElement codeElement = result["code"];
    BSONElement errmsgElement = result["errmsg"];

    int code = codeElement.numberInt();
    if (0 == code) {
        code = ErrorCodes::UnknownError;
    }
    std::string errmsg;
    if (errmsgElement.type() == String) {
        errmsg = errmsgElement.String();
    } else if (!errmsgElement.eoo()) {
        errmsg = errmsgElement.toString();
    }

    // we can't use startsWith(errmsg, "no such")
    // as we have errors such as "no such collection"
    if (code == ErrorCodes::UnknownError &&
        (str::startsWith(errmsg, "no such cmd") || str::startsWith(errmsg, "no such command"))) {
        code = ErrorCodes::CommandNotFound;
    }

    return Status(ErrorCodes::Error(code), std::move(errmsg), result);
}

}  // namespace mongo
