// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <mutex>
#include <string>

namespace mongo {

/**
 * The BalancerStatsRegistry is used to cache metadata on shards, such as the orphan documents
 * count. The blancer (on the config sever) periodically fetches this metadata through the
 * _shardsvrGetStatsForBalancing command and uses it to evaluate balancing status of collections.
 *
 * The BalancerStatsRegistry is active only on replicaset primary nodes, it is initialized on stepUp
 * and terminated on stepDown.
 */

class [[MONGO_MOD_NEEDS_REPLACEMENT]] BalancerStatsRegistry
    : public ReplicaSetAwareServiceShardSvr<BalancerStatsRegistry> {

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
     * Update orphan document count for a specific collection.
     * `delta` is the increment/decrement that will be applied to the current cached count.
     *
     * If the registy is not initialized this function will be a noop.
     */
    [[MONGO_MOD_PRIVATE]] void updateOrphansCount(const UUID& collectionUUID, long long delta);
    void onRangeDeletionTaskInsertion(const UUID& collectionUUID, long long numOrphanDocs);
    void onRangeDeletionTaskDeletion(const UUID& collectionUUID, long long numOrphanDocs);

    [[MONGO_MOD_PRIVATE]] long long getCollNumOrphanDocs(const UUID& collectionUUID) const;

    /**
     * Retrieves the numOrphanDocs from the balancer stats registry if initialized or runs an
     * aggregation on disk if not.
     */
    long long getCollNumOrphanDocsFromDiskIfNeeded(OperationContext* opCtx,
                                                   const UUID& collectionUUID) const;

private:
    void onSetCurrentConfig(OperationContext* opCtx) final {}
    void onConsistentDataAvailable(OperationContext* opCtx,
                                   bool isMajority,
                                   bool isRollback) final {}
    void onStepUpBegin(OperationContext* opCtx, long long term) final {}
    void onBecomeArbiter() final {}
    void onRollbackBegin() final {}

    void onStartup(OperationContext* opCtx) final;
    void onStepUpComplete(OperationContext* opCtx, long long term) final;
    void onStepDown() final;
    void onShutdown() final;

    inline std::string getServiceName() const final {
        return "BalancerStatsRegistry";
    }

    void _loadOrphansCount(OperationContext* opCtx);
    bool _isInitialized() const {
        return _state.load() == State::kInitialized;
    }

    /**
     * Non blocking initialization. Performs an asynchronous initialization of this registry.
     */
    void _initializeAsync(OperationContext* opCtx);

    /**
     * Terminate the asynchronous initialization of this registry.
     */
    void _terminate(std::unique_lock<std::mutex>& lock);

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

    mutable std::mutex _stateMutex;
    Atomic<State> _state{State::kSecondary};
    ServiceContext::UniqueOperationContext _initOpCtxHolder;

    mutable std::mutex _mutex;
    // Map containing all the currently cached collection stats
    stdx::unordered_map<UUID, CollectionStats, UUID::Hash> _collStatsMap;

    // Thread pool used for asyncronous initialization
    std::shared_ptr<ThreadPool> _threadPool;
};

}  // namespace mongo
