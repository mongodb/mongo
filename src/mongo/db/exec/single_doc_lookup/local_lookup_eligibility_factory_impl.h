// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility_factory_interface.h"

namespace mongo::exec::agg {

/**
 * Factory responsible for LocalLookupEligibility based on ShardingState and how the current
 * client connected:
 *   - ShardingState disabled (replica set / not initialized): AlwaysLocalEligibility.
 *   - ShardingState enabled, but the client connected directly to this shard rather than via
 *     mongos (not an internal thread/client): AlwaysLocalEligibility, mirroring
 *     mongod_process_interface_factory.cpp's MongoProcessInterface selection for the same reason
 *     -- such a connection bypassed the routing protocol, so there is no safe way to reason about
 *     cross-shard placement for it.
 *   - ShardingState enabled and routed (internal client, e.g. mongos or another shard):
 *     ShardedClusterLocalLookupEligibility, which routes each lookup to decide locality against
 *     this shard.
 */
class LocalLookupEligibilityFactoryImpl final : public LocalLookupEligibilityFactoryInterface {
public:
    std::unique_ptr<LocalLookupEligibility> makeLocalLookupEligibility(
        OperationContext* opCtx) const override;
};

}  // namespace mongo::exec::agg
