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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingMigration

#include "mongo/db/s/resharding/resharding_recipient_service.h"

#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/resharding/resharding_collection_cloner.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_shard_version_helpers.h"
#include "mongo/util/future_util.h"

namespace mongo {

namespace {

std::shared_ptr<executor::ThreadPoolTaskExecutor> makeTaskExecutor(StringData name,
                                                                   size_t maxThreads) {
    ThreadPool::Limits threadPoolLimits;
    threadPoolLimits.maxThreads = maxThreads;

    ThreadPool::Options threadPoolOptions(std::move(threadPoolLimits));
    threadPoolOptions.threadNamePrefix = name + "-";
    threadPoolOptions.poolName = name + "ThreadPool";

    auto executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(std::move(threadPoolOptions)),
        executor::makeNetworkInterface(name + "Network"));

    executor->startup();
    return executor;
}

ChunkManager getShardedCollectionRoutingInfo(OperationContext* opCtx, const NamespaceString& nss) {
    auto* catalogCache = Grid::get(opCtx)->catalogCache();
    auto cm = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));

    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Expected collection " << nss << " to be sharded",
            cm.isSharded());

    return cm;
}

}  // namespace

namespace resharding {

void createTemporaryReshardingCollectionLocally(OperationContext* opCtx,
                                                const NamespaceString& originalNss,
                                                const UUID& reshardingUUID,
                                                const UUID& existingUUID,
                                                Timestamp fetchTimestamp) {
    LOGV2_DEBUG(
        5002300, 1, "Creating temporary resharding collection", "originalNss"_attr = originalNss);

    auto catalogCache = Grid::get(opCtx)->catalogCache();
    auto reshardingNss = constructTemporaryReshardingNss(originalNss.db(), existingUUID);

    // Load the original collection's options from the database's primary shard.
    auto [collOptions, uuid] =
        shardVersionRetry(opCtx,
                          catalogCache,
                          reshardingNss,
                          "loading collection options to create temporary resharding collection"_sd,
                          [&]() -> MigrationDestinationManager::CollectionOptionsAndUUID {
                              auto originalCm = uassertStatusOK(
                                  catalogCache->getCollectionRoutingInfo(opCtx, originalNss));
                              return MigrationDestinationManager::getCollectionOptions(
                                  opCtx,
                                  NamespaceStringOrUUID(originalNss.db().toString(), existingUUID),
                                  originalCm.dbPrimary(),
                                  originalCm,
                                  fetchTimestamp);
                          });

    // Load the original collection's indexes from the shard that owns the global minimum chunk.
    auto [indexes, idIndex] =
        shardVersionRetry(opCtx,
                          catalogCache,
                          reshardingNss,
                          "loading indexes to create temporary resharding collection"_sd,
                          [&]() -> MigrationDestinationManager::IndexesAndIdIndex {
                              auto originalCm = getShardedCollectionRoutingInfo(opCtx, originalNss);
                              auto indexShardId = originalCm.getMinKeyShardIdWithSimpleCollation();
                              return MigrationDestinationManager::getCollectionIndexes(
                                  opCtx,
                                  NamespaceStringOrUUID(originalNss.db().toString(), existingUUID),
                                  indexShardId,
                                  originalCm,
                                  fetchTimestamp);
                          });

    // Set the temporary resharding collection's UUID to the resharding UUID. Note that
    // BSONObj::addFields() replaces any fields that already exist.
    collOptions = collOptions.addFields(BSON("uuid" << reshardingUUID));

    CollectionOptionsAndIndexes optionsAndIndexes = {reshardingUUID, indexes, idIndex, collOptions};
    MigrationDestinationManager::cloneCollectionIndexesAndOptions(
        opCtx, reshardingNss, optionsAndIndexes);
}

}  // namespace resharding

std::shared_ptr<repl::PrimaryOnlyService::Instance> ReshardingRecipientService::constructInstance(
    BSONObj initialState) const {
    return std::make_shared<RecipientStateMachine>(std::move(initialState));
}

