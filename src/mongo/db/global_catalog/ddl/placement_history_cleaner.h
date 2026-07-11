// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"

#include <cstddef>
#include <mutex>
#include <string>

namespace mongo {

/**
 * Background service that launches a periodic job to assess whether there are documents that can be
 * removed from config.placementHistory.
 */
class PlacementHistoryCleaner : public ReplicaSetAwareServiceConfigSvr<PlacementHistoryCleaner> {
public:
    PlacementHistoryCleaner() = default;

    /**
     * Obtains the service-wide instance.
     */
    static PlacementHistoryCleaner* get(ServiceContext* serviceContext);
    static PlacementHistoryCleaner* get(OperationContext* opCtx);

    void pause();
    void resume(OperationContext* opCtx);

    static void runOnce(Client* client, size_t minPlacementHistoryEntries);

private:
    PlacementHistoryCleaner(const PlacementHistoryCleaner&) = delete;
    PlacementHistoryCleaner& operator=(const PlacementHistoryCleaner&) = delete;

    void _start(OperationContext* opCtx, bool steppingUp);

    void _stop(bool steppingDown);

    /**
     * ReplicaSetAwareService entry points.
     */
    void onStartup(OperationContext* opCtx) final {}

    void onSetCurrentConfig(OperationContext* opCtx) final {}

    void onConsistentDataAvailable(OperationContext* opCtx,
                                   bool isMajority,
                                   bool isRollback) final {}

    void onStepUpBegin(OperationContext* opCtx, long long term) final {}

    void onStepUpComplete(OperationContext* opCtx, long long term) final;

    void onStepDown() final;

    void onRollbackBegin() final {}

    void onShutdown() final {}

    void onBecomeArbiter() final {}

    inline std::string getServiceName() const final {
        return "PlacementHistoryCleaner";
    }

    std::mutex _mutex;

    PeriodicJobAnchor _anchor;

    bool _runningAsPrimary = false;
};
}  // namespace mongo
