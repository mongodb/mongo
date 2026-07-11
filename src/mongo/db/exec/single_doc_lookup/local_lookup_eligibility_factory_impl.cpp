// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility_factory_impl.h"

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

    // Sharded: route each lookup through the catalog cache to decide locality against this shard.
    return std::make_unique<ShardedClusterLocalLookupEligibility>(shardingState->shardId());
}

}  // namespace mongo::exec::agg
