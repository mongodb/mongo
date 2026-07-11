// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility_factory_interface.h"

namespace mongo::exec::agg {

/**
 * Factory responsible for LocalLookupEligibility based on ShardingState:
 *   - disabled (replica set / not initialized): AlwaysLocalEligibility.
 *   - enabled (sharded): ShardedClusterLocalLookupEligibility, which routes each lookup to decide
 *     locality against this shard.
 */
class LocalLookupEligibilityFactoryImpl final : public LocalLookupEligibilityFactoryInterface {
public:
    std::unique_ptr<LocalLookupEligibility> makeLocalLookupEligibility(
        OperationContext* opCtx) const override;
};

}  // namespace mongo::exec::agg
