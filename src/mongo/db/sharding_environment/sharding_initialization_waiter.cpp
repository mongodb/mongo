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

#include "mongo/db/sharding_environment/sharding_initialization_waiter.h"

#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/vector_clock/vector_clock.h"

namespace mongo {

namespace sharding {

void awaitShardRoleReady(OperationContext* opCtx) {
    // We wait for the topology time here because we only need to see the write of the shard
    // identity document on this replica set and do not care that the write to config.shards is
    // not included in this timestamp.
    VectorClock::VectorTime vt = VectorClock::get(opCtx)->getTime();
    // If this node does not have a valid topology time it means this operation is not coming from
    // a mongos (since a mongoS would need to observe the addition of the shard to the cluster to
    // contact it). This indicates either a user doing something weird or an internal bug which is
    // accessing sharding components before they are initialized.
    uassert(ErrorCodes::ShardingStateNotInitialized,
            "This operation tried to access components associated with a sharded cluster, but this "
            "node has not been added to a shard. This indicates either a manually constructed "
            "request sent directly to the shard or a possible software bug.",
            VectorClock::isValidComponentTime(vt.topologyTime()));
    const auto lastSeenTopologyTime =
        repl::OpTime(vt.topologyTime().asTimestamp(), repl::OpTime::kUninitializedTerm);
    WaitForMajorityService::get(opCtx->getServiceContext())
        .waitUntilMajorityForRead(lastSeenTopologyTime, opCtx->getCancellationToken())
        .get();
}

}  // namespace sharding

}  // namespace mongo
