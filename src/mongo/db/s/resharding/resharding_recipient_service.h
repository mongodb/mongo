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

#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/db/s/resharding/resharding_data_replication.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_metrics.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {

class ReshardingRecipientService : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "ReshardingRecipientService"_sd;

    explicit ReshardingRecipientService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext) {}
    ~ReshardingRecipientService() = default;

    class RecipientStateMachine;

    class RecipientStateMachineExternalState;

    StringData getServiceName() const override {
        return kServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kRecipientReshardingOperationsNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const override;

    // The service implemented its own conflict check before this method was added.
    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const repl::PrimaryOnlyService::Instance*>& existingInstances) override{};

    std::shared_ptr<repl::PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) override;

    inline std::vector<std::shared_ptr<PrimaryOnlyService::Instance>> getAllReshardingInstances(
        OperationContext* opCtx) {
        return getAllInstances(opCtx);
    }
};

/**
 * Represents the current state of a resharding recipient operation on this shard. This class
 * drives state transitions and updates to underlying on-disk metadata.
 */
class ReshardingRecipientService::RecipientStateMachine final
    : public repl::PrimaryOnlyService::TypedInstance<RecipientStateMachine> {
public:
    struct CloneDetails {
        Timestamp cloneTimestamp;
        int64_t approxDocumentsToCopy;
        int64_t approxBytesToCopy;
        std::vector<DonorShardFetchTimestamp> donorShards;

        auto lens() const {
            return std::tie(cloneTimestamp, approxDocumentsToCopy, approxBytesToCopy);
        }

        friend bool operator==(const CloneDetails& a, const CloneDetails& b) {
            return a.lens() == b.lens();
        }

        friend bool operator!=(const CloneDetails& a, const CloneDetails& b) {
            return a.lens() != b.lens();
        }
    };

    explicit RecipientStateMachine(
        const ReshardingRecipientService* recipientService,
        const ReshardingRecipientDocument& recipientDoc,
        std::unique_ptr<RecipientStateMachineExternalState> externalState,
        ReshardingDataReplicationFactory dataReplicationFactory);

    ~RecipientStateMachine() = default;

    /**
     *  Runs up until the recipient is in state kStrictConsistency or encountered an error.
     */
    ExecutorFuture<void> _runUntilStrictConsistencyOrErrored(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken) noexcept;

    /**
     * Notifies the coordinator if the recipient is in kStrictConsistency or kError and waits for
     * _coordinatorHasDecisionPersisted to be fulfilled (success) or for the abortToken to be
     * canceled (failure or stepdown).
     */
    ExecutorFuture<void> _notifyCoordinatorAndAwaitDecision(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken) noexcept;

    /**
     * Finishes the work left remaining on the recipient after the coordinator persists its decision
     * to abort or complete resharding.
     */
    ExecutorFuture<void> _finishReshardingOperation(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& stepdownToken,
        bool aborted) noexcept;

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& token) noexcept override;

    void interrupt(Status status) override;

    /**
     * Returns a Future fulfilled once the recipient locally persists its final state before the
     * coordinator makes its decision to commit or abort (RecipientStateEnum::kError or
     * RecipientStateEnum::kStrictConsistency).
     */
    SharedSemiFuture<void> awaitInStrictConsistencyOrError() const {
        return _inStrictConsistencyOrError.getFuture();
    }

    /**
     * Returns a Future that will be resolved when all work associated with this Instance is done
     * making forward progress.
     */
    SharedSemiFuture<void> getCompletionFuture() const {
        return _completionPromise.getFuture();
    }

    inline const CommonReshardingMetadata& getMetadata() const {
        return _metadata;
    }

    inline const ReshardingMetrics& getMetrics() const {
        invariant(_metrics);
        return *_metrics;
    }

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode,
        MongoProcessInterface::CurrentOpSessionsMode) noexcept override;

    void onReshardingFieldsChanges(OperationContext* opCtx,
                                   const TypeCollectionReshardingFields& reshardingFields);

    static void insertStateDocument(OperationContext* opCtx,
                                    const ReshardingRecipientDocument& recipientDoc);

    /**
     * Indicates that the coordinator has persisted a decision. Unblocks the
     * _coordinatorHasDecisionPersisted promise.
     */
    void commit();

    /**
     * Initiates the cancellation of the resharding operation.
     */
    void abort(bool isUserCancelled);

    void checkIfOptionsConflict(const BSONObj& stateDoc) const final {}

