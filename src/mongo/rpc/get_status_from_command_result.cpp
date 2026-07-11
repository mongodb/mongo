// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/get_status_from_command_result.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/str.h"

#include <string>

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
    if (errmsgElement.type() == BSONType::string) {
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
