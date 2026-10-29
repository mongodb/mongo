// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Extracts the write concern error from a command response.
 */
Status getWriteConcernStatusFromCommandResult(const BSONObj& cmdResponse);


/**
 * Extracts the first write error from a command response and converts it into a status. This
 * ignores all errors after the first and does not preserve the write error index, so it should not
 * be used with bulk writes.
 */
Status getFirstWriteErrorStatusFromCommandResult(const BSONObj& cmdResponse);

/**
 * Extracts the first write error from a bulk write command response and converts it into a status.
 * This ignores all errors after the first.
 */
Status getFirstWriteErrorStatusFromBulkWriteResult(const BSONObj& cmdResponse);

/**
 * Extracts any type of error from a write command response.
 */
Status getStatusFromWriteCommandReply(const BSONObj& cmdResponse);

}  // namespace mongo
