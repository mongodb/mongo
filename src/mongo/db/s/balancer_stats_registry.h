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

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * Scoped lock to synchronize with the execution of range deletions.
 * The range-deleter acquires a scoped lock in IX mode while orphans are being deleted.
 * Acquiring the scoped lock in MODE_X ensures that no orphan counter in `config.rangeDeletions`
 * entries is going to be updated concurrently.
 */
class ScopedRangeDeleterLock {
public:
    ScopedRangeDeleterLock(OperationContext* opCtx, LockMode mode)
        : _resourceLock(opCtx, _mutex.getRid(), mode) {}

private:
    const Lock::ResourceLock _resourceLock;
    static inline const Lock::ResourceMutex _mutex{"ScopedRangeDeleterLock"};
};

/**
 * The BalancerStatsRegistry is used to cache metadata on shards, such as the orphan documents
 * count. The blancer (on the config sever) periodically fetches this metadata through the
 * _shardsvrGetStatsForBalancing command and uses it to evaluate balancing status of collections.
 *
 * The BalancerStatsRegistry is active only on replicaset primary nodes, it is initialized on stepUp
 * and terminated on stepDown.
 */

class BalancerStatsRegistry : public ReplicaSetAwareServiceShardSvr<BalancerStatsRegistry> {

    BalancerStatsRegistry(const BalancerStatsRegistry&) = delete;
    BalancerStatsRegistry& operator=(const BalancerStatsRegistry&) = delete;

public:
    BalancerStatsRegistry() = default;

    /**
     * Obtains the service-wide instance.
     */
    static BalancerStatsRegistry* get(ServiceContext* serviceContext);
    static BalancerStatsRegistry* get(OperationContext* opCtx);

    /**
     * Non blocking initialization. Performs an asyncronous initialization of this registry.
     */
    void initializeAsync(OperationContext* opCtx);

    void terminate();

    /**
     * Update orphan document count for a specific collection.
     * `delta` is the increment/decrement that will be applied to the current cached count.
     *
     * If the registy is not initialized this function will be a noop.
     */
    void updateOrphansCount(const UUID& collectionUUID, long long delta);
    void onRangeDeletionTaskInsertion(const UUID& collectionUUID, long long numOrphanDocs);
    void onRangeDeletionTaskDeletion(const UUID& collectionUUID, long long numOrphanDocs);

    long long getCollNumOrphanDocs(const UUID& collectionUUID) const;

    /**
     * Retrieves the numOrphanDocs from the balancer stats registry if initialized or runs an
     * aggregation on disk if not.
     */
    long long getCollNumOrphanDocsFromDiskIfNeeded(OperationContext* opCtx,
                                                   const UUID& collectionUUID) const;

private:
    void onSetCurrentConfig(OperationContext* opCtx) override final {}
    void onInitialDataAvailable(OperationContext* opCtx,
                                bool isMajorityDataAvailable) override final {}
    void onStepUpBegin(OperationContext* opCtx, long long term) override final {}
    void onBecomeArbiter() override final {}
    void onShutdown() override final {}

    void onStartup(OperationContext* opCtx) override final;
    void onStepUpComplete(OperationContext* opCtx, long long term) override final;
    void onStepDown() override final;

    void _loadOrphansCount(OperationContext* opCtx);
    bool _isInitialized() const {
        return _state.load() == State::kInitialized;
    }

    struct CollectionStats {
        // Number of orphan documents for this collection
        long long numOrphanDocs;
        // Number of range deletion tasks
        long long numRangeDeletionTasks;
    };

    enum class State {
        kPrimaryIdle,  // The node is primary but the registry is not initialzed
        kInitializing,
        kInitialized,
        kTerminating,
        kSecondary,
    };

    mutable Mutex _stateMutex = MONGO_MAKE_LATCH("BalancerStatsRegistry::_stateMutex");
    AtomicWord<State> _state{State::kSecondary};
    ServiceContext::UniqueOperationContext _initOpCtxHolder;

    mutable Mutex _mutex = MONGO_MAKE_LATCH("BalancerStatsRegistry::_mutex");
    // Map containing all the currently cached collection stats
    stdx::unordered_map<UUID, CollectionStats, UUID::Hash> _collStatsMap;

    // Thread pool used for asyncronous initialization
    std::shared_ptr<ThreadPool> _threadPool;
};

}  // namespace mongo
