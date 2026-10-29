// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/replica_set_aware_service.h"

namespace mongo {

/**
 * Drops temporary collections on step-up.
 */
class TempCollectionsCleanupMongoD : public ReplicaSetAwareService<TempCollectionsCleanupMongoD> {
public:
    static TempCollectionsCleanupMongoD* get(ServiceContext* sc);

    std::string getServiceName() const final {
        return "TempCollectionsCleanupMongoD";
    }

    // unused no-ops
    void onStepUpBegin(OperationContext*, long long) final {}
    void onShutdown() final {}
    void onStartup(OperationContext*) final {}
    void onStepDown() final {}
    void onRollbackBegin() final {}
    void onConsistentDataAvailable(OperationContext*, bool, bool) final {}
    void onBecomeArbiter() final {}
    void onSetCurrentConfig(OperationContext*) final {}

    // used
    void onStepUpComplete(OperationContext* opCtx, long long term) final;
};

}  // namespace mongo
