// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/primary_only_service_helpers/cancel_state.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/resharding_change_streams_monitor.h"
#include "mongo/db/s/resharding/resharding_donor_promises.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/otel/telemetry_context.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

class [[MONGO_MOD_PUBLIC]] ReshardingDonorService : public repl::PrimaryOnlyService {
public:
    static constexpr std::string_view kServiceName = "ReshardingDonorService"sv;

    explicit ReshardingDonorService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext), _serviceContext(serviceContext) {}
    ~ReshardingDonorService() override = default;

    class [[MONGO_MOD_PRIVATE]] DonorStateMachine;

    class [[MONGO_MOD_PRIVATE]] DonorStateMachineExternalState;

    [[MONGO_MOD_PRIVATE]] std::string_view getServiceName() const override {
        return kServiceName;
    }

    [[MONGO_MOD_PRIVATE]] NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kDonorReshardingOperationsNamespace;
    }

    [[MONGO_MOD_PRIVATE]] ThreadPoolLimits getThreadPoolLimits() const override;

    // The service implemented its own conflict check before this method was added.
    [[MONGO_MOD_PRIVATE]] void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const Instance*>& existingInstances) override {}

    [[MONGO_MOD_PRIVATE]] std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) override;

    [[MONGO_MOD_PRIVATE]] void stepDown_forTest();
    [[MONGO_MOD_PRIVATE]] void stepUp_forTest();

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
        // coverity[missing_lock]
        return _completionPromise.getFuture();
    }

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override;

    void onReshardingFieldsChanges(OperationContext* opCtx,
                                   const TypeCollectionReshardingFields& reshardingFields);

    void onReadDuringCriticalSection();
    void onWriteDuringCriticalSection();

    /**
     * Fulfills the subset of donor promises that are driven by the coordinator advancing through
     * its state machine. Shared entry point for command handlers and onReshardingFieldsChanges.
     * Idempotent and cascading: invoking with a later state also fulfills promises associated with
     * all earlier coordinator states.
     *
     * Promises fulfilled here:
     *   newState >= kApplying        -> _allRecipientsDoneCloning   (via _promises)
     *   newState >= kBlockingWrites  -> _allRecipientsDoneApplying  (via _promises)
     *   newState >= kCommitting      -> _coordinatorHasDecisionPersisted
     */
    void onCoordinatorStateAdvanced(CoordinatorStateEnum newState);

    SharedSemiFuture<void> awaitCriticalSectionAcquired();

    SharedSemiFuture<void> awaitCriticalSectionPromoted();

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
     * Returns a Future fulfilled once the donor transitions into
     * DonorStateEnum::kDonatingOplogEntries, or fulfilled with an error if the donor fails before
     * reaching that state.
     */
    SharedSemiFuture<void> awaitInDonatingOplogEntries() const {
        return _promises.getInDonatingOplogEntriesFuture();
    }

    SharedSemiFuture<void> awaitAllRecipientsDoneCloningForTest() {
        return _promises.getAllRecipientsDoneCloningFuture();
    }

    SharedSemiFuture<void> awaitAllRecipientsDoneApplyingForTest() {
        return _promises.getAllRecipientsDoneApplyingFuture();
    }

    /**
     * Returns a Future fulfilled once the donor locally persists its final state before the
     * coordinator makes its decision to commit or abort (DonorStateEnum::kError or
     * DonorStateEnum::kBlockingWrites).
     */
    SharedSemiFuture<void> awaitInBlockingWritesOrError() const {
        return _promises.getInBlockingWritesOrErrorFuture();
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
     * Fulfills in-memory promises that can be inferred from the persisted donor state on step-up.
     * The workflow promises are recovered via _promises.recover(); the remaining SharedPromise
     * members (coordinator decision and change-streams-monitor promises) are recovered inline.
     */
    void _fulfillPromisesOnStepup(const ReshardingDonorDocument& donorDoc);

    /**
     * With-lock implementation of onCoordinatorStateAdvanced. Used by callers that already hold
     * _mutex.
     */
    void _onCoordinatorStateAdvanced(WithLock lk, CoordinatorStateEnum newState);

    /**
     * Helper to construct an opCtx and set non-deprioritizable state if needed.
     */
    CancelableOperationContext _makeOperationContext(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) const;

    /**
     * Runs up until the donor is either in state kBlockingWrites or encountered an error.
     */
    ExecutorFuture<void> _runUntilBlockingWritesOrErrored(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<otel::TelemetryContext> telemetryCtx);

    /**
     * Notifies the coordinator if the donor is in kBlockingWrites or kError and waits for
     * _coordinatorHasDecisionPersisted to be fulfilled (success) or for the abortToken to be
     * canceled (failure or stepdown).
     */
    ExecutorFuture<void> _notifyCoordinatorAndAwaitDecision(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    /**
     * Finishes the work left remaining on the donor after the coordinator persists its decision to
     * abort or complete resharding.
     */
    ExecutorFuture<void> _finishReshardingOperation(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    /**
     * The work inside this function must be run regardless of any work on _scopedExecutor ever
     * running.
     */
    ExecutorFuture<void> _runMandatoryCleanup(Status status);

    // The following functions correspond to the actions to take at a particular donor state.
    void _transitionToPreparingToDonate();

    void _onPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    /**
     * If verification is enabled, waits for the coordinator to notify this recipient to start the
     * change streams monitor, and then initializes and start the change streams monitor.
     */
    ExecutorFuture<void> _createAndStartChangeStreamsMonitor(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    void _createChangeStreamsMonitor(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    ExecutorFuture<void> _awaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    ExecutorFuture<void> _awaitAllRecipientsDoneApplyingThenTransitionToPreparingToBlockWrites(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    void _writeTransactionOplogEntryThenTransitionToBlockingWrites(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    /**
     * If verification is enabled, waits for the the change streams monitor to complete.
     */
    ExecutorFuture<void> _awaitChangeStreamsMonitorCompleted(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    // Drops the original collection and throws if the returned status is not either Status::OK()
    // or NamespaceNotFound.
    void _dropOriginalCollectionThenTransitionToDone(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    // Transitions the on-disk and in-memory state to 'newState'.
    void _transitionState(DonorStateEnum newState,
                          std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    void _transitionState(DonorShardContext&& newDonorCtx,
                          std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    // Transitions the on-disk and in-memory state to DonorStateEnum::kDonatingInitialData.
    void _transitionToDonatingInitialData(
        Timestamp minFetchTimestamp,
        int64_t bytesToClone,
        int64_t documentsToClone,
        int64_t indexCount,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    // Transitions the on-disk and in-memory state to DonorStateEnum::kError.
    void _transitionToError(Status abortReason,
                            std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    // Transitions the on-disk and in-memory state to DonorStateEnum::kDone.
    void _transitionToDone(std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    BSONObj _makeQueryForCoordinatorUpdate(const ShardId& shardId, DonorStateEnum newState);

    ExecutorFuture<void> _updateCoordinator(
        OperationContext* opCtx,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    // Updates the mutable portion of the on-disk and in-memory donor document with 'newDonorCtx'
    // and 'newChangeStreamsMonitorCtx'.
    void _updateDonorDocument(
        DonorShardContext&& newDonorCtx,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);
    void _updateDonorDocument(OperationContext* opCtx,
                              ChangeStreamsMonitorContext&& newChangeStreamsMonitorCtx);
    void _updateDonorDocument(OperationContext* opCtx, const BSONObj& updateMod);

    // Removes the local donor document from disk.
    void _removeDonorDocument(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    /**
     * Creates a new span with the resharding UUID set as an attribute.
     */
    otel::traces::Span _startSpan(std::shared_ptr<otel::TelemetryContext> telemetryCtx,
                                  otel::traces::SpanName spanName);

    // The primary-only service instance corresponding to the donor instance. Not owned.
    const ReshardingDonorService* const _donorService;

    ServiceContext* const _serviceContext;

    std::unique_ptr<ReshardingMetrics> _metrics;

    // The in-memory representation of the immutable portion of the document in
    // config.localReshardingOperations.donor.
    const CommonReshardingMetadata _metadata;

    // Cached copy of _metadata's ForwardableOperationMetadata with cross-shard propagation enabled.
    const boost::optional<ForwardableOperationMetadata> _forwardableOpMetadata;
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
    mutable std::mutex _mutex;

    // Manages abort state and provides cancellation tokens for async operations.
    primary_only_service_helpers::CancelState _cancelState;

    // The identifier associated to the recoverable critical section.
    const BSONObj _critSecReason;

    // It states whether the current node has also the recipient role.
    const bool _isAlsoRecipient;

    // Owns the donor-state-machine workflow promises (allRecipientsDoneCloning,
    // allRecipientsDoneApplying, inDonatingOplogEntries, inBlockingWritesOrError,
    // critSecWasAcquired, critSecWasPromoted). Stepup recovery, terminal-error broadcast, and
    // per-state fulfillment all flow through this wrapper. See ReshardingDonorPromises for the
    // per-promise recovery rules.
    ReshardingDonorPromises _promises;

    // Fulfilled once the coordinator has persisted its commit/abort decision. Set with success
    // by commit(), with an abort status by abort(), or by stepup recovery when the donor
    // already reflects the coordinator decision locally.
    SharedPromise<void> _coordinatorHasDecisionPersisted;

    // Fulfilled when all the work associated with this Instance has finished, including the
    // local state document being removed. Errored from interrupt() so that consumers waiting on
    // completion observe an error rather than hang if run() is never called by POS during
    // stepdown.
    SharedPromise<void> _completionPromise;

    // Change-streams-monitor SharedPromises (only used when verification is enabled). Kept out
    // of _promises because they have independent error semantics from the donor's main state
    // machine: a monitor failure does not necessarily error the workflow promises and vice
    // versa.
    SharedPromise<Timestamp> _changeStreamMonitorStartTimeSelected;
    SharedPromise<void> _changeStreamsMonitorStarted;
    SharedPromise<int64_t> _changeStreamsMonitorCompleted;

    // The change-streams-monitor quiesce future is wired up by the monitor itself.
    SharedSemiFuture<void> _changeStreamsMonitorQuiesced;
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
    getOnReleaseCriticalSectionCustomAction(bool mustClearCollectionMetadata) = 0;

    virtual void refreshCollectionPlacementInfo(OperationContext* opCtx,
                                                const NamespaceString& sourceNss) = 0;

    virtual void abortUnpreparedTransactionIfNecessary(
        OperationContext* opCtx, const boost::optional<ForwardableOperationMetadata>& metadata) = 0;
};

}  // namespace mongo
