// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Converts "result" into a Status object.  The input is expected to be the object returned
 * by running a command.  Returns ErrorCodes::CommandResultSchemaViolation if "result" does
 * not look like the result of a command.
 *
 * Command results must have a field called "ok" whose value may be interpreted as a boolean.
 * If the value interpreted as a boolean is true, the resultant status is Status::OK().
 * Otherwise, it is an error status.  The code comes from the "code" field, if present and
 * number-ish, while the reason message will come from the errmsg field, if present and
 * string-ish.
 */
Status getStatusFromCommandResult(const BSONObj& result);

/**
 * Converts error "result" into a Status object.  The input is expected to be the object returned
 * by running a command.  Assumes the result does not represent ok: 1.
 */
Status getErrorStatusFromCommandResult(const BSONObj& result);

}  // namespace mongo
