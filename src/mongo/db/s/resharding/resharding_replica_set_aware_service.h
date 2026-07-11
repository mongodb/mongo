// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/util/modules.h"

namespace mongo {

class ReshardingReplicaSetAwareService
    : public ReplicaSetAwareServiceShardSvr<ReshardingReplicaSetAwareService> {
public:
    static ReshardingReplicaSetAwareService* get(ServiceContext* serviceContext);

    std::string getServiceName() const final {
        return "ReshardingReplicaSetAwareService";
    }

    void onConsistentDataAvailable(OperationContext* opCtx, bool isMajority, bool isRollback) final;

    void onStepUpBegin(OperationContext*, long long) final {}
    void onShutdown() final {}
    void onStartup(OperationContext*) final {}
    void onStepDown() final {}
    void onRollbackBegin() final {}
    void onBecomeArbiter() final {}
    void onSetCurrentConfig(OperationContext*) final {}
    void onStepUpComplete(OperationContext*, long long) final {}
};

}  // namespace mongo