ReshardingRecipientService::RecipientStateMachine::RecipientStateMachine(
    const BSONObj& recipientDoc)
    : repl::PrimaryOnlyService::TypedInstance<RecipientStateMachine>(),
      _recipientDoc(ReshardingRecipientDocument::parse(
          IDLParserErrorContext("ReshardingRecipientDocument"), recipientDoc)),
      _id(_recipientDoc.getCommonReshardingMetadata().get_id()) {}

ReshardingRecipientService::RecipientStateMachine::~RecipientStateMachine() {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_allDonorsMirroring.getFuture().isReady());
    invariant(_coordinatorHasCommitted.getFuture().isReady());
    invariant(_completionPromise.getFuture().isReady());
}

SemiFuture<void> ReshardingRecipientService::RecipientStateMachine::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancelationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this] { _transitionToCreatingTemporaryReshardingCollection(); })
        .then([this] { _createTemporaryReshardingCollectionThenTransitionToCloning(); })
        .then([this, executor] { return _cloneThenTransitionToApplying(executor); })
        .then([this] { return _applyThenTransitionToSteadyState(); })
        .then([this, executor] {
            return _awaitAllDonorsMirroringThenTransitionToStrictConsistency(executor);
        })
        .then([this, executor] {
            return _awaitCoordinatorHasCommittedThenTransitionToRenaming(executor);
        })
        .then([this] { _renameTemporaryReshardingCollectionThenDeleteLocalState(); })
        .onError([this](Status status) {
            LOGV2(4956500,
                  "Resharding operation recipient state machine failed",
                  "namespace"_attr = _recipientDoc.getNss().ns(),
                  "reshardingId"_attr = _id,
                  "error"_attr = status);
            // TODO SERVER-50584 Report errors to the coordinator so that the resharding operation
            // can be aborted.
            this->_transitionStateToError(status);
            return status;
        })
        .onCompletion([this](Status status) {
            stdx::lock_guard<Latch> lg(_mutex);
            if (_completionPromise.getFuture().isReady()) {
                // interrupt() was called before we got her.e
                return;
            }

            if (status.isOK()) {
                _completionPromise.emplaceValue();
            } else {
                // Set error on all promises
                _completionPromise.setError(status);
            }
        })
        .semi();
}

