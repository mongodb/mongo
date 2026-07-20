// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"

#include <cstdint>
#include <mutex>

#include <boost/move/utility_core.hpp>

namespace mongo {

class OperationContext;
class ServiceContext;

/**
 * Owns a periodic background job that tracks the number of sharded collections that have
 * inconsistent indexes across shards. The job only runs on the primary node in the config server
 * replica set.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] PeriodicShardedIndexConsistencyChecker final {
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
     * Stops the periodic job.
     */
    void onStepDown();

    /**
     * Invoked when this node is shutting down. Stops the periodic job.
     */
    void onShutDown();

private:
    /**
     * Packs the primary status and the latest count of sharded collections with inconsistent
     * indexes into a single 64-bit word: the top bit stores the primary status and the remaining 63
     * bits store the count. Keeping both fields in one word allows them to be checked and updated
     * atomically without holding a mutex.
     */
    class State {
    public:
        State() = default;
        State(bool isPrimary, long long count)
            : _bits((isPrimary ? kPrimaryBit : 0) | (static_cast<uint64_t>(count) & kCountMask)) {
            dassert(count >= 0);
        }

        bool isPrimary() const {
            return _bits & kPrimaryBit;
        }

        long long count() const {
            return static_cast<long long>(_bits & kCountMask);
        }

    private:
        static constexpr uint64_t kPrimaryBit = 1ULL << 63;
        static constexpr uint64_t kCountMask = kPrimaryBit - 1;

        uint64_t _bits = 0;
    };

    /**
     * Initializes and starts the periodic job.
     */
    void _launchShardedIndexConsistencyChecker(WithLock, ServiceContext* serviceContext);

    /**
     * Initializes and starts the periodic job for timeseries shard key checking.
     * If the shard key for a sharded timeseries contains the time field then this job emits a
     * warning.
     *
     * This supposed to be temporary since the sharding on the time field will be deprecated in 9.0,
     * hence there is no need for the periodic check anymore.
     */
    void _launchShardedTimeseriesShardkeyChecker(WithLock, ServiceContext* serviceContext);

    // Protects the variables below. Uses acquisition level 1 because it will be held while starting
    // a periodic job, which resolves a future.
    mutable std::mutex _mutex;

    // Periodic job for counting inconsistent indexes in the cluster.
    PeriodicJobAnchor _shardedIndexConsistencyChecker;

    // Periodic job for counting inconsistent indexes in the cluster.
    PeriodicJobAnchor _shardedTimeseriesShardkeyChecker;

    // Tracks the primary status and the latest count of sharded collections with inconsistent
    // indexes.
    Atomic<State> _state{State{false, 0}};
};
}  // namespace mongo
