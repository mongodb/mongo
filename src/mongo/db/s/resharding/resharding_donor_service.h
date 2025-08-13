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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/resharding_change_streams_monitor.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class ReshardingDonorService : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "ReshardingDonorService"_sd;

    explicit ReshardingDonorService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext), _serviceContext(serviceContext) {}
    ~ReshardingDonorService() override = default;

    class DonorStateMachine;

    class DonorStateMachineExternalState;

    StringData getServiceName() const override {
        return kServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kDonorReshardingOperationsNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const override;

    // The service implemented its own conflict check before this method was added.
    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const Instance*>& existingInstances) override {}

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialState) override;

private:
    ServiceContext* _serviceContext;
};

/**
 * Represents the current state of a resharding donor operation on this shard. This class drives
 * state transitions and updates to underlying on-disk metadata.
 */
class ReshardingDonorService::DonorStateMachine final
    : public repl::PrimaryOnlyService::TypedInstance<DonorStateMachine> {
public:
    explicit DonorStateMachine(const ReshardingDonorService* donorService,
                               const ReshardingDonorDocument& donorDoc,
                               std::unique_ptr<DonorStateMachineExternalState> externalState,
                               ServiceContext* serviceContext);

    ~DonorStateMachine() override = default;

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

    void onReadDuringCriticalSection();
    void onWriteDuringCriticalSection();

    SharedSemiFuture<void> awaitCriticalSectionAcquired();

    SharedSemiFuture<void> awaitCriticalSectionPromoted();

    SharedSemiFuture<void> awaitFinalOplogEntriesWritten();

    /**
     * To be used by the _shardsvrReshardingDonorStartChangeStreamsMonitor command sent by the
     * coordinator. Notifies the donor to start the change streams monitor, and then waits for the
     * monitor to start. Throws an error if verification is not enabled.
     */
    SharedSemiFuture<void> createAndStartChangeStreamsMonitor(const Timestamp& cloneTimestamp);

    /**
     * To be used in testing only. Waits for the monitor to start. Throws an error if verification
     * is not enabled.
     */
    SharedSemiFuture<void> awaitChangeStreamsMonitorStarted();

    /**
     * To be used by the _shardsvrReshardingDonorFetchFinalCollectionStats command sent by the
     * coordinator. Waits for the monitor to complete and gets back the change in the number of
     * documents between the start of the cloning phase and the start of the critical section on
     * this donor. Throws an error if verification is not enabled.
     */
    SharedSemiFuture<int64_t> awaitChangeStreamsMonitorCompleted();

    /**
     * Returns a Future fulfilled once the donor locally persists its final state before the
     * coordinator makes its decision to commit or abort (DonorStateEnum::kError or
     * DonorStateEnum::kBlockingWrites).
     */
    SharedSemiFuture<void> awaitInBlockingWritesOrError() const {
        return _inBlockingWritesOrError.getFuture();
    }

    static void insertStateDocument(OperationContext* opCtx,
                                    const ReshardingDonorDocument& donorDoc);

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
     * Runs up until the donor is either in state kBlockingWrites or encountered an error.
     */
    ExecutorFuture<void> _runUntilBlockingWritesOrErrored(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken);

    /**
     * Notifies the coordinator if the donor is in kBlockingWrites or kError and waits for
     * _coordinatorHasDecisionPersisted to be fulfilled (success) or for the abortToken to be
     * canceled (failure or stepdown).
     */
    ExecutorFuture<void> _notifyCoordinatorAndAwaitDecision(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken);

    /**
     * Finishes the work left remaining on the donor after the coordinator persists its decision to
     * abort or complete resharding.
     */
    ExecutorFuture<void> _finishReshardingOperation(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& stepdownToken,
        bool aborted);

    /**
     * The work inside this function must be run regardless of any work on _scopedExecutor ever
     * running.
     */
    ExecutorFuture<void> _runMandatoryCleanup(Status status,
                                              const CancellationToken& stepdownToken);

    // The following functions correspond to the actions to take at a particular donor state.
    void _transitionToPreparingToDonate();

    void _onPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData(
        const CancelableOperationContextFactory& factory);

    /**
     * If verification is enabled, waits for the coordinator to notify this recipient to start the
     * change streams monitor, and then initializes and start the change streams monitor.
     */
    ExecutorFuture<void> _createAndStartChangeStreamsMonitor(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken,
        const CancelableOperationContextFactory& factory);

    ExecutorFuture<void> _awaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken,
        const CancelableOperationContextFactory& factory);

    ExecutorFuture<void> _awaitAllRecipientsDoneApplyingThenTransitionToPreparingToBlockWrites(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken,
        const CancelableOperationContextFactory& factory);

    void _writeTransactionOplogEntryThenTransitionToBlockingWrites(
        const CancelableOperationContextFactory& factory);

    /**
     * If verification is enabled, waits for the the change streams monitor to complete.
     */
    ExecutorFuture<void> _awaitChangeStreamsMonitorCompleted(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken);

    // Drops the original collection and throws if the returned status is not either Status::OK()
    // or NamespaceNotFound.
    void _dropOriginalCollectionThenTransitionToDone(
        const CancelableOperationContextFactory& factory);

    // Transitions the on-disk and in-memory state to 'newState'.
    void _transitionState(DonorStateEnum newState,
                          const CancelableOperationContextFactory& factory);

    void _transitionState(DonorShardContext&& newDonorCtx,
                          const CancelableOperationContextFactory& factory);

    // Transitions the on-disk and in-memory state to DonorStateEnum::kDonatingInitialData.
    void _transitionToDonatingInitialData(Timestamp minFetchTimestamp,
                                          int64_t bytesToClone,
                                          int64_t documentsToClone,
                                          int64_t indexCount,
                                          const CancelableOperationContextFactory& factory);

    // Transitions the on-disk and in-memory state to DonorStateEnum::kError.
    void _transitionToError(Status abortReason, const CancelableOperationContextFactory& factory);

    // Transitions the on-disk and in-memory state to DonorStateEnum::kDone.
    void _transitionToDone(bool aborted, const CancelableOperationContextFactory& factory);

    BSONObj _makeQueryForCoordinatorUpdate(const ShardId& shardId, DonorStateEnum newState);

    ExecutorFuture<void> _updateCoordinator(
        OperationContext* opCtx,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancelableOperationContextFactory& factory);

    // Updates the mutable portion of the on-disk and in-memory donor document with 'newDonorCtx'
    // and 'newChangeStreamsMonitorCtx'.
    void _updateDonorDocument(DonorShardContext&& newDonorCtx,
                              const CancelableOperationContextFactory& factory);
    void _updateDonorDocument(OperationContext* opCtx,
                              ChangeStreamsMonitorContext&& newChangeStreamsMonitorCtx);
    void _updateDonorDocument(OperationContext* opCtx, const BSONObj& updateMod);

    // Removes the local donor document from disk.
    void _removeDonorDocument(const CancellationToken& stepdownToken,
                              bool aborted,
                              const CancelableOperationContextFactory& factory);

    // Initializes the _abortSource and generates a token from it to return back the caller. If an
    // abort was reported prior to the initialization, automatically cancels the _abortSource before
    // returning the token.
    //
    // Should only be called once per lifetime.
    CancellationToken _initAbortSource(const CancellationToken& stepdownToken);

    // The primary-only service instance corresponding to the donor instance. Not owned.
    const ReshardingDonorService* const _donorService;

    ServiceContext* const _serviceContext;

    std::unique_ptr<ReshardingMetrics> _metrics;

    // The in-memory representation of the immutable portion of the document in
    // config.localReshardingOperations.donor.
    const CommonReshardingMetadata _metadata;
    const std::vector<ShardId> _recipientShardIds;

    // The in-memory representation of the mutable portion of the document in
    // config.localReshardingOperations.donor.
    DonorShardContext _donorCtx;
    boost::optional<ChangeStreamsMonitorContext> _changeStreamsMonitorCtx;

    // This is only used to restore metrics on a stepup.
    const ReshardingDonorMetrics _donorMetricsToRestore;

    const std::unique_ptr<DonorStateMachineExternalState> _externalState;
    std::shared_ptr<ReshardingChangeStreamsMonitor> _changeStreamsMonitor;

    // ThreadPool used by CancelableOperationContext.
    // CancelableOperationContext must have a thread that is always available to it to mark its
    // opCtx as killed when the cancelToken has been cancelled.
    const std::shared_ptr<ThreadPool> _markKilledExecutor;
    boost::optional<resharding::RetryingCancelableOperationContextFactory>
        _retryingCancelableOpCtxFactory;


    // Protects the state below
    stdx::mutex _mutex;

    // Canceled by 2 different sources: (1) This DonorStateMachine when it learns of an
    // unrecoverable error (2) The primary-only service instance driving this DonorStateMachine that
    // cancels the parent CancellationSource upon stepdown/failover.
    boost::optional<CancellationSource> _abortSource;

    // The identifier associated to the recoverable critical section.
    const BSONObj _critSecReason;

    // It states whether the current node has also the recipient role.
    const bool _isAlsoRecipient;

    // Each promise below corresponds to a state on the donor state machine. They are listed in
    // ascending order, such that the first promise below will be the first promise fulfilled -
    // fulfillment order is not necessarily maintained if the operation gets aborted.
    SharedPromise<void> _allRecipientsDoneCloning;

    SharedPromise<void> _allRecipientsDoneApplying;

    SharedPromise<void> _finalOplogEntriesWritten;

    SharedPromise<void> _inBlockingWritesOrError;

    SharedPromise<Timestamp> _changeStreamMonitorStartTimeSelected;
    SharedPromise<void> _changeStreamsMonitorStarted;
    SharedPromise<int64_t> _changeStreamsMonitorCompleted;
    SharedSemiFuture<void> _changeStreamsMonitorQuiesced;

    SharedPromise<void> _coordinatorHasDecisionPersisted;

    SharedPromise<void> _completionPromise;

    // Promises used to synchronize the acquisition/promotion of the recoverable critical section.
    SharedPromise<void> _critSecWasAcquired;
    SharedPromise<void> _critSecWasPromoted;
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

    virtual void waitForCollectionFlush(OperationContext* opCtx, const NamespaceString& nss) = 0;

    virtual void updateCoordinatorDocument(OperationContext* opCtx,
                                           const BSONObj& query,
                                           const BSONObj& update) = 0;

    virtual std::unique_ptr<ShardingRecoveryService::BeforeReleasingCustomAction>
    getOnReleaseCriticalSectionCustomAction() = 0;

    virtual void refreshCollectionPlacementInfo(OperationContext* opCtx,
                                                const NamespaceString& sourceNss) = 0;

    virtual void abortUnpreparedTransactionIfNecessary(OperationContext* opCtx) = 0;
};

}  // namespace mongo
