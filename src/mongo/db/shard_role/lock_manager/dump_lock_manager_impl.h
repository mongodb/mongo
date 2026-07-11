// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/util/modules.h"

#include <map>

namespace mongo {

/**
 * Diagnostics function which obtains a mapping of lock to client info. Used by the lockInfo command
 * and any other code which needs to generate a global view of what operations hold what resources.
 */
[[MONGO_MOD_PRIVATE]] std::map<LockerId, BSONObj> getLockerIdToClientMap(
    ServiceContext* serviceContext);

}  // namespace mongo
