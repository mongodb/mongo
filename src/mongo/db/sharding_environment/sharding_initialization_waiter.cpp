// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/sharding_initialization_waiter.h"

#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"

namespace mongo {

namespace sharding {

void awaitShardRoleReady(OperationContext* opCtx) {
    // We wait for the topology time here because we only need to see the write of the shard
    // identity document on this replica set and do not care that the write to config.shards is
    // not included in this timestamp.
    VectorClock::VectorTime vt = VectorClock::get(opCtx)->getTime();
    // If this node does not have a valid topology time it means this operation is not coming from
    // a mongos (since a mongoS would need to observe the addition of the shard to the cluster to
    // contact it). This indicates
    // - either we are running a replicaset with --shardsvr but we are not part of a cluster yet
    // - or a user doing something weird
    // - or an internal bug which is accessing sharding components before they are initialized.
    uassert(ErrorCodes::ShardingStateNotInitialized,
            "This operation tried to access components associated with a sharded cluster, but this "
            "node has not been added to a shard, or the shard is not part of a cluster yet. This "
            "indicates either that the node is in a transitional state between a replicatset and "
            "sharded cluster, a manually constructed request has been sent directly to the shard, "
            "or a possible software bug",
            VectorClock::isValidComponentTime(vt.topologyTime()));
    const auto lastSeenTopologyTime =
        repl::OpTime(vt.topologyTime().asTimestamp(), repl::OpTime::kUninitializedTerm);
    WaitForMajorityService::get(opCtx->getServiceContext())
        .waitUntilMajorityForRead(lastSeenTopologyTime, opCtx->getCancellationToken())
        .get();
}

}  // namespace sharding

}  // namespace mongo
