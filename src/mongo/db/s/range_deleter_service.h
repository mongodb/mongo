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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class RangeDeleterService : public ReplicaSetAwareServiceShardSvr<RangeDeleterService> {
public:
    RangeDeleterService() = default;

    static RangeDeleterService* get(ServiceContext* serviceContext);

    static RangeDeleterService* get(OperationContext* opCtx);

private:
    /*
     * In memory representation of registered range deletion tasks. To each non-pending range
     * deletion task corresponds a registered task on the service.
     */
    class RangeDeletion : public ChunkRange {
    public:
        RangeDeletion(const RangeDeletionTask& task) : ChunkRange(task.getRange()) {}

        ~RangeDeletion() {
            if (!_completionPromise.getFuture().isReady()) {
                _completionPromise.setError(
                    Status{ErrorCodes::Interrupted, "Range deletion interrupted"});
            }
        }

        SharedSemiFuture<void> getPendingFuture() {
            return _pendingPromise.getFuture();
        }

        void clearPending() {
            if (!_pendingPromise.getFuture().isReady()) {
                _pendingPromise.emplaceValue();
            }
        }

        SharedSemiFuture<void> getCompletionFuture() const {
            return _completionPromise.getFuture().semi().share();
        }

        void makeReady() {
            _completionPromise.emplaceValue();
        }

    private:
        // Marked ready once the range deletion has been fully processed
        SharedPromise<void> _completionPromise;

        SharedPromise<void> _pendingPromise;
    };

    /*
     * Internal comparator to sort ranges in _rangeDeletionTasks's sets.
     *
     * NB: it ONLY makes sense to use this on ranges that are comparable, meaning
     * the ones based on the same key pattern (aka the ones belonging to the same
     * sharded collection).
     */
    struct RANGES_COMPARATOR {
        bool operator()(const std::shared_ptr<ChunkRange>& a,
                        const std::shared_ptr<ChunkRange>& b) const {
            return a->getMin().woCompare(b->getMin()) < 0;
        }
    };

    /*
     * Class enclosing a thread continuously processing "ready" range deletions, meaning tasks
     * that are allowed to be processed (already drained ongoing queries and already waited for
     * `orphanCleanupDelaySecs`).
     */
    class ReadyRangeDeletionsProcessor {
    public:
        ReadyRangeDeletionsProcessor(OperationContext* opCtx);
        ~ReadyRangeDeletionsProcessor();

        /*
         * Interrupt ongoing range deletions
         */
        void shutdown();

        /*
         * Schedule a range deletion at the end of the queue
         */
        void emplaceRangeDeletion(const RangeDeletionTask& rdt);

    private:
        /*
         * Return true if this processor have been shutted down
         */
        bool _stopRequested() const;

        /*
         * Remove a range deletion from the head of the queue. Supposed to be called only once a
         * range deletion successfully finishes.
         */
        void _completedRangeDeletion();

        /*
         * Code executed by the internal thread
         */
        void _runRangeDeletions();

        ServiceContext* const _service;

        mutable stdx::mutex _mutex;

        enum State { kRunning, kStopped };
        State _state{kRunning};

        /*
         * Condition variable notified when:
         * - The component has been initialized (the operation context has been instantiated)
         * - The instance is shutting down (the operation context has been marked killed)
         * - A new range deletion is scheduled (the queue size has increased by one)
         */
        stdx::condition_variable _condVar;

        /* Queue containing scheduled range deletions */
        std::queue<RangeDeletionTask> _queue;

        /* Pointer to the (one and only) operation context used by the thread */
        ServiceContext::UniqueOperationContext _threadOpCtxHolder;

        /* Thread consuming the range deletions queue */
        stdx::thread _thread;
    };

    // Keeping track of per-collection registered range deletion tasks
    stdx::unordered_map<UUID, std::set<std::shared_ptr<ChunkRange>, RANGES_COMPARATOR>, UUID::Hash>
        _rangeDeletionTasks;

    // Mono-threaded executor processing range deletion tasks
    std::shared_ptr<executor::TaskExecutor> _executor;

    enum State { kReadyForInitialization, kInitializing, kUp, kDown };

    State _state{kDown};

    // Future markes as ready when the state changes to "up"
    SharedSemiFuture<void> _stepUpCompletedFuture;
    // Operation context used for initialization
    ServiceContext::UniqueOperationContext _initOpCtxHolder;

    /* Acquire mutex only if service is up (for "user" operation) */
    [[nodiscard]] stdx::unique_lock<stdx::mutex> _acquireMutexFailIfServiceNotUp() {
        stdx::unique_lock<stdx::mutex> lg(_mutex_DO_NOT_USE_DIRECTLY);
        uassert(ErrorCodes::NotYetInitialized, "Range deleter service not up", _state == kUp);
        return lg;
    }

    /* Unconditionally acquire mutex (for internal operations) */
    [[nodiscard]] stdx::unique_lock<stdx::mutex> _acquireMutexUnconditionally() {
        stdx::unique_lock<stdx::mutex> lg(_mutex_DO_NOT_USE_DIRECTLY);
        return lg;
    }

    // Protecting the access to all class members (DO NOT USE DIRECTLY: rely on
    // `_acquireMutexUnconditionally` and `_acquireMutexFailIfServiceNotUp`)
    stdx::mutex _mutex_DO_NOT_USE_DIRECTLY;

public:
    /*
     * Register a task on the range deleter service.
     * Returns a future that will be marked ready once the range deletion will be completed.
     *
     * In case of trying to register an already existing task, the original future will be returned.
     *
     * A task can be registered only if the service is up (except for tasks resubmitted on step-up).
     *
     * When a task is registered as `pending`, it can be unblocked by calling again the same method
     * with `pending=false`.
     */
    SharedSemiFuture<void> registerTask(
        const RangeDeletionTask& rdt,
        SemiFuture<void>&& waitForActiveQueriesToComplete = SemiFuture<void>::makeReady(),
        bool fromResubmitOnStepUp = false,
        bool pending = false);

    /*
     * Deregister a task from the range deleter service.
     */
    void deregisterTask(const UUID& collUUID, const ChunkRange& range);

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
    SharedSemiFuture<void> getOverlappingRangeDeletionsFuture(const UUID& collectionUUID,
                                                              const ChunkRange& range);

    /**
     * Checks if the range deleter service is disabled.
     */
    bool isDisabled();

    /* ReplicaSetAwareServiceShardSvr implemented methods */
    void onStartup(OperationContext* opCtx) override;
    void onSetCurrentConfig(OperationContext* opCtx) override {}
    void onRollbackBegin() override {}
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
    long long totalNumOfRegisteredTasks();

    /* Returns a shared semi-future marked as ready once the service is initialized */
    SharedSemiFuture<void> getRangeDeleterServiceInitializationFuture() {
        return _stepUpCompletedFuture;
    }

    std::unique_ptr<ReadyRangeDeletionsProcessor> _readyRangeDeletionsProcessorPtr;

private:
    /* Join all threads and executor and reset the in memory state of the service
     * Used for onStartUpBegin and on onShutdown
     */
    void _joinAndResetState();

    /* Asynchronously register range deletions on the service. To be called on on step-up */
    void _recoverRangeDeletionsOnStepUp(OperationContext* opCtx);

    /* Called by shutdown/stepdown hooks to interrupt the service */
    void _stopService();

    /* ReplicaSetAwareServiceShardSvr "empty implemented" methods */
    void onConsistentDataAvailable(OperationContext* opCtx,
                                   bool isMajority,
                                   bool isRollback) final {}
    void onStepUpBegin(OperationContext* opCtx, long long term) final {};
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
    static inline const Lock::ResourceMutex _mutex{"ScopedRangeDeleterLock"};
};

}  // namespace mongo
