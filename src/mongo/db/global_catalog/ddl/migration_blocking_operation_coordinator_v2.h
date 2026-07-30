// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/migration_blocking_operation_coordinator_v2_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/synchronized_value.h"

namespace mongo {

class MigrationBlockingOperationCoordinatorV2
    : public RecoverableShardingDDLCoordinator<MigrationBlockingOperationCoordinatorV2Document> {
public:
    using UUIDSet = stdx::unordered_set<UUID, UUID::Hash>;
    using ShardingCoordinator::getOrCreate;

    static std::shared_ptr<MigrationBlockingOperationCoordinatorV2> getOrCreate(
        OperationContext* opCtx, const NamespaceString& nss);
    static boost::optional<std::shared_ptr<MigrationBlockingOperationCoordinatorV2>> get(
        OperationContext* opCtx, const NamespaceString& nss);

    MigrationBlockingOperationCoordinatorV2(ShardingCoordinatorService* service,
                                            const BSONObj& initialState);

    void checkIfOptionsConflict(const BSONObj& stateDoc) const override;

    /**
     * Enqueue a request to begin/end the operation identified by `operationUUID`. The returned
     * future is fulfilled once the coordinator has processed the request: for a begin, once
     * migrations are confirmed blocked and the operation is durably recorded; for an end, once the
     * operation has been durably removed. If the coordinator is already tearing down, the future is
     * set with MigrationBlockingOperationCoordinatorCleaningUp so the caller retries on a fresh
     * instance.
     *
     * The migration-blocking work itself is driven by `_runImpl` and retried until success (while
     * holding the DDL locks), so callers observe either success or a retryable error.
     */
    Future<void> beginOperation(OperationContext* opCtx, const UUID& operationUUID);
    Future<void> endOperation(OperationContext* opCtx, const UUID& operationUUID);

protected:
    bool isInCriticalSection(Phase phase) const override;

private:
    enum class RequestType { kBeginOperation, kEndOperation };

    struct Request {
        RequestType type;
        UUID id;
        Promise<void> result;
    };

    /**
     * On interruption, fail any request still in the queue so callers never hang. Waits for the
     * current `_runImpl` workflow to resolve before draining so the drain cannot race the consumer
     * (`_processRequests`); see `_runImplWorkflow`.
     */
    void _onInterrupt(Status status) override;

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    /**
     * The migration-blocking work runs inside `_runImpl` and must abort on stepdown so that
     * recovery can re-drive it on the new primary, so the phase opCtxs must be cancelable.
     */
    bool _shouldUseCancelableOpCtx() const override {
        return true;
    }

    /**
     * Once we have reached kStartBlockingMigrations, migrations may have been blocked (the phase is
     * persisted before `_startBlockingMigrations` runs `allowMigrations(false)`). From that point
     * we must retry through to `_stopBlockingMigrations` rather than give up on a non-retriable
     * error, otherwise we could leave migrations blocked with no coordinator left to unblock them.
     */
    bool _mustAlwaysMakeProgress() override {
        return _doc.getPhase() >= Phase::kStartBlockingMigrations;
    }

    Future<void> _enqueue(OperationContext* opCtx, RequestType type, const UUID& operationUUID);

    /**
     * Phase handlers. `_processRequests` blocks consuming the request queue until every operation
     * has ended, at which point migrations are unblocked.
     */
    void _startBlockingMigrations(OperationContext* opCtx);
    void _processRequests(OperationContext* opCtx);
    void _stopBlockingMigrations(OperationContext* opCtx);

    /**
     * Pops the next request, waiting at most `timeout` for one to arrive. Returns boost::none if
     * the wait elapsed with the queue still empty.
     */
    boost::optional<Request> _popRequest(OperationContext* opCtx, Milliseconds timeout);

    /**
     * Applies `request` and fulfills its result promise: sets the value on success, or sets the
     * error on failure.
     */
    void _handleRequest(OperationContext* opCtx, Request& request);

    /**
     * Applies `request` to the operation set, persisting the change.
     */
    void _applyRequest(OperationContext* opCtx, Request& request);

    void _persistOperations(OperationContext* opCtx, const UUIDSet& operations);

    /**
     * Closes the producer end and fails every request still in the queue with `status`. Used both
     * when the operation set empties (with MigrationBlockingOperationCoordinatorCleaningUp) and on
     * the interruption teardown path (with the interrupt status).
     */
    void _closeAndFailQueue(const Status& status);

    /**
     * The set of ongoing operations. Only accessed by the single consumer thread (the `_runImpl`
     * phase handlers), so it needs no external synchronization.
     */
    UUIDSet _operations;

    MultiProducerSingleConsumerQueue<Request> _queue;

    // The completion future of the current `_runImpl` invocation, or boost::none if `_runImpl` has
    // never been entered. Set at the start of each `_runImpl` invocation and read by `_onInterrupt`
    // (which may run on a different thread); `_onInterrupt` waits on it so the queue is only
    // drained once the consumer (`_processRequests`) has stopped.
    synchronized_value<boost::optional<SharedSemiFuture<void>>> _runImplWorkflow;
};

}  // namespace mongo
