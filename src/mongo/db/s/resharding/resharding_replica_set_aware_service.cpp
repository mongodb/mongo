// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_replica_set_aware_service.h"

#include "mongo/db/s/resharding/local_resharding_operations_registry.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"

namespace mongo {

namespace {

const auto decoration = ServiceContext::declareDecoration<ReshardingReplicaSetAwareService>();

const ReplicaSetAwareServiceRegistry::Registerer<ReshardingReplicaSetAwareService> registerer(
    "ReshardingReplicaSetAwareService");

}  // namespace

ReshardingReplicaSetAwareService* ReshardingReplicaSetAwareService::get(
    ServiceContext* serviceContext) {
    return &decoration(serviceContext);
}

void ReshardingReplicaSetAwareService::onConsistentDataAvailable(OperationContext* opCtx,
                                                                 bool isMajority,
                                                                 bool isRollback) {
    if (resharding::gFeatureFlagReshardingRegistry.isEnabled()) {
        LocalReshardingOperationsRegistry::get().resyncFromDisk(
            opCtx,
            isRollback ? "replica set data became consistent after rollback"
                       : "replica set data became consistent");
    }
}

}  // namespace mongo
