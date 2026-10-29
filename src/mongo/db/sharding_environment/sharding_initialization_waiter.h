// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {

namespace sharding {

/**
 * Waits until sharding components (ShardingState + Grid) are initialized. Can be called after
 * receiving a ShardingStateNotYetInitialized error, which can happen on secondaries which are part
 * of a sharded cluster but have not yet replicated the shard identity document.
 */
void awaitShardRoleReady(OperationContext* opCtx);

}  // namespace sharding

}  // namespace mongo
