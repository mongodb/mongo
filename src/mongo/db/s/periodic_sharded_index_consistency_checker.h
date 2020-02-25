/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {

class OperationContext;
class ServiceContext;

/**
 * Owns a periodic background job that tracks the number of sharded collections that have
 * inconsistent indexes across shards. The job only runs on the primary node in the config server
 * replica set.
 */
class PeriodicShardedIndexConsistencyChecker final {
    PeriodicShardedIndexConsistencyChecker(const PeriodicShardedIndexConsistencyChecker&) = delete;
    PeriodicShardedIndexConsistencyChecker& operator=(
        const PeriodicShardedIndexConsistencyChecker&) = delete;

public:
    PeriodicShardedIndexConsistencyChecker() = default;
    ~PeriodicShardedIndexConsistencyChecker() = default;

    PeriodicShardedIndexConsistencyChecker(PeriodicShardedIndexConsistencyChecker&& source) =
        delete;
    PeriodicShardedIndexConsistencyChecker& operator=(
        PeriodicShardedIndexConsistencyChecker&& other) = delete;

    /**
     * Obtains the service-wide PeriodicShardedIndexConsistencyChecker instance.
     */
    static PeriodicShardedIndexConsistencyChecker& get(OperationContext* opCtx);
    static PeriodicShardedIndexConsistencyChecker& get(ServiceContext* serviceContext);

    long long getNumShardedCollsWithInconsistentIndexes() const;

    /**
     * Invoked when the config server primary enters the 'PRIMARY' state to
     * trigger the start of the periodic sharded index consistency check.
     */
    void onStepUp(ServiceContext* serviceContext);

    /**
     * Invoked when this node which is currently serving as a 'PRIMARY' steps down.
     *
     * Pauses the periodic job until subsequent step up. This method might be called
     * multiple times in succession, which is what happens as a result of incomplete
     * transition to primary so it is resilient to that.
     */
    void onStepDown();

    /**
     * Invoked when this node is shutting down. Stops the periodic job.
     */
    void onShutDown();

private:
    /**
     * Initializes and starts the periodic job.
     */
    void _launchShardedIndexConsistencyChecker(WithLock, ServiceContext* serviceContext);

    // Protects the variables below. Uses acquisition level 1 because it will be held while starting
    // a periodic job, which resolves a future.
    mutable Mutex _mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(1),
                                            "PeriodicShardedIndexConsistencyChecker::_mutex");

    // Periodic job for counting inconsistent indexes in the cluster.
    PeriodicJobAnchor _shardedIndexConsistencyChecker;

    // The latest count of sharded collections with inconsistent indexes.
    long long _numShardedCollsWithInconsistentIndexes{0};

    bool _isPrimary{false};
};
}  // namespace mongo
