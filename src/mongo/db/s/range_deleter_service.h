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

#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/util/future_util.h"

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
        RangeDeletion(const RangeDeletionTask& task, SharedSemiFuture<void> completion)
            : ChunkRange(task.getRange().getMin(), task.getRange().getMax()),
              _completion(completion) {}

        SharedSemiFuture<void> getCompletionFuture() const {
            return _completion;
        }

    private:
        // Marked ready once the range deletion has been fully processed
        const SharedSemiFuture<void> _completion;
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

    // Keeping track of per-collection registered range deletion tasks
    stdx::unordered_map<UUID, std::set<std::shared_ptr<ChunkRange>, RANGES_COMPARATOR>, UUID::Hash>
        _rangeDeletionTasks;

    // Mono-threaded executor processing range deletion tasks
    std::shared_ptr<executor::TaskExecutor> _executor;

    enum State { kInitializing, kUp, kDown };

    AtomicWord<State> _state{kDown};

    /* Acquire mutex only if service is up (for "user" operation) */
    [[nodiscard]] stdx::unique_lock<Latch> _acquireMutexFailIfServiceNotUp() {
        stdx::unique_lock<Latch> lg(_mutex_DO_NOT_USE_DIRECTLY);
        uassert(
            ErrorCodes::NotYetInitialized, "Range deleter service not up", _state.load() == kUp);
        return lg;
    }

    /* Unconditionally acquire mutex (for internal operations) */
    [[nodiscard]] stdx::unique_lock<Latch> _acquireMutexUnconditionally() {
        stdx::unique_lock<Latch> lg(_mutex_DO_NOT_USE_DIRECTLY);
        return lg;
    }

    // TODO SERVER-67642 implement fine-grained per-collection locking
    // Protecting the access to all class members (DO NOT USE DIRECTLY: rely on
    // `_acquireMutexUnconditionally` and `_acquireMutexFailIfServiceNotUp`)
    Mutex _mutex_DO_NOT_USE_DIRECTLY = MONGO_MAKE_LATCH("RangeDeleterService::_mutex");

public:
    /*
     * Register a task on the range deleter service.
     * Returns a future that will be marked ready once the range deletion will be completed.
     *
     * In case of trying to register an already existing task, the future will contain an error.
     */
    SharedSemiFuture<void> registerTask(
        const RangeDeletionTask& rdt,
        SemiFuture<void>&& waitForActiveQueriesToComplete = SemiFuture<void>::makeReady(),
        bool fromResubmitOnStepUp = false);

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

    /* ReplicaSetAwareServiceShardSvr implemented methods */
    void onStepUpComplete(OperationContext* opCtx, long long term) override;
    void onStepDown() override;

    /*
     * Returns the RangeDeleterService state with the following schema:
     *     {collectionUUIDA: [{min: x, max: y}, {min: w, max: z}....], collectionUUIDB: ......}
     */
    BSONObj dumpState();

    /*
     * Returns the total number of range deletion tasks registered on the service.
     */
    long long totalNumOfRegisteredTasks();

private:
    /* Asynchronously register range deletions on the service. To be called on on step-up */
    void _recoverRangeDeletionsOnStepUp(OperationContext* opCtx);

    /* ReplicaSetAwareServiceShardSvr "empty implemented" methods */
    void onStartup(OperationContext* opCtx) override final{};
    void onInitialDataAvailable(OperationContext* opCtx,
                                bool isMajorityDataAvailable) override final {}
    void onShutdown() override final {}
    void onStepUpBegin(OperationContext* opCtx, long long term) override final {}
    void onBecomeArbiter() override final {}
};

}  // namespace mongo