void ReshardingRecipientService::RecipientStateMachine::interrupt(Status status) {
    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<Latch> lg(_mutex);

    if (_oplogFetcherExecutor) {
        _oplogFetcherExecutor->shutdown();
    }

    if (_oplogApplierExecutor) {
        _oplogApplierExecutor->shutdown();
    }

    for (auto&& threadPool : _oplogApplierWorkers) {
        threadPool->shutdown();
    }

    if (!_allDonorsMirroring.getFuture().isReady()) {
        _allDonorsMirroring.setError(status);
    }

    if (!_coordinatorHasCommitted.getFuture().isReady()) {
        _coordinatorHasCommitted.setError(status);
    }

    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

void ReshardingRecipientService::RecipientStateMachine::onReshardingFieldsChanges(
    boost::optional<TypeCollectionReshardingFields> reshardingFields) {}

void ReshardingRecipientService::RecipientStateMachine::
    _transitionToCreatingTemporaryReshardingCollection() {
    if (_recipientDoc.getState() > RecipientStateEnum::kCreatingCollection) {
        return;
    }

    _transitionState(RecipientStateEnum::kCreatingCollection);
}

void ReshardingRecipientService::RecipientStateMachine::
    _createTemporaryReshardingCollectionThenTransitionToCloning() {
    if (_recipientDoc.getState() > RecipientStateEnum::kCreatingCollection) {
        return;
    }

    {
        auto opCtx = cc().makeOperationContext();
        resharding::createTemporaryReshardingCollectionLocally(opCtx.get(),
                                                               _recipientDoc.getNss(),
                                                               _recipientDoc.get_id(),
                                                               _recipientDoc.getExistingUUID(),
                                                               *_recipientDoc.getFetchTimestamp());
    }

    _transitionState(RecipientStateEnum::kCloning);
}

ExecutorFuture<void>
ReshardingRecipientService::RecipientStateMachine::_cloneThenTransitionToApplying(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_recipientDoc.getState() > RecipientStateEnum::kCloning) {
        return ExecutorFuture(**executor);
    }

    auto* serviceContext = Client::getCurrent()->getServiceContext();
    auto tempNss = constructTemporaryReshardingNss(_recipientDoc.getNss().db(),
                                                   _recipientDoc.getExistingUUID());

    _collectionCloner = std::make_unique<ReshardingCollectionCloner>(
        ShardKeyPattern(_recipientDoc.getReshardingKey()),
        _recipientDoc.getNss(),
        _recipientDoc.getExistingUUID(),
        ShardingState::get(serviceContext)->shardId(),
        *_recipientDoc.getFetchTimestamp(),
        std::move(tempNss));

    auto numDonors = _recipientDoc.getDonorShardsMirroring().size();
    _oplogFetchers.reserve(numDonors);
    _oplogFetcherFutures.reserve(numDonors);

    {
        stdx::lock_guard<Latch> lk(_mutex);
        _oplogFetcherExecutor = makeTaskExecutor("ReshardingOplogFetcher"_sd, numDonors);
    }

    const auto& minKeyChunkOwningShardId = [&] {
        auto opCtx = cc().makeOperationContext();
        auto sourceChunkMgr = getShardedCollectionRoutingInfo(opCtx.get(), _recipientDoc.getNss());
        return sourceChunkMgr.getMinKeyShardIdWithSimpleCollation();
    }();

    const auto& recipientId = ShardingState::get(serviceContext)->shardId();
    for (const auto& donor : _recipientDoc.getDonorShardsMirroring()) {
        _oplogFetchers.emplace_back(std::make_unique<ReshardingOplogFetcher>(
            _recipientDoc.get_id(),
            _recipientDoc.getExistingUUID(),
            // The recipient fetches oplog entries from the donor starting from the fetchTimestamp,
            // which corresponds to {clusterTime: fetchTimestamp, ts: fetchTimestamp} as a resume
            // token value.
            ReshardingDonorOplogId{*_recipientDoc.getFetchTimestamp(),
                                   *_recipientDoc.getFetchTimestamp()},
            donor.getId(),
            recipientId,
            donor.getId() == minKeyChunkOwningShardId,
            getLocalOplogBufferNamespace(_recipientDoc.get_id(), donor.getId())));

        _oplogFetcherFutures.emplace_back(
            _oplogFetchers.back()->schedule(_oplogFetcherExecutor.get()).onError([](Status status) {
                LOGV2(5259300, "Error fetching oplog entries", "error"_attr = redact(status));
                return status;
            }));
    }

    return _collectionCloner->run(serviceContext, **executor).then([this] {
        _transitionStateAndUpdateCoordinator(RecipientStateEnum::kApplying);
    });
}

void ReshardingRecipientService::RecipientStateMachine::_applyThenTransitionToSteadyState() {
    if (_recipientDoc.getState() > RecipientStateEnum::kApplying) {
        return;
    }

    // The contents of the temporary resharding collection are already consistent because the
    // ReshardingCollectionCloner uses atClusterTime. Using replication's initial sync nomenclature,
    // resharding has immediately finished the "apply phase" as soon as the
    // ReshardingCollectionCloner has finished. This is why it is acceptable to not call
    // applyUntilCloneFinishedTs() here and to only do so in
    // _awaitAllDonorsMirroringThenTransitionToStrictConsistency() instead.
    //
    // TODO: Consider removing _applyThenTransitionToSteadyState() and changing
    // _cloneThenTransitionToApplying() to call _transitionStateAndUpdateCoordinator(kSteadyState).

    _transitionStateAndUpdateCoordinator(RecipientStateEnum::kSteadyState);

    // Unless a test is prepared to write the final oplog entries itself, without this interrupt(),
    // the futures returned by ReshardingOplogFetcher::schedule() would never become ready.
    //
    // TODO SERVER-52795: Remove once the donor shards write the final oplog entry themselves.
    if (resharding::gReshardingTempInterruptBeforeOplogApplication) {
        interrupt({ErrorCodes::InternalError, "Artificial interruption to enable jsTests"});
    }
}

