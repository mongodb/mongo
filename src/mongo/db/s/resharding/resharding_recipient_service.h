// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/global_catalog/ddl/shard_key_util.h"
#include "mongo/db/hierarchical_cancelable_operation_context_factory.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/primary_only_service_helpers/cancel_state.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/db/s/resharding/resharding_change_streams_monitor.h"
#include "mongo/db/s/resharding/resharding_data_replication.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_metrics.h"
#include "mongo/db/s/resharding/resharding_recipient_promises.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/otel/telemetry_context.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/platform/atomic.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

class [[MONGO_MOD_PUBLIC]] ReshardingRecipientService : public repl::PrimaryOnlyService {
public:
    static constexpr std::string_view kServiceName = "ReshardingRecipientService"sv;

    explicit ReshardingRecipientService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext), _serviceContext(serviceContext) {}
    ~ReshardingRecipientService() override = default;

    class [[MONGO_MOD_PRIVATE]] RecipientStateMachine;

    class [[MONGO_MOD_PRIVATE]] RecipientStateMachineExternalState;

    [[MONGO_MOD_PRIVATE]] std::string_view getServiceName() const override {
        return kServiceName;
    }

    [[MONGO_MOD_PRIVATE]] NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kRecipientReshardingOperationsNamespace;
    }

    [[MONGO_MOD_PRIVATE]] ThreadPoolLimits getThreadPoolLimits() const override;

    // The service implemented its own conflict check before this method was added.
    [[MONGO_MOD_PRIVATE]] void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const repl::PrimaryOnlyService::Instance*>& existingInstances) override {
    };

    [[MONGO_MOD_PRIVATE]] std::shared_ptr<repl::PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) override;

    [[MONGO_MOD_PRIVATE]] inline std::vector<std::shared_ptr<PrimaryOnlyService::Instance>>
    getAllReshardingInstances(OperationContext* opCtx) {
        return getAllInstances(opCtx);
    }

    [[MONGO_MOD_PRIVATE]] void stepDown_forTest();
    [[MONGO_MOD_PRIVATE]] void stepUp_forTest();

private:
    ServiceContext* _serviceContext;
};

/**
 * Represents the current state of a resharding recipient operation on this shard. This class
 * drives state transitions and updates to underlying on-disk metadata.
 */
