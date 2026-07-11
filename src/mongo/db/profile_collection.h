// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/rpc/message.h"
#include "mongo/util/modules.h"

namespace mongo {

class Database;
class OperationContext;

// This namespace houses code related to the "system.profile" collection -- a capped system
// collection where, if enabled, we write statistics and other diagnostic information on a
// per-operation basis.
namespace profile_collection {

/**
 * Invoked when database profile is enabled.
 */
[[MONGO_MOD_PUBLIC]] void profile(OperationContext* opCtx, NetworkOp op);

/**
 * Pre-creates the profile collection for the specified database.
 */
Status createProfileCollection(OperationContext* opCtx, Database* db);

}  // namespace profile_collection
}  // namespace mongo