private:
    /**
     * The work inside this function must be run regardless of any work on _scopedExecutor ever
     * running.
     */
    ExecutorFuture<void> _runMandatoryCleanup(Status status,
                                              const CancellationToken& stepdownToken);

    // The following functions correspond to the actions to take at a particular recipient state.
    ExecutorFuture<void> _awaitAllDonorsPreparedToDonateThenTransitionToCreatingCollection(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken,
        const CancelableOperationContextFactory& factory);

    void _createTemporaryReshardingCollectionThenTransitionToCloning(
        const CancelableOperationContextFactory& factory);

    ExecutorFuture<void> _cloneThenTransitionToApplying(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken,
        const CancelableOperationContextFactory& factory);

    ExecutorFuture<void> _awaitAllDonorsBlockingWritesThenTransitionToStrictConsistency(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken,
        const CancelableOperationContextFactory& factory);

    void _writeStrictConsistencyOplog(const CancelableOperationContextFactory& factory);

    void _renameTemporaryReshardingCollection(const CancelableOperationContextFactory& factory);

    void _cleanupReshardingCollections(bool aborted,
                                       const CancelableOperationContextFactory& factory);

    // Transitions the on-disk and in-memory state to 'newState'.
    void _transitionState(RecipientStateEnum newState,
                          const CancelableOperationContextFactory& factory);

    void _transitionState(RecipientShardContext&& newRecipientCtx,
                          boost::optional<CloneDetails>&& cloneDetails,
                          boost::optional<mongo::Date_t> configStartTime,
                          const CancelableOperationContextFactory& factory);

    // The following functions transition the on-disk and in-memory state to the named state.
    void _transitionToCreatingCollection(CloneDetails cloneDetails,
                                         boost::optional<mongo::Date_t> startConfigTxnCloneTime,
                                         const CancelableOperationContextFactory& factory);

    void _transitionToCloning(const CancelableOperationContextFactory& factory);

    void _transitionToApplying(const CancelableOperationContextFactory& factory);

    void _transitionToStrictConsistency(const CancelableOperationContextFactory& factory);

    void _transitionToError(Status abortReason, const CancelableOperationContextFactory& factory);

    BSONObj _makeQueryForCoordinatorUpdate(const ShardId& shardId, RecipientStateEnum newState);

    ExecutorFuture<void> _updateCoordinator(
        OperationContext* opCtx,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancelableOperationContextFactory& factory);

    // Updates the mutable portion of the on-disk and in-memory recipient document with
    // 'newRecipientCtx', 'fetchTimestamp and 'donorShards'.
    void _updateRecipientDocument(RecipientShardContext&& newRecipientCtx,
                                  boost::optional<CloneDetails>&& cloneDetails,
                                  boost::optional<mongo::Date_t> configStartTime,
                                  const CancelableOperationContextFactory& factory);

    // Removes the local recipient document from disk.
    void _removeRecipientDocument(bool aborted, const CancelableOperationContextFactory& factory);

    std::unique_ptr<ReshardingDataReplicationInterface> _makeDataReplication(
        OperationContext* opCtx, bool cloningDone);

    void _ensureDataReplicationStarted(
        OperationContext* opCtx,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken,
        const CancelableOperationContextFactory& factory);

    ExecutorFuture<void> _startMetrics(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken);

    // Restore metrics using the persisted metrics after stepping up.
    ExecutorFuture<void> _restoreMetricsWithRetry(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken);
    void _restoreMetrics(const CancelableOperationContextFactory& factory);

    // Initializes the _abortSource and generates a token from it to return back the caller.
    //
    // Should only be called once per lifetime.
    CancellationToken _initAbortSource(const CancellationToken& stepdownToken);

    // The primary-only service instance corresponding to the recipient instance. Not owned.
    const ReshardingRecipientService* const _recipientService;

    std::unique_ptr<ReshardingMetrics> _metrics;
    ReshardingApplierMetricsMap _applierMetricsMap;

    // The in-memory representation of the immutable portion of the document in
    // config.localReshardingOperations.recipient.
    const CommonReshardingMetadata _metadata;
    const Milliseconds _minimumOperationDuration;

    // The in-memory representation of the mutable portion of the document in
    // config.localReshardingOperations.recipient.
    RecipientShardContext _recipientCtx;
    std::vector<DonorShardFetchTimestamp> _donorShards;
    boost::optional<Timestamp> _cloneTimestamp;

    const std::unique_ptr<RecipientStateMachineExternalState> _externalState;

    // Time at which the minimum operation duration threshold has been met, and
    // config.transactions cloning can begin.
    boost::optional<Date_t> _startConfigTxnCloneAt;

    // ThreadPool used by CancelableOperationContext.
    // CancelableOperationContext must have a thread that is always available to it to mark its
    // opCtx as killed when the cancelToken has been cancelled.
    const std::shared_ptr<ThreadPool> _markKilledExecutor;
    boost::optional<resharding::RetryingCancelableOperationContextFactory>
        _retryingCancelableOpCtxFactory;

    const ReshardingDataReplicationFactory _dataReplicationFactory;
    SharedSemiFuture<void> _dataReplicationQuiesced;

    // Protects the state below
    Mutex _mutex = MONGO_MAKE_LATCH("RecipientStateMachine::_mutex");

    std::unique_ptr<ReshardingDataReplicationInterface> _dataReplication;

    // Canceled when there is an unrecoverable error or stepdown.
    boost::optional<CancellationSource> _abortSource;

    // The identifier associated to the recoverable critical section.
    const BSONObj _critSecReason;

    // It states whether the current node has also the donor role.
    const bool _isAlsoDonor;

    // It states whether or not the user has aborted the resharding operation.
    boost::optional<bool> _userCanceled;

    // Each promise below corresponds to a state on the recipient state machine. They are listed in
    // ascending order, such that the first promise below will be the first promise fulfilled.
    SharedPromise<CloneDetails> _allDonorsPreparedToDonate;

    SharedPromise<void> _inStrictConsistencyOrError;

    SharedPromise<void> _coordinatorHasDecisionPersisted;

    SharedPromise<void> _completionPromise;
};

}  // namespace mongo
