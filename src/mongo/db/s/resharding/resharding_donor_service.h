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

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/resharding_critical_section.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"

namespace mongo {

class ReshardingDonorService : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "ReshardingDonorService"_sd;

    explicit ReshardingDonorService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext) {}
    ~ReshardingDonorService() = default;

    class DonorStateMachine;

    class DonorStateMachineExternalState;

    StringData getServiceName() const override {
        return kServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kDonorReshardingOperationsNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const override {
        // TODO Limit the size of ReshardingDonorService thread pool.
        return ThreadPool::Limits();
    }

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialState) override;
};

/**
 * Represents the current state of a resharding donor operation on this shard. This class drives
 * state transitions and updates to underlying on-disk metadata.
 */
class ReshardingDonorService::DonorStateMachine final
    : public repl::PrimaryOnlyService::TypedInstance<DonorStateMachine> {
public:
    explicit DonorStateMachine(const BSONObj& donorDoc,
                               std::unique_ptr<DonorStateMachineExternalState> externalState);

    ~DonorStateMachine();

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& stepdownToken) noexcept override;

    void interrupt(Status status) override;

    /**
     * Returns a Future that will be resolved when all work associated with this Instance has
     * completed running.
     */
    SharedSemiFuture<void> getCompletionFuture() const {
        return _completionPromise.getFuture();
    }

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override;

    void onReshardingFieldsChanges(OperationContext* opCtx,
                                   const TypeCollectionReshardingFields& reshardingFields);

    SharedSemiFuture<void> awaitFinalOplogEntriesWritten();

    static void insertStateDocument(OperationContext* opCtx,
                                    const ReshardingDonorDocument& donorDoc);

private:
    DonorStateMachine(const ReshardingDonorDocument& donorDoc,
                      std::unique_ptr<DonorStateMachineExternalState> externalState);

    /**
     * Runs up until the donor is either in state kBlockingWrites or encountered an error.
     */
    ExecutorFuture<void> _runUntilBlockingWritesOrErrored(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken) noexcept;

    /**
     * Notifies the coordinator if the donor is in kBlockingWrites or kError and waits for
     * _coordinatorHasDecisionPersisted to be fulfilled (success) or for the abortToken to be
     * canceled (failure or stepdown).
     */
    ExecutorFuture<void> _notifyCoordinatorAndAwaitDecision(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken) noexcept;

    /**
     * Finishes the work left remaining on the donor after the coordinator persists its decision to
     * abort or complete resharding.
     */
    ExecutorFuture<void> _finishReshardingOperation(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& stepdownToken,
        bool aborted) noexcept;

    // The following functions correspond to the actions to take at a particular donor state.
    void _transitionToPreparingToDonate();

    void _onPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData();

    ExecutorFuture<void> _awaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken);

    ExecutorFuture<void> _awaitAllRecipientsDoneApplyingThenTransitionToPreparingToBlockWrites(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken);

    void _writeTransactionOplogEntryThenTransitionToBlockingWrites();

    // Drops the original collection and throws if the returned status is not either Status::OK()
    // or NamespaceNotFound.
    void _dropOriginalCollectionThenTransitionToDone();

    // Transitions the on-disk and in-memory state to 'newState'.
    void _transitionState(DonorStateEnum newState);

    void _transitionState(DonorShardContext&& newDonorCtx);

    // Transitions the on-disk and in-memory state to DonorStateEnum::kDonatingInitialData.
    void _transitionToDonatingInitialData(Timestamp minFetchTimestamp,
                                          int64_t bytesToClone,
                                          int64_t documentsToClone);

    // Transitions the on-disk and in-memory state to DonorStateEnum::kError.
    void _transitionToError(Status abortReason);

    BSONObj _makeQueryForCoordinatorUpdate(const ShardId& shardId, DonorStateEnum newState);

    ExecutorFuture<void> _updateCoordinator(
        OperationContext* opCtx, const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    // Updates the mutable portion of the on-disk and in-memory donor document with 'newDonorCtx'.
    void _updateDonorDocument(DonorShardContext&& newDonorCtx);

    // Removes the local donor document from disk.
    void _removeDonorDocument();

    // Initializes the _abortSource and generates a token from it to return back the caller. If an
    // abort was reported prior to the initialization, automatically cancels the _abortSource before
    // returning the token.
    //
    // Should only be called once per lifetime.
    CancellationToken _initAbortSource(const CancellationToken& stepdownToken);

    // Initiates the cancellation of the resharding operation.
    void _onAbortEncountered(const Status& abortReason);

    // The in-memory representation of the immutable portion of the document in
    // config.localReshardingOperations.donor.
    const CommonReshardingMetadata _metadata;
    const std::vector<ShardId> _recipientShardIds;

    // The in-memory representation of the mutable portion of the document in
    // config.localReshardingOperations.donor.
    DonorShardContext _donorCtx;

    const std::unique_ptr<DonorStateMachineExternalState> _externalState;

    // ThreadPool used by CancelableOperationContext.
    // CancelableOperationContext must have a thread that is always available to it to mark its
    // opCtx as killed when the cancelToken has been cancelled.
    const std::shared_ptr<ThreadPool> _markKilledExecutor;
    boost::optional<CancelableOperationContextFactory> _cancelableOpCtxFactory;

    // Protects the state below
    Mutex _mutex = MONGO_MAKE_LATCH("DonorStateMachine::_mutex");

    // Canceled by 2 different sources: (1) This DonorStateMachine when it learns of an
    // unrecoverable error (2) The primary-only service instance driving this DonorStateMachine that
    // cancels the parent CancellationSource upon stepdown/failover.
    boost::optional<CancellationSource> _abortSource;

    // Holds the unrecoverable error reported by the coordinator that caused the entire resharding
    // operation to fail.
    boost::optional<Status> _abortReason;

    boost::optional<ReshardingCriticalSection> _critSec;

    // Each promise below corresponds to a state on the donor state machine. They are listed in
    // ascending order, such that the first promise below will be the first promise fulfilled -
    // fulfillment order is not necessarily maintained if the operation gets aborted.
    SharedPromise<void> _allRecipientsDoneCloning;

    SharedPromise<void> _allRecipientsDoneApplying;

    SharedPromise<void> _finalOplogEntriesWritten;

    SharedPromise<void> _coordinatorHasDecisionPersisted;

    SharedPromise<void> _completionPromise;
};

/**
 * Represents the interface that DonorStateMachine uses to interact with the rest of the sharding
 * codebase.
 *
 * In particular, DonorStateMachine must not directly use CatalogCacheLoader, Grid, ShardingState,
 * or ShardingCatalogClient. DonorStateMachine must instead access those types through the
 * DonorStateMachineExternalState interface. Having it behind an interface makes it more
 * straightforward to unit test DonorStateMachine.
 */
class ReshardingDonorService::DonorStateMachineExternalState {
public:
    virtual ~DonorStateMachineExternalState() = default;

    virtual ShardId myShardId(ServiceContext* serviceContext) const = 0;

    virtual void refreshCatalogCache(OperationContext* opCtx, const NamespaceString& nss) = 0;

    virtual void waitForCollectionFlush(OperationContext* opCtx, const NamespaceString& nss) = 0;

    virtual void updateCoordinatorDocument(OperationContext* opCtx,
                                           const BSONObj& query,
                                           const BSONObj& update) = 0;
};

}  // namespace mongo
