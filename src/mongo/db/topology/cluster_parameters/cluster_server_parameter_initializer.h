// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

/**
 * An interface that provides methods to manipulate in-memory cluster server parameter values in
 * response to on-disk changes, specifically in a replica set context.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ClusterServerParameterInitializer
    : public ReplicaSetAwareService<ClusterServerParameterInitializer> {
    ClusterServerParameterInitializer(const ClusterServerParameterInitializer&) = delete;
    ClusterServerParameterInitializer& operator=(const ClusterServerParameterInitializer&) = delete;

public:
    ClusterServerParameterInitializer() = default;
    ~ClusterServerParameterInitializer() override = default;

    static ClusterServerParameterInitializer* get(OperationContext* opCtx);
    static ClusterServerParameterInitializer* get(ServiceContext* serviceContext);

    // Virtual methods coming from the ReplicaSetAwareService
    void onStartup(OperationContext* opCtx) final {}

    void onSetCurrentConfig(OperationContext* opCtx) final {}

    static void synchronizeAllParametersFromDisk(OperationContext* opCtx);
    /**
     * Called after startup recovery or initial sync is complete.
     */
    void onConsistentDataAvailable(OperationContext* opCtx, bool isMajority, bool isRollback) final;
    void onShutdown() final {}
    void onStepUpBegin(OperationContext* opCtx, long long term) final {}
    void onStepUpComplete(OperationContext* opCtx, long long term) final {}
    void onStepDown() final {}
    void onRollbackBegin() final {}
    void onBecomeArbiter() final {}
    inline std::string getServiceName() const final {
        return "ClusterServerParameterInitializer";
    }
};

}  // namespace mongo