ExecutorFuture<void> ReshardingRecipientService::RecipientStateMachine::
    _awaitAllDonorsMirroringThenTransitionToStrictConsistency(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_recipientDoc.getState() > RecipientStateEnum::kSteadyState) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    auto numDonors = _recipientDoc.getDonorShardsMirroring().size();
    _oplogAppliers.reserve(numDonors);
    _oplogApplierWorkers.reserve(numDonors);

    {
        stdx::lock_guard<Latch> lk(_mutex);
        _oplogApplierExecutor = makeTaskExecutor("ReshardingOplogApplier"_sd, numDonors);
    }

    auto* serviceContext = Client::getCurrent()->getServiceContext();
    const auto& sourceChunkMgr = [&] {
        auto opCtx = cc().makeOperationContext();
        return getShardedCollectionRoutingInfo(opCtx.get(), _recipientDoc.getNss());
    }();

    size_t i = 0;
    auto futuresToWaitOn = std::move(_oplogFetcherFutures);
    for (const auto& donor : _recipientDoc.getDonorShardsMirroring()) {
        {
            stdx::lock_guard<Latch> lk(_mutex);
            _oplogApplierWorkers.emplace_back(
                repl::makeReplWriterPool(resharding::gReshardingWriterThreadCount,
                                         "ReshardingOplogApplierWorker",
                                         true /* isKillableByStepdown */));
        }

        auto oplogBufferNss = getLocalOplogBufferNamespace(_recipientDoc.get_id(), donor.getId());
        _oplogAppliers.emplace_back(std::make_unique<ReshardingOplogApplier>(
            serviceContext,
            ReshardingSourceId{_recipientDoc.get_id(), donor.getId()},
            oplogBufferNss,
            _recipientDoc.getNss(),
            _recipientDoc.getExistingUUID(),
            *_recipientDoc.getFetchTimestamp(),
            // The recipient applies oplog entries from the donor starting from the fetchTimestamp,
            // which corresponds to {clusterTime: fetchTimestamp, ts: fetchTimestamp} as a resume
            // token value.
            std::make_unique<ReshardingDonorOplogIterator>(
                oplogBufferNss,
                ReshardingDonorOplogId{*_recipientDoc.getFetchTimestamp(),
                                       *_recipientDoc.getFetchTimestamp()},
                _oplogFetchers[i].get()),
            resharding::gReshardingBatchLimitOperations,
            sourceChunkMgr,
            _oplogApplierExecutor.get(),
            _oplogApplierWorkers.back().get()));

        // The contents of the temporary resharding collection are already consistent because the
        // ReshardingCollectionCloner uses atClusterTime. Using replication's initial sync
        // nomenclature, resharding has immediately finished the "apply phase" as soon as the
        // ReshardingCollectionCloner has finished. This is why applyUntilCloneFinishedTs() and
        // applyUntilDone() are both called here in sequence.
        auto* applier = _oplogAppliers.back().get();
        futuresToWaitOn.emplace_back(applier->applyUntilCloneFinishedTs().then(
            [applier] { return applier->applyUntilDone(); }));
        ++i;
    }

    return whenAllSucceed(std::move(futuresToWaitOn)).thenRunOn(**executor).then([this] {
        _transitionStateAndUpdateCoordinator(RecipientStateEnum::kStrictConsistency);
        interrupt({ErrorCodes::InternalError, "Artificial interruption to enable jsTests"});
    });
}

