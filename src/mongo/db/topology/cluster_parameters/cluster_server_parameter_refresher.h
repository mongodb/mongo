// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/version/releases.h"

#include <memory>
#include <mutex>

#include <boost/move/utility_core.hpp>

namespace mongo {

/**
 * Runs a background job on mongos only that periodically refreshes its in-memory cache of cluster
 * server parameters with updated values from the config servers.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ClusterServerParameterRefresher {
public:
    static ClusterServerParameterRefresher* get(OperationContext* opCtx);
    static ClusterServerParameterRefresher* get(ServiceContext* serviceCtx);

    /**
     * Create a new ClusterServerParameterRefresher as a decorator on the service context
     * and start the background job.
     */
    static void start(ServiceContext* serviceCtx, OperationContext* opCtx);

    /**
     * Callback to be called when the mongos is shutting down. Will stop the currently running
     * refresh, if one is running, and stop running periodically.
     */
    static void onShutdown(ServiceContext* serviceCtx);

    /**
     * Refreshes all cluster server parameters from the config servers. Called periodically in the
     * run method, which executes in a background thread. Also called in-line during
     * getClusterParameter on mongos to ensure that cached values returned are up-to-date.
     * If 'ensureReadYourWritesConsistency' is true, then effect of all preceeding operations issued
     * by the current thread on the cluster parameters is visible after this method returns.
     * Otherwise, the values of the cluster parameters may be stale after this method returns.
     */
    Status refreshParameters(OperationContext* opCtx, bool ensureReadYourWritesConsistency = false);

    /**
     * Set the period of the background job. This should only be used internally (by the
     * setParameter).
     */
    void setPeriod(Milliseconds period);

    SharedPromise<void>* getRefreshPromise_forTest() {
        return _refreshPromise.get();
    }

private:
    void _run();

    /** What the actual refresh job runs to do a refresh. */
    Status _refreshParameters(OperationContext* opCtx);

    std::unique_ptr<PeriodicJobAnchor> _job;
    multiversion::FeatureCompatibilityVersion _lastFcv;
    std::mutex _mutex;

    std::unique_ptr<SharedPromise<void>> _refreshPromise;
};

[[MONGO_MOD_PRIVATE]] Status clusterServerParameterRefreshIntervalSecsNotify(const int& newValue);

[[MONGO_MOD_PRIVATE]]
std::pair<multiversion::FeatureCompatibilityVersion, TenantIdMap<StringMap<BSONObj>>>
getFCVAndClusterParametersFromConfigServer();

}  // namespace mongo
