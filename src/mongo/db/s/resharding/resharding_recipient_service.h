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
#include "mongo/db/s/resharding/resharding_critical_section.h"
#include "mongo/db/s/resharding/resharding_oplog_applier.h"
#include "mongo/db/s/resharding/resharding_oplog_fetcher.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {

class ReshardingCollectionCloner;
class ReshardingTxnCloner;

namespace resharding {

/**
 * Creates the temporary resharding collection locally by loading the collection options and
 * collection indexes from the original collection's primary and MinKey owning chunk shards,
 * respectively.
 */
void createTemporaryReshardingCollectionLocally(OperationContext* opCtx,
                                                const NamespaceString& originalNss,
                                                const NamespaceString& reshardingNss,
                                                const UUID& reshardingUUID,
                                                const UUID& existingUUID,
                                                Timestamp fetchTimestamp);

std::vector<NamespaceString> ensureStashCollectionsExist(OperationContext* opCtx,
                                                         const ChunkManager& cm,
                                                         const UUID& existingUUID,
                                                         std::vector<ShardId> donorShards);

ReshardingDonorOplogId getFetcherIdToResumeFrom(OperationContext* opCtx,
                                                NamespaceString oplogBufferNss,
                                                Timestamp fetchTimestamp);

ReshardingDonorOplogId getApplierIdToResumeFrom(OperationContext* opCtx,
                                                ReshardingSourceId sourceId,
                                                Timestamp fetchTimestamp);
}  // namespace resharding

class ReshardingRecipientService final : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "ReshardingRecipientService"_sd;

    explicit ReshardingRecipientService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext) {}
    ~ReshardingRecipientService() = default;

    class RecipientStateMachine;

    StringData getServiceName() const override {
        return kServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kRecipientReshardingOperationsNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const override {
        // TODO Limit the size of ReshardingRecipientService thread pool.
        return ThreadPool::Limits();
    }

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) const override;
};

/**
 * Represents the current state of a resharding recipient operation on this shard. This class
 * drives state transitions and updates to underlying on-disk metadata.
 */
class ReshardingRecipientService::RecipientStateMachine final
    : public repl::PrimaryOnlyService::TypedInstance<RecipientStateMachine> {
public:
    explicit RecipientStateMachine(const BSONObj& recipientDoc);

    ~RecipientStateMachine();

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancelationToken& token) noexcept override;

    void interrupt(Status status) override;

    /**
     * Returns a Future that will be resolved when all work associated with this Instance has
     * completed running.
     */
    SharedSemiFuture<void> getCompletionFuture() const {
        return _completionPromise.getFuture();
    }

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode,
        MongoProcessInterface::CurrentOpSessionsMode) noexcept override;

    void onReshardingFieldsChanges(OperationContext* opCtx,
                                   const TypeCollectionReshardingFields& reshardingFields);

    static void insertStateDocument(OperationContext* opCtx,
                                    const ReshardingRecipientDocument& recipientDoc);

private:
    RecipientStateMachine(const ReshardingRecipientDocument& recipientDoc);

    // The following functions correspond to the actions to take at a particular recipient state.
    ExecutorFuture<void> _awaitAllDonorsPreparedToDonateThenTransitionToCreatingCollection(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    void _createTemporaryReshardingCollectionThenTransitionToCloning();

    ExecutorFuture<void> _cloneThenTransitionToApplying(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancelationToken& abortToken);

    ExecutorFuture<void> _applyThenTransitionToSteadyState(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    ExecutorFuture<void> _awaitAllDonorsBlockingWritesThenTransitionToStrictConsistency(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancelationToken& abortToken);

    ExecutorFuture<void> _awaitCoordinatorHasDecisionPersistedThenTransitionToRenaming(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    void _renameTemporaryReshardingCollection();

    // Transitions the on-disk and in-memory state to 'newState'.
    void _transitionState(RecipientStateEnum newState);

    void _transitionState(RecipientShardContext&& newRecipientCtx,
                          boost::optional<Timestamp>&& fetchTimestamp);

    // Transitions the on-disk and in-memory state to RecipientStateEnum::kCreatingCollection.
    void _transitionToCreatingCollection(Timestamp fetchTimestamp);

    // Transitions the on-disk and in-memory state to RecipientStateEnum::kError.
    void _transitionToError(Status abortReason);

    BSONObj _makeQueryForCoordinatorUpdate(const ShardId& shardId, RecipientStateEnum newState);

    ExecutorFuture<void> _updateCoordinator(
        OperationContext* opCtx, const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    // Updates the mutable portion of the on-disk and in-memory recipient document with
    // 'newRecipientCtx' and 'fetchTimestamp'.
    void _updateRecipientDocument(RecipientShardContext&& newRecipientCtx,
                                  boost::optional<Timestamp>&& fetchTimestamp);

    // Removes the local recipient document from disk.
    void _removeRecipientDocument();

    // Removes any docs from the oplog applier progress and txn applier progress collections that
    // are associated with the in-progress operation. Also drops all oplog buffer collections and
    // all conflict stash collections that are associated with the in-progress operation.
    void _dropOplogCollections(OperationContext* opCtx);

    // Initializes the txn cloners for this resharding operation.
    void _initTxnCloner(OperationContext* opCtx, const Timestamp& fetchTimestamp);

    ReshardingMetrics* _metrics() const;

    // Does work necessary for both recoverable errors (failover/stepdown) and unrecoverable errors
    // (abort resharding).
    void _onAbortOrStepdown(WithLock, Status status);

    // Initializes the _abortSource and generates a token from it to return back the caller.
    //
    // Should only be called once per lifetime.
    CancelationToken _initAbortSource(const CancelationToken& stepdownToken);

    // The in-memory representation of the immutable portion of the document in
    // config.localReshardingOperations.recipient.
    const CommonReshardingMetadata _metadata;
    const std::vector<ShardId> _donorShardIds;
    const Milliseconds _minimumOperationDuration;

    // The in-memory representation of the mutable portion of the document in
    // config.localReshardingOperations.recipient.
    RecipientShardContext _recipientCtx;
    boost::optional<Timestamp> _fetchTimestamp;

    std::unique_ptr<ReshardingCollectionCloner> _collectionCloner;
    std::vector<std::unique_ptr<ReshardingTxnCloner>> _txnCloners;

    std::vector<std::unique_ptr<ReshardingOplogApplier>> _oplogAppliers;
    std::vector<std::unique_ptr<ThreadPool>> _oplogApplierWorkers;

    // The ReshardingOplogFetcher must be destructed before the corresponding ReshardingOplogApplier
    // to ensure the future returned by awaitInsert() is always eventually readied.
    std::vector<std::unique_ptr<ReshardingOplogFetcher>> _oplogFetchers;
    std::shared_ptr<executor::TaskExecutor> _oplogFetcherExecutor;
    std::vector<ExecutorFuture<void>> _oplogFetcherFutures;

    // Protects the promises below
    Mutex _mutex = MONGO_MAKE_LATCH("RecipientStateMachine::_mutex");

    // Canceled when there is an unrecoverable error or stepdown.
    boost::optional<CancelationSource> _abortSource;

    boost::optional<ReshardingCriticalSection> _critSec;

    // Each promise below corresponds to a state on the recipient state machine. They are listed in
    // ascending order, such that the first promise below will be the first promise fulfilled.
    SharedPromise<Timestamp> _allDonorsPreparedToDonate;

    SharedPromise<void> _coordinatorHasDecisionPersisted;

    SharedPromise<void> _completionPromise;
};

}  // namespace mongo