ExecutorFuture<void> ReshardingRecipientService::RecipientStateMachine::
    _awaitCoordinatorHasCommittedThenTransitionToRenaming(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_recipientDoc.getState() > RecipientStateEnum::kStrictConsistency) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _coordinatorHasCommitted.getFuture().thenRunOn(**executor).then([this]() {
        _transitionState(RecipientStateEnum::kRenaming);
    });
}

void ReshardingRecipientService::RecipientStateMachine::
    _renameTemporaryReshardingCollectionThenDeleteLocalState() {

    if (_recipientDoc.getState() > RecipientStateEnum::kRenaming) {
        return;
    }

    auto opCtx = cc().makeOperationContext();

    auto reshardingNss = constructTemporaryReshardingNss(_recipientDoc.getNss().db(),
                                                         _recipientDoc.getExistingUUID());

    RenameCollectionOptions options;
    options.dropTarget = true;
    uassertStatusOK(renameCollection(opCtx.get(), reshardingNss, _recipientDoc.getNss(), options));

    _transitionStateAndUpdateCoordinator(RecipientStateEnum::kDone);
}

void ReshardingRecipientService::RecipientStateMachine::_transitionState(
    RecipientStateEnum endState, boost::optional<Timestamp> fetchTimestamp) {
    ReshardingRecipientDocument replacementDoc(_recipientDoc);
    replacementDoc.setState(endState);
    if (endState == RecipientStateEnum::kCreatingCollection) {
        _insertRecipientDocument(replacementDoc);
        return;
    }

    emplaceFetchTimestampIfExists(replacementDoc, std::move(fetchTimestamp));

    _updateRecipientDocument(std::move(replacementDoc));
}

void ReshardingRecipientService::RecipientStateMachine::_transitionStateAndUpdateCoordinator(
    RecipientStateEnum endState) {
    _transitionState(endState, boost::none);

    auto opCtx = cc().makeOperationContext();

    auto shardId = ShardingState::get(opCtx.get())->shardId();

    BSONObjBuilder updateBuilder;
    updateBuilder.append("recipientShards.$.state", RecipientState_serializer(endState));

    uassertStatusOK(
        Grid::get(opCtx.get())
            ->catalogClient()
            ->updateConfigDocument(
                opCtx.get(),
                NamespaceString::kConfigReshardingOperationsNamespace,
                BSON("_id" << _recipientDoc.get_id() << "recipientShards.id" << shardId),
                BSON("$set" << updateBuilder.done()),
                false /* upsert */,
                ShardingCatalogClient::kMajorityWriteConcern));
}

void ReshardingRecipientService::RecipientStateMachine::_transitionStateToError(
    const Status& status) {
    ReshardingRecipientDocument replacementDoc(_recipientDoc);
    replacementDoc.setState(RecipientStateEnum::kError);
    _updateRecipientDocument(std::move(replacementDoc));
}

void ReshardingRecipientService::RecipientStateMachine::_insertRecipientDocument(
    const ReshardingRecipientDocument& doc) {
    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<ReshardingRecipientDocument> store(
        NamespaceString::kRecipientReshardingOperationsNamespace);
    store.add(opCtx.get(), doc, WriteConcerns::kMajorityWriteConcern);

    _recipientDoc = doc;
}

void ReshardingRecipientService::RecipientStateMachine::_updateRecipientDocument(
    ReshardingRecipientDocument&& replacementDoc) {
    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<ReshardingRecipientDocument> store(
        NamespaceString::kRecipientReshardingOperationsNamespace);
    store.update(opCtx.get(),
                 BSON(ReshardingRecipientDocument::k_idFieldName << _id),
                 replacementDoc.toBSON(),
                 WriteConcerns::kMajorityWriteConcern);

    _recipientDoc = replacementDoc;
}

}  // namespace mongo
