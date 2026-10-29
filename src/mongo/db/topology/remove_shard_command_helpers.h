// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/ddl/ddl_lock_manager.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/remove_shard_draining_progress_gen.h"
#include "mongo/util/modules.h"


namespace mongo {

namespace topology_change_helpers {

/**
 * Runs the coordinator to remove a shard from the cluster.
 */
RemoveShardProgress runCoordinatorRemoveShard(
    OperationContext* opCtx,
    boost::optional<DDLLockManager::ScopedCollectionDDLLock>& ddlLock,
    boost::optional<FixedFCVRegion>& fcvRegion,
    const ShardId& shardId);

/**
 * Runs the different stages of the remove shard command - checkPreconditionsAndStartDrain,
 * checkDrainingStatus, and commit of the removal. Will retry the entire procedure after
 * receiving a ConflictingOperationOnProgress error.
 */
RemoveShardProgress removeShard(OperationContext* opCtx, const ShardId& shardId);

/**
 * Starts the draining process.
 */
boost::optional<RemoveShardProgress> startShardDraining(
    OperationContext* opCtx,
    const ShardId& shardId,
    DDLLockManager::ScopedCollectionDDLLock& ddlLock);


void stopShardDraining(OperationContext* opCtx, const ShardId& shardId);

}  // namespace topology_change_helpers
}  // namespace mongo
