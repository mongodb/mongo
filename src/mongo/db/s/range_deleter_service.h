// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/s/range_deletion_recovery_tracker.h"
#include "mongo/db/s/range_deletion_task_tracker.h"
#include "mongo/db/s/ready_range_deletions_processor.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] RangeDeleterService
    : public ReplicaSetAwareServiceShardSvr<RangeDeleterService> {
public:
    RangeDeleterService() = default;

    static RangeDeleterService* get(ServiceContext* serviceContext);

    static RangeDeleterService* get(OperationContext* opCtx);

private:
    RangeDeletionRecoveryTracker _recoveryState;
    std::unique_ptr<RangeDeletionRecoveryTracker::ActiveTerm> _activeTerm;

    // Keeping track of per-collection registered range deletion tasks.
    RangeDeletionTaskTracker _rangeDeletionTasks;

    // Range-deletion task ids classified by the MaxKey orphan guard. Populated at step-up before
    // processing begins, cleared on step-down/shutdown.
    stdx::unordered_set<UUID, UUID::Hash> _blockedMaxKeyTasks;

    // Mono-threaded executor processing range deletion tasks
    std::shared_ptr<executor::TaskExecutor> _executor;

    enum State { kReadyForInitialization, kInitializing, kUp, kDown };

    State _state{kDown};
    // Promise which is fulfilled once initialization for the current term has completed.
    boost::optional<SharedPromise<void>> _termInitializationPromise;
    // Promise which is fulfilled when the state changes to kUp.
    boost::optional<SharedPromise<void>> _serviceUpPromise;

    // Operation context used for initialization
    ServiceContext::UniqueOperationContext _initOpCtxHolder;

    /* Acquire mutex only if service is up (for "user" operation) */
    [[nodiscard]] std::unique_lock<std::mutex> _acquireMutexFailIfServiceNotUp() {
        std::unique_lock<std::mutex> lg(_mutex_DO_NOT_USE_DIRECTLY);
        uassert(ErrorCodes::NotYetInitialized, "Range deleter service not up", _state == kUp);
        return lg;
    }

    /* Unconditionally acquire mutex (for internal operations) */
    [[nodiscard]] std::unique_lock<std::mutex> _acquireMutexUnconditionally() {
        std::unique_lock<std::mutex> lg(_mutex_DO_NOT_USE_DIRECTLY);
        return lg;
    }

    // Protecting the access to all class members (DO NOT USE DIRECTLY: rely on
    // `_acquireMutexUnconditionally` and `_acquireMutexFailIfServiceNotUp`)
    std::mutex _mutex_DO_NOT_USE_DIRECTLY;

public:
    void registerRecoveryJob(long long term);
    void notifyRecoveryJobComplete(long long term);

    enum class TaskPending { kNotPending, kPending };

    /*
     * Register a task on the range deleter service.
     * Returns a future that will be marked ready once the range deletion will be completed.
     *
     * In case of trying to register an already existing task, the original future will be returned.
     *
     * A task can be registered only if the service has been initialized for this term.
     *
     * When a task is registered as `pending`, it can be unblocked by calling again the same method
     * with `pending=false`.
     */
    SharedSemiFuture<void> registerTask(
        const RangeDeletionTask& rdt,
        SemiFuture<void>&& waitForActiveQueriesToComplete = SemiFuture<void>::makeReady(),
        TaskPending pending = TaskPending::kNotPending);

    /*
     * Deregister a task from the range deleter service and fulfill its completion promise. Returns
     * the completed task or nullptr if no task existed.
     */
    std::shared_ptr<RangeDeletion> completeTask(const UUID& collUUID, const ChunkRange& range);

    /*
     * Returns the number of registered range deletion tasks for a collection
     */
    int getNumRangeDeletionTasksForCollection(const UUID& collectionUUID);

    /*
     * Returns a future marked as ready when all overlapping range deletion tasks complete.
     *
     * NB: in case an overlapping range deletion task is registered AFTER invoking this method,
     * it will not be taken into account. Handling this scenario is responsibility of the caller.
     * */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] SharedSemiFuture<void> getOverlappingRangeDeletionsFuture(
        const UUID& collectionUUID, const ChunkRange& range);

    /**
     * Checks if the range deleter service is disabled.
     */
    bool isDisabled();

    /*
     * Returns true iff 'taskId' was classified as a blocked MaxKey orphan task at step-up.
     */
    bool isMaxKeyBlocked(const UUID& taskId);

    /*
     * Replaces the blocked MaxKey task set. Called once per term at step-up, before processing.
     */
    void setBlockedMaxKeyTasks(std::vector<UUID> blockedTaskIds);

    /*
     * Classifies pre-existing range-deletion tasks and stores the blocked set (no-op when the guard
     * flag is off). Called by the range-deletion processor before it deletes any task, so a blocked
     * task is never deleted before classification. May throw; the caller retries.
     */
    void classifyBlockedMaxKeyTasks(OperationContext* opCtx);

    /* ReplicaSetAwareServiceShardSvr implemented methods */
    void onStartup(OperationContext* opCtx) override;
    void onSetCurrentConfig(OperationContext* opCtx) override {}
    void onRollbackBegin() override {}
    void onStepUpBegin(OperationContext* opCtx, long long term) override;
    void onStepUpComplete(OperationContext* opCtx, long long term) override;
    void onStepDown() override;
    void onShutdown() override;
    inline std::string getServiceName() const final {
        return "RangeDeleterService";
    }

    /*
     * Returns the RangeDeleterService state with the following schema:
     *     {collectionUUIDA: [{min: x, max: y}, {min: w, max: z}....], collectionUUIDB: ......}
     */
    BSONObj dumpState();

    /*
     * Returns the total number of range deletion tasks registered on the service.
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] long long totalNumOfRegisteredTasks();

    /* Returns a future which is fulfilled when the service is initialized for the current term. */
    SemiFuture<void> getTermInitializationFuture();

    /* Returns a future which is fulfilled when the service has reached the kUp state and is
     * actively processing ready tasks. */
    SemiFuture<void> getServiceUpFuture();

    std::unique_ptr<ReadyRangeDeletionsProcessor> _readyRangeDeletionsProcessorPtr;

private:
    SemiFuture<void> _getTermInitializationFuture(WithLock);

    /* Join all threads and executor and reset the in memory state of the service
     * Used for onStartUpBegin and on onShutdown
     */
    void _joinAndResetState();

    /* Asynchronously register range deletions on the service. To be called on on step-up. */
    void _launchRangeDeletionRecoveryTask(OperationContext* opCtx, long long term);

    /* Called by shutdown/stepdown hooks to interrupt the service */
    void _stopService();

    /* ReplicaSetAwareServiceShardSvr "empty implemented" methods */
    void onConsistentDataAvailable(OperationContext* opCtx,
                                   bool isMajority,
                                   bool isRollback) final {}
    void onBecomeArbiter() final {}
};

/**
 * Scoped lock to synchronize with the execution of range deletions.
 * The range-deleter acquires a scoped lock in IX mode while orphans are being deleted.
 * As long as this scoped lock is acquired in MODE_X, no range deletion will be running.
 */
class ScopedRangeDeleterLock {
public:
    ScopedRangeDeleterLock(OperationContext* opCtx, LockMode mode)
        : _resourceLock(opCtx, _mutex.getRid(), mode) {}

private:
    const Lock::ResourceLock _resourceLock;
    static inline const ResourceMutex _mutex{"ScopedRangeDeleterLock"};
};

}  // namespace mongo
