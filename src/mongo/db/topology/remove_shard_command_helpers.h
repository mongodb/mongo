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

#pragma once

#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/remove_shard_draining_progress_gen.h"


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
