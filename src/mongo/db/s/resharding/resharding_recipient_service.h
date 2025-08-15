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
#include "mongo/db/global_catalog/ddl/shard_key_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/db/s/resharding/resharding_change_streams_monitor.h"
#include "mongo/db/s/resharding/resharding_data_replication.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_metrics.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class ReshardingRecipientService : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "ReshardingRecipientService"_sd;

    explicit ReshardingRecipientService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext), _serviceContext(serviceContext) {}
    ~ReshardingRecipientService() override = default;

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
        const std::vector<const repl::PrimaryOnlyService::Instance*>& existingInstances) override {
    };

    std::shared_ptr<repl::PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) override;

    inline std::vector<std::shared_ptr<PrimaryOnlyService::Instance>> getAllReshardingInstances(
        OperationContext* opCtx) {
        return getAllInstances(opCtx);
    }

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
        ServiceContext* serviceContext);

    ~RecipientStateMachine() override = default;

    /**
     *  Runs up until the recipient is in state kStrictConsistency or encountered an error.
     */
    ExecutorFuture<void> _runUntilStrictConsistencyOrErrored(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken);

    /**
     * Notifies the coordinator if the recipient is in kStrictConsistency or kError and waits for
     * _coordinatorHasDecisionPersisted to be fulfilled (success) or for the abortToken to be
     * canceled (failure or stepdown).
     */
    ExecutorFuture<void> _notifyCoordinatorAndAwaitDecision(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken);

    /**
     * Finishes the work left remaining on the recipient after the coordinator persists its decision
     * to abort or complete resharding.
     */
    ExecutorFuture<void> _finishReshardingOperation(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& stepdownToken,
        bool aborted);

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& token) noexcept override;

    void interrupt(Status status) override;

    /**
     * Makes the recipient prepare for the critical section.
     */
    void prepareForCriticalSection();

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

    /**
     * Waits for the monitor to start. Throws an error if verification
     * is not enabled or skipCloningAndApplying is true.
     */
    SharedSemiFuture<void> awaitChangeStreamsMonitorStartedForTest();

    /**
     * Waits for the monitor to complete and returns the final document delta from the applying
     * phase. Throws an error if verification is not enabled or skipCloningAndApplying is true.
     */
    SharedSemiFuture<int64_t> awaitChangeStreamsMonitorCompletedForTest();

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

    SemiFuture<void> fulfillAllDonorsPreparedToDonate(CloneDetails cloneDetails,
                                                      const CancellationToken& cancelToken);

private:
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
    ExecutorFuture<void> _runMandatoryCleanup(Status status,
                                              const CancellationToken& stepdownToken);

    // The following functions correspond to the actions to take at a particular recipient state.
    ExecutorFuture<void> _awaitAllDonorsPreparedToDonateThenTransitionToCreatingCollection(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken,
        const CancelableOperationContextFactory& factory);

    void _createTemporaryReshardingCollectionThenTransitionToCloning(
        const CancelableOperationContextFactory& factory);

    ExecutorFuture<void> _cloneThenTransitionToBuildingIndex(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken,
        const CancelableOperationContextFactory& factory);

    ExecutorFuture<void> _buildIndexThenTransitionToApplying(
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

    // Transitions the on-disk and in-memory state to the state defined in 'newRecipientCtx'.
    void _transitionState(RecipientShardContext&& newRecipientCtx,
                          boost::optional<CloneDetails>&& cloneDetails,
                          boost::optional<mongo::Date_t> configStartTime,
                          const CancelableOperationContextFactory& factory);

    // The following functions transition the on-disk and in-memory state to the named state.
    void _transitionToCreatingCollection(CloneDetails cloneDetails,
                                         boost::optional<mongo::Date_t> startConfigTxnCloneTime,
                                         const CancelableOperationContextFactory& factory);

    void _transitionToApplying(const CancelableOperationContextFactory& factory);

    void _transitionToError(Status abortReason, const CancelableOperationContextFactory& factory);

    void _transitionToDone(bool aborted, const CancelableOperationContextFactory& factory);

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

    void _updateRecipientDocument(ChangeStreamsMonitorContext newChangeStreamsCtx,
                                  const CancelableOperationContextFactory& factory);

    // Removes the local recipient document from disk.
    void _removeRecipientDocument(bool aborted, const CancelableOperationContextFactory& factory);

    void _ensureDataReplicationStarted(
        OperationContext* opCtx,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken,
        const CancelableOperationContextFactory& factory);

    void _createAndStartChangeStreamsMonitor(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken,
        const CancelableOperationContextFactory& factory);

    ExecutorFuture<void> _awaitChangeStreamsMonitorCompleted(
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

    void _initializeShardApplierMetrics(
        const boost::optional<ShardApplierProgress>& existingProgress);

    void _initializeDataReplication();

    void _updateContextMetrics(OperationContext* opCtx);

    // Initializes the _abortSource and generates a token from it to return back the caller.
    //
    // Should only be called once per lifetime.
    CancellationToken _initAbortSource(const CancellationToken& stepdownToken);

    // Get indexesToBuild and indexesBuilt from the index catalog, then save them in _metrics
    void _tryFetchBuildIndexMetrics(OperationContext* opCtx);

    // Return the total and per-donor number documents and bytes cloned if the numbers are available
    // in the cloner resume data documents. Otherwise, return none.
    boost::optional<CloningMetrics> _tryFetchCloningMetrics(OperationContext* opCtx);

    void _fulfillPromisesOnStepup(boost::optional<mongo::ReshardingRecipientMetrics> metrics);

    // The primary-only service instance corresponding to the recipient instance. Not owned.
    const ReshardingRecipientService* const _recipientService;

    ServiceContext* _serviceContext;

    std::unique_ptr<ReshardingMetrics> _metrics;
    ReshardingApplierMetricsMap _applierMetricsMap;

    // The in-memory representation of the immutable portion of the document in
    // config.localReshardingOperations.recipient.
    const CommonReshardingMetadata _metadata;
    const Milliseconds _minimumOperationDuration;
    const boost::optional<std::size_t> _oplogBatchTaskCount;
    // Set to true if this recipient should skip cloning documents and fetching/applying oplog
    // entries because it is not going to own any chunks for the collection after resharding.
    const bool _skipCloningAndApplying;
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

    SharedSemiFuture<void> _dataReplicationQuiesced;

    SharedPromise<void> _changeStreamsMonitorStarted;
    SharedPromise<int64_t> _changeStreamsMonitorCompleted;
    SharedSemiFuture<void> _changeStreamsMonitorQuiesced;

    // Protects the state below
    stdx::mutex _mutex;

    std::unique_ptr<ReshardingDataReplicationInterface> _dataReplication;
    std::shared_ptr<ReshardingChangeStreamsMonitor> _changeStreamsMonitor;

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

    // This promise is emplaced if the recipient has majority committed the createCollection state.
    SharedPromise<void> _transitionedToCreateCollection;
};

}  // namespace mongo