class ReshardingRecipientService::RecipientStateMachine final
    : public repl::PrimaryOnlyService::TypedInstance<RecipientStateMachine> {
public:
    using CloneDetails = ReshardingRecipientPromises::CloneDetails;

    explicit RecipientStateMachine(
        const ReshardingRecipientService* recipientService,
        const ReshardingRecipientDocument& recipientDoc,
        std::unique_ptr<RecipientStateMachineExternalState> externalState,
        ServiceContext* serviceContext);

    ~RecipientStateMachine() override = default;

    /**
     *  Runs up until the recipient is in state kStrictConsistency or encountered an error.
     */
    ExecutorFuture<void> _runUntilStrictConsistencyOrErrored(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<otel::TelemetryContext> telemetryCtx);

    /**
     * Notifies the coordinator if the recipient is in kStrictConsistency or kError and waits for
     * _coordinatorCommitted to become ready — either successfully (commit) or with an error
     * (coordinator abort via setCoordinatorError() or stepdown via setRunnerError()), or for the
     * abortToken to be canceled.
     */
    ExecutorFuture<void> _notifyCoordinatorAndAwaitDecision(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    /**
     * Finishes the work left remaining on the recipient after the coordinator persists its decision
     * to abort or complete resharding.
     */
    ExecutorFuture<void> _finishReshardingOperation(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& token) noexcept override;

    void interrupt(Status status) override;

    /**
     * Makes the recipient prepare for the critical section.
     */
    void prepareForCriticalSection();

    /**
     * Returns a future that becomes ready once the recipient has majority committed
     * RecipientStateEnum::kCreatingCollection.
     */
    SharedSemiFuture<void> awaitInCreatingCollection() const {
        return _promises.getInCreatingCollectionFuture();
    }

    /**
     * Returns a Future fulfilled once the recipient transitions to RecipientStateEnum::kApplying
     * or RecipientStateEnum::kError and that state change has been majority committed.
     */
    SharedSemiFuture<void> awaitInApplyingOrError() const {
        return _promises.getInApplyingOrErrorFuture();
    }

    /**
     * Returns a Future fulfilled once the recipient majority commits its final state before the
     * coordinator makes its decision to commit or abort (RecipientStateEnum::kStrictConsistency
     * or RecipientStateEnum::kError).
     */
    SharedSemiFuture<void> awaitInStrictConsistencyOrError() const {
        return _promises.getInStrictConsistencyOrErrorFuture();
    }

    /**
     * Returns a Future that will be resolved when all work associated with this Instance is done
     * making forward progress.
     */
    SharedSemiFuture<void> getCompletionFuture() const {
        // coverity[missing_lock]
        return _completionPromise.getFuture();
    }

    /**
     * Waits for the monitor to start. Throws an error if verification
     * is not enabled or skipCloningAndApplying is true.
     */
    SharedSemiFuture<void> awaitChangeStreamsMonitorStartedForTest();

    /**
     * Waits for the monitor to complete and returns the final document delta from the applying
     * phase. Throws an error if verification is not enabled or skipCloningAndApplying is true.
     */
    SharedSemiFuture<int64_t> awaitChangeStreamsMonitorCompleted();

    inline const CommonReshardingMetadata& getMetadata() const {
        return _metadata;
    }

    inline const ReshardingMetrics& getMetrics() const {
        invariant(_metrics);
        return *_metrics;
    }

    ReshardingMetrics& getMetricsForTest() const {
        invariant(_metrics);
        return *_metrics;
    }

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode,
        MongoProcessInterface::CurrentOpSessionsMode) noexcept override;

    void onReshardingFieldsChanges(OperationContext* opCtx,
                                   const TypeCollectionReshardingFields& reshardingFields,
                                   bool noChunksToCopy);

    /**
     * Fulfills the subset of recipient promises that are driven by the coordinator advancing
     * through its state machine. Shared entry point for command handlers and
     * onReshardingFieldsChanges. Idempotent and cascading: calling with the same state multiple
     * times is safe, and calling with a later state fulfills all promises up to and including that
     * state (via >= checks in ReshardingRecipientPromises::onCoordinatorStateAdvanced).
     *
     * Promises fulfilled here:
     *   newState >= kCloning && cloneDetails -> _allDonorsPreparedToDonate
     *   newState >= kBlockingWrites          -> _coordinatorBlockingWrites
     *   newState >= kCommitting              -> _coordinatorCommitted
     *
     * Side effects:
     *   newState == kBlockingWrites          -> _dataReplication->prepareForCriticalSection()
     */
    void onCoordinatorStateAdvanced(CoordinatorStateEnum newState,
                                    boost::optional<CloneDetails> cloneDetails = boost::none);

    static void insertStateDocument(OperationContext* opCtx,
                                    const ReshardingRecipientDocument& recipientDoc);

    /**
     * Indicates that the coordinator has committed. Unblocks the _coordinatorCommitted promise.
     */
    void commit();

    /**
     * Initiates the cancellation of the resharding operation.
     */
    void abort(bool isUserCancelled);

    void checkIfOptionsConflict(const BSONObj& stateDoc) const final {}

private:
    /**
     * With-lock implementation of onCoordinatorStateAdvanced. Used by callers that already hold
     * _mutex (e.g. onReshardingFieldsChanges).
     */
    void _onCoordinatorStateAdvanced(WithLock lk,
                                     CoordinatorStateEnum newState,
                                     boost::optional<CloneDetails> cloneDetails);

    class CloningMetrics {
    public:
        void add(int64_t documentsCopied, int64_t bytesCopied);

        int64_t getDocumentsCopied() const {
            return _documentsCopied;
        }

        int64_t getBytesCopied() const {
            return _bytesCopied;
        }

    private:
        int64_t _documentsCopied = 0;
        int64_t _bytesCopied = 0;
    };

    using ShardApplierProgress = std::map<ShardId, ReshardingOplogApplierProgress>;

    /**
     * The work inside this function must be run regardless of any work on _scopedExecutor ever
     * running.
     */
    ExecutorFuture<void> _runMandatoryCleanup(Status status);

    /**
     * Helper to construct an opCtx and set non-deprioritizable state if needed.
     */
    CancelableOperationContext _makeOperationContext(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) const;

    // The following functions correspond to the actions to take at a particular recipient state.
    ExecutorFuture<void> _awaitAllDonorsPreparedToDonateThenTransitionToCreatingCollection(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    void _createTemporaryReshardingCollectionThenTransitionToCloning(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    ExecutorFuture<void> _cloneThenTransitionToBuildingIndex(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    ExecutorFuture<void> _buildIndexThenTransitionToApplying(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    ExecutorFuture<void> _awaitAllDonorsBlockingWritesThenTransitionToStrictConsistency(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    bool _hasAlreadyWrittenStrictConsistencyOplog(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    void _writeStrictConsistencyOplog(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    void _renameTemporaryReshardingCollection(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    void _cleanupReshardingCollections(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    void _verifyIndexesBuilt(OperationContext* opCtx, bool shardKeyIndexAdded);
    void _verifyCollectionOptions(OperationContext* opCtx);

    // Transitions the on-disk and in-memory state to 'newState'.
    void _transitionState(RecipientStateEnum newState,
                          std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    // Transitions the on-disk and in-memory state to the state defined in 'newRecipientCtx'.
    void _transitionState(RecipientShardContext&& newRecipientCtx,
                          boost::optional<CloneDetails>&& cloneDetails,
                          boost::optional<mongo::Date_t> configStartTime,
                          std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    // The following functions transition the on-disk and in-memory state to the named state.
    void _transitionToCreatingCollection(
        CloneDetails cloneDetails,
        boost::optional<mongo::Date_t> startConfigTxnCloneTime,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    void _transitionToApplying(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    void _transitionToError(Status abortReason,
                            std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    void _transitionToDone(std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    BSONObj _makeQueryForCoordinatorUpdate(const ShardId& shardId, RecipientStateEnum newState);

    ExecutorFuture<void> _updateCoordinator(
        OperationContext* opCtx,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    // Updates the mutable portion of the on-disk and in-memory recipient document with
    // 'newRecipientCtx', 'fetchTimestamp and 'donorShards'.
    void _updateRecipientDocument(
        RecipientShardContext&& newRecipientCtx,
        boost::optional<CloneDetails>&& cloneDetails,
        boost::optional<mongo::Date_t> configStartTime,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    void _updateRecipientDocument(
        ChangeStreamsMonitorContext newChangeStreamsCtx,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    // Removes the local recipient document from disk.
    void _removeRecipientDocument(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    void _ensureDataReplicationStarted(
        OperationContext* opCtx,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    void _createAndStartChangeStreamsMonitor(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    void _createChangeStreamsMonitor(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    ExecutorFuture<void> _awaitChangeStreamsMonitorCompleted(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    ExecutorFuture<void> _startMetrics(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    // Restore metrics using the persisted metrics after stepping up.
    ExecutorFuture<void> _restoreMetricsWithRetry(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);
    void _restoreMetrics(std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory);

    void _initializeShardApplierMetrics(
        const boost::optional<ShardApplierProgress>& existingProgress);

    void _initializeDataReplication();

    void _updateContextMetrics(OperationContext* opCtx);

    // Get indexesToBuild and indexesBuilt from the index catalog, then save them in _metrics
    void _tryFetchBuildIndexMetrics(OperationContext* opCtx);

    // Return the total and per-donor number documents and bytes cloned if the numbers are available
    // in the cloner resume data documents. Otherwise, return none.
    boost::optional<CloningMetrics> _tryFetchCloningMetrics(OperationContext* opCtx);

    void _fulfillPromisesOnStepup(const ReshardingRecipientDocument&);

    /**
     * Creates a new span with the resharding UUID set as an attribute.
     */
    otel::traces::Span _startSpan(std::shared_ptr<otel::TelemetryContext> telemetryCtx,
                                  otel::traces::SpanName spanName);

    // The primary-only service instance corresponding to the recipient instance. Not owned.
    const ReshardingRecipientService* const _recipientService;

    ServiceContext* _serviceContext;

    std::unique_ptr<ReshardingMetrics> _metrics;
    ReshardingApplierMetricsMap _applierMetricsMap;

    // The in-memory representation of the immutable portion of the document in
    // config.localReshardingOperations.recipient.
    const CommonReshardingMetadata _metadata;

    // Cached copy of _metadata's ForwardableOperationMetadata with cross-shard propagation enabled.
    const boost::optional<ForwardableOperationMetadata> _forwardableOpMetadata;
    const Milliseconds _minimumOperationDuration;
    const boost::optional<std::size_t> _oplogBatchTaskCount;
    // Set to true if this recipient should skip cloning documents and fetching/applying oplog
    // entries because it is not going to own any chunks for the collection after resharding.
    const bool _skipCloningAndApplying;
    // Set to true if this recipient should skip cloning documents because it is not going to own
    // any chunks for the collection after resharding.
    const bool _skipCloning;
    // Set to true if this recipient should skip building indexes because it is not going to own any
    // chunks for the collection after resharding.
    const bool _skipBuildingIndexes;
    // Set to true if this recipient should store the count of oplog entries fetched in a progress
    // document and use this count instead of the fast count to recover metrics upon recovery
    const bool _storeOplogFetcherProgress;
    // Set to true if this recipient should run cloner aggregation without specifying a collection
    // UUID to avoid errors in a scenario where the collection UUIDs are inconsistent among shards.
    const OptionalBool _relaxed;

    // The in-memory representation of the mutable portion of the document in
    // config.localReshardingOperations.recipient.
    RecipientShardContext _recipientCtx;
    boost::optional<ChangeStreamsMonitorContext> _changeStreamsMonitorCtx;
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
    boost::optional<resharding::RetryingCancelableOperationContextFactory> _finishOperationFactory;

    SharedSemiFuture<void> _dataReplicationQuiesced;

    SharedPromise<void> _changeStreamsMonitorStarted;
    SharedPromise<int64_t> _changeStreamsMonitorCompleted;
    SharedSemiFuture<void> _changeStreamsMonitorQuiesced;

    // Protects the state below
    mutable std::mutex _mutex;

    // Manages abort state and provides cancellation tokens for async operations.
    primary_only_service_helpers::CancelState _cancelState;

    std::unique_ptr<ReshardingDataReplicationInterface> _dataReplication;
    std::shared_ptr<ReshardingChangeStreamsMonitor> _changeStreamsMonitor;

    // The identifier associated to the recoverable critical section.
    const BSONObj _critSecReason;

    // It states whether the current node has also the donor role.
    const bool _isAlsoDonor;

    // It states whether or not the user has aborted the resharding operation.
    boost::optional<bool> _userCanceled;

    Atomic<long long> _lastWriteBlockWarningAt{std::numeric_limits<long long>::min()};

    ReshardingRecipientPromises _promises;

    SharedPromise<void> _completionPromise;
};

}  // namespace mongo
