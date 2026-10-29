// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/update_result.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

namespace mongo {

class CanonicalQuery;
class OperationContext;
class CollectionAcquisition;
class UpdateDriver;

/**
 * Utility method to execute an update described by "request".
 *
 * Caller must hold the appropriate database locks.
 */
[[MONGO_MOD_PUBLIC]] UpdateResult doUpdate(OperationContext* opCtx,
                                           CollectionAcquisition& coll,
                                           const UpdateRequest& request);

}  // namespace mongo
