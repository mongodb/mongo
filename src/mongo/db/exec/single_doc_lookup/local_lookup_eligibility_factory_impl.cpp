// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility_factory_impl.h"

#include "mongo/db/client.h"
#include "mongo/db/exec/single_doc_lookup/sharded_cluster_local_lookup_eligibility.h"
#include "mongo/db/topology/sharding_state.h"

namespace mongo::exec::agg {

std::unique_ptr<LocalLookupEligibility>
LocalLookupEligibilityFactoryImpl::makeLocalLookupEligibility(OperationContext* opCtx) const {
    auto* shardingState = ShardingState::get(opCtx);
    if (!shardingState || !shardingState->enabled()) {
        // Replica set / unsharded: every lookup is local by construction.
        return std::make_unique<AlwaysLocalEligibility>();
    }

    // A client that connected directly to this shard (rather than via mongos) bypassed the routing
    // protocol entirely, mirroring the MongoProcessInterface selection. Such connections are
    // treated as replica-set-like Every lookup is local by construction here too.
    const bool isInternalThreadOrClient =
        !opCtx->getClient()->session() || opCtx->getClient()->isInternalClient();
    if (!isInternalThreadOrClient) {
        return std::make_unique<AlwaysLocalEligibility>();
    }

    // Sharded, routed connection: route each lookup through the catalog cache to decide locality
    // against this shard.
    return std::make_unique<ShardedClusterLocalLookupEligibility>(shardingState->shardId());
}

}  // namespace mongo::exec::agg
