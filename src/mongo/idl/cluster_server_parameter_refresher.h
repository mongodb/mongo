/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

namespace mongo {

/**
 * Runs a background job on mongos only that periodically refreshes its in-memory cache of cluster
 * server parameters with updated values from the config servers.
 */
class ClusterServerParameterRefresher {
public:
    ClusterServerParameterRefresher() = default;

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
     */
    Status refreshParameters(OperationContext* opCtx);

    /**
     * Set the period of the background job. This should only be used internally (by the
     * setParameter).
     */
    void setPeriod(Milliseconds period);

private:
    void run();

    std::unique_ptr<PeriodicJobAnchor> _job;
    multiversion::FeatureCompatibilityVersion _lastFcv;
};

Status clusterServerParameterRefreshIntervalSecsNotify(const int& newValue);

}  // namespace mongo
