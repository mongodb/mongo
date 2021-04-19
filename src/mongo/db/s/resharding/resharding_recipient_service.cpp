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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

#include "mongo/db/s/resharding/resharding_recipient_service.h"

#include <algorithm>

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_oplog_applier.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/s/shard_key_util.h"
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

MONGO_FAIL_POINT_DEFINE(removeRecipientDocFailpoint);
MONGO_FAIL_POINT_DEFINE(reshardingPauseRecipientBeforeCloning);
MONGO_FAIL_POINT_DEFINE(reshardingPauseRecipientDuringCloning);
MONGO_FAIL_POINT_DEFINE(reshardingPauseRecipientDuringOplogApplication);

namespace {

const WriteConcernOptions kNoWaitWriteConcern{1, WriteConcernOptions::SyncMode::UNSET, Seconds(0)};

/**
 * Fulfills the promise if it is not already. Otherwise, does nothing.
 */
void ensureFulfilledPromise(WithLock lk, SharedPromise<void>& sp) {
    if (!sp.getFuture().isReady()) {
        sp.emplaceValue();
    }
}

void ensureFulfilledPromise(
    WithLock lk,
    SharedPromise<ReshardingRecipientService::RecipientStateMachine::CloneDetails>& sp,
    ReshardingRecipientService::RecipientStateMachine::CloneDetails details) {
    auto future = sp.getFuture();
    if (!future.isReady()) {
        sp.emplaceValue(details);
    } else {
        // Ensure that we would only attempt to fulfill the promise with the same Timestamp value.
        invariant(future.get().cloneTimestamp == details.cloneTimestamp);
    }
}

}  // namespace

namespace resharding {

void createTemporaryReshardingCollectionLocally(OperationContext* opCtx,
                                                const NamespaceString& originalNss,
                                                const NamespaceString& reshardingNss,
                                                const UUID& reshardingUUID,
                                                const UUID& existingUUID,
                                                Timestamp fetchTimestamp) {
    LOGV2_DEBUG(
        5002300, 1, "Creating temporary resharding collection", "originalNss"_attr = originalNss);

    auto catalogCache = Grid::get(opCtx)->catalogCache();

    // Load the original collection's options from the database's primary shard.
    auto [collOptions, uuid] = shardVersionRetry(
        opCtx,
        catalogCache,
        reshardingNss,
        "loading collection options to create temporary resharding collection"_sd,
        [&]() -> MigrationDestinationManager::CollectionOptionsAndUUID {
            auto originalCm = uassertStatusOK(
                catalogCache->getShardedCollectionRoutingInfoWithRefresh(opCtx, originalNss));
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
                              auto originalCm =
                                  catalogCache->getShardedCollectionRoutingInfo(opCtx, originalNss);
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
    BSONObj initialState) {
    return std::make_shared<RecipientStateMachine>(
        this, std::move(initialState), ReshardingDataReplication::make);
}

ReshardingRecipientService::RecipientStateMachine::RecipientStateMachine(
    const ReshardingRecipientService* recipientService,
    const BSONObj& recipientDoc,
    ReshardingDataReplicationFactory dataReplicationFactory)
    : RecipientStateMachine(
          recipientService,
          ReshardingRecipientDocument::parse({"RecipientStateMachine"}, recipientDoc),
          std::move(dataReplicationFactory)) {}

ReshardingRecipientService::RecipientStateMachine::RecipientStateMachine(
    const ReshardingRecipientService* recipientService,
    const ReshardingRecipientDocument& recipientDoc,
    ReshardingDataReplicationFactory dataReplicationFactory)
    : repl::PrimaryOnlyService::TypedInstance<RecipientStateMachine>(),
      _recipientService{recipientService},
      _metadata{recipientDoc.getCommonReshardingMetadata()},
      _minimumOperationDuration{Milliseconds{recipientDoc.getMinimumOperationDurationMillis()}},
      _recipientCtx{recipientDoc.getMutableState()},
      _donorShards{recipientDoc.getDonorShards()},
      _cloneTimestamp{recipientDoc.getCloneTimestamp()},
      _markKilledExecutor(std::make_shared<ThreadPool>([] {
          ThreadPool::Options options;
          options.poolName = "RecipientStateMachineCancelableOpCtxPool";
          options.minThreads = 1;
          options.maxThreads = 1;
          return options;
      }())),
      _dataReplicationFactory{std::move(dataReplicationFactory)} {}

ReshardingRecipientService::RecipientStateMachine::~RecipientStateMachine() {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_allDonorsPreparedToDonate.getFuture().isReady());
    invariant(_coordinatorHasDecisionPersisted.getFuture().isReady());
    invariant(_completionPromise.getFuture().isReady());
}

SemiFuture<void> ReshardingRecipientService::RecipientStateMachine::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& stepdownToken) noexcept {
    auto abortToken = _initAbortSource(stepdownToken);
    _markKilledExecutor->startup();
    _cancelableOpCtxFactory.emplace(abortToken, _markKilledExecutor);

    return ExecutorFuture<void>(**executor)
        .then([this, executor] {
            _metrics()->onStart();
            return _awaitAllDonorsPreparedToDonateThenTransitionToCreatingCollection(executor);
        })
        .then([this] { _createTemporaryReshardingCollectionThenTransitionToCloning(); })
        .then([this, executor, abortToken] {
            return _cloneThenTransitionToApplying(executor, abortToken);
        })
        .then([this, executor, abortToken] {
            return _applyThenTransitionToSteadyState(executor, abortToken);
        })
        .then([this, executor, abortToken] {
            return _awaitAllDonorsBlockingWritesThenTransitionToStrictConsistency(executor,
                                                                                  abortToken);
        })
        .then([this, executor] {
            return _awaitCoordinatorHasDecisionPersistedThenTransitionToRenaming(executor);
        })
        .then([this] { _renameTemporaryReshardingCollection(); })
        .then([this, executor] {
            auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
            return _updateCoordinator(opCtx.get(), executor);
        })
        .onCompletion([this, stepdownToken](auto passthroughFuture) {
            _cancelableOpCtxFactory.emplace(stepdownToken, _markKilledExecutor);
            return passthroughFuture;
        })
        .onError([this, executor](Status status) {
            Status error = status;
            {
                stdx::lock_guard<Latch> lk(_mutex);
                if (_abortStatus)
                    error = *_abortStatus;
            }

            LOGV2(4956500,
                  "Resharding operation recipient state machine failed",
                  "namespace"_attr = _metadata.getSourceNss(),
                  "reshardingUUID"_attr = _metadata.getReshardingUUID(),
                  "error"_attr = error);

            _transitionToError(error);
            auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
            return _updateCoordinator(opCtx.get(), executor)
                .then([this, executor] {
                    // Wait for all of the data replication components to halt. We ignore any errors
                    // because resharding is known to have failed already.
                    return _dataReplicationQuiesced.thenRunOn(**executor)
                        .onError([](Status status) { return Status::OK(); });
                })
                .then([this] {
                    // TODO SERVER-52838: Ensure all local collections that may have been created
                    // for resharding are removed, with the exception of the
                    // ReshardingRecipientDocument, before transitioning to kDone.
                    _transitionState(RecipientStateEnum::kDone);
                })
                .then([this, executor] {
                    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                    return _updateCoordinator(opCtx.get(), executor);
                })
                .then([this, error] { return error; });
        })
        .onCompletion([this, self = shared_from_this()](Status status) {
            {
                stdx::lock_guard<Latch> lg(_mutex);
                if (_completionPromise.getFuture().isReady()) {
                    // interrupt() was called before we got here.
                    _metrics()->onCompletion(ReshardingOperationStatusEnum::kCanceled);
                    return;
                }
            }

            if (status.isOK()) {
                // The shared_ptr stored in the PrimaryOnlyService's map for the
                // ReshardingRecipientService Instance is removed when the recipient state document
                // tied to the instance is deleted. It is necessary to use shared_from_this() to
                // extend the lifetime so the code can safely finish executing.

                {
                    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                    removeRecipientDocFailpoint.pauseWhileSet(opCtx.get());
                }

                _removeRecipientDocument();
                _metrics()->onCompletion(ReshardingOperationStatusEnum::kSuccess);
                stdx::lock_guard<Latch> lg(_mutex);
                if (!_completionPromise.getFuture().isReady()) {
                    _completionPromise.emplaceValue();
                }
            } else {
                _metrics()->onCompletion(ErrorCodes::isCancellationError(status)
                                             ? ReshardingOperationStatusEnum::kCanceled
                                             : ReshardingOperationStatusEnum::kFailure);
                stdx::lock_guard<Latch> lg(_mutex);
                if (!_completionPromise.getFuture().isReady()) {
                    _completionPromise.setError(status);
                }
            }
        })
        .semi();
}

void ReshardingRecipientService::RecipientStateMachine::interrupt(Status status) {
    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<Latch> lk(_mutex);
    _abortStatus.emplace(status);
    _onAbortOrStepdown(lk, status);

    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

boost::optional<BSONObj> ReshardingRecipientService::RecipientStateMachine::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode,
    MongoProcessInterface::CurrentOpSessionsMode) noexcept {
    ReshardingMetrics::ReporterOptions options(ReshardingMetrics::ReporterOptions::Role::kRecipient,
                                               _metadata.getReshardingUUID(),
                                               _metadata.getSourceNss(),
                                               _metadata.getReshardingKey().toBSON(),
                                               false);
    return _metrics()->reportForCurrentOp(options);
}

void ReshardingRecipientService::RecipientStateMachine::onReshardingFieldsChanges(
    OperationContext* opCtx, const TypeCollectionReshardingFields& reshardingFields) {
    stdx::lock_guard<Latch> lk(_mutex);
    if (reshardingFields.getAbortReason()) {
        auto status = getStatusFromAbortReason(reshardingFields);
        invariant(!status.isOK());
        _abortStatus.emplace(status);

        if (_abortSource) {
            _abortSource->cancel();
        }

        _onAbortOrStepdown(lk, status);
        _critSec.reset();
        return;
    }

    auto coordinatorState = reshardingFields.getState();

    if (coordinatorState >= CoordinatorStateEnum::kCloning) {
        auto recipientFields = *reshardingFields.getRecipientFields();
        invariant(recipientFields.getCloneTimestamp());
        ensureFulfilledPromise(
            lk,
            _allDonorsPreparedToDonate,
            {*recipientFields.getCloneTimestamp(), recipientFields.getDonorShards()});
    }

    if (coordinatorState >= CoordinatorStateEnum::kDecisionPersisted) {
        ensureFulfilledPromise(lk, _coordinatorHasDecisionPersisted);
    }
}

ExecutorFuture<void> ReshardingRecipientService::RecipientStateMachine::
    _awaitAllDonorsPreparedToDonateThenTransitionToCreatingCollection(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_recipientCtx.getState() > RecipientStateEnum::kAwaitingFetchTimestamp) {
        invariant(_cloneTimestamp);
        return ExecutorFuture(**executor);
    }

    return _allDonorsPreparedToDonate.getFuture()
        .thenRunOn(**executor)
        .then([this](ReshardingRecipientService::RecipientStateMachine::CloneDetails cloneDetails) {
            _transitionToCreatingCollection(std::move(cloneDetails));
        });
}

void ReshardingRecipientService::RecipientStateMachine::
    _createTemporaryReshardingCollectionThenTransitionToCloning() {
    if (_recipientCtx.getState() > RecipientStateEnum::kCreatingCollection) {
        return;
    }

    {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

        resharding::createTemporaryReshardingCollectionLocally(opCtx.get(),
                                                               _metadata.getSourceNss(),
                                                               _metadata.getTempReshardingNss(),
                                                               _metadata.getReshardingUUID(),
                                                               _metadata.getSourceUUID(),
                                                               *_cloneTimestamp);

        ShardKeyPattern shardKeyPattern{_metadata.getReshardingKey()};

        auto catalogCache = Grid::get(opCtx.get())->catalogCache();
        shardVersionRetry(opCtx.get(),
                          catalogCache,
                          _metadata.getTempReshardingNss(),
                          "validating shard key index for reshardCollection"_sd,
                          [&] {
                              shardkeyutil::validateShardKeyIndexExistsOrCreateIfPossible(
                                  opCtx.get(),
                                  _metadata.getTempReshardingNss(),
                                  shardKeyPattern,
                                  CollationSpec::kSimpleSpec,
                                  false,
                                  shardkeyutil::ValidationBehaviorsShardCollection(opCtx.get()));
                          });
    }

    _transitionState(RecipientStateEnum::kCloning);
}

std::unique_ptr<ReshardingDataReplicationInterface>
ReshardingRecipientService::RecipientStateMachine::_makeDataReplication(OperationContext* opCtx,
                                                                        bool cloningDone) {
    invariant(_cloneTimestamp);

    auto myShardId = ShardingState::get(opCtx->getServiceContext())->shardId();
    auto catalogCache = Grid::get(opCtx)->catalogCache();
    auto sourceChunkMgr =
        catalogCache->getShardedCollectionRoutingInfo(opCtx, _metadata.getSourceNss());

    return _dataReplicationFactory(opCtx,
                                   _metrics(),
                                   _metadata,
                                   _donorShards,
                                   *_cloneTimestamp,
                                   cloningDone,
                                   std::move(myShardId),
                                   std::move(sourceChunkMgr));
}

void ReshardingRecipientService::RecipientStateMachine::_ensureDataReplicationStarted(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& abortToken) {
    const bool cloningDone = _recipientCtx.getState() > RecipientStateEnum::kCloning;

    if (!_dataReplication) {
        auto dataReplication = _makeDataReplication(opCtx, cloningDone);
        _dataReplicationQuiesced =
            dataReplication
                ->runUntilStrictlyConsistent(**executor,
                                             _recipientService->getInstanceCleanupExecutor(),
                                             abortToken,
                                             *_cancelableOpCtxFactory,
                                             _minimumOperationDuration)
                .share();

        stdx::lock_guard lk(_mutex);
        _dataReplication = std::move(dataReplication);
    }

    if (cloningDone) {
        _dataReplication->startOplogApplication();
    }
}

ExecutorFuture<void>
ReshardingRecipientService::RecipientStateMachine::_cloneThenTransitionToApplying(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& abortToken) {
    if (_recipientCtx.getState() > RecipientStateEnum::kCloning) {
        return ExecutorFuture(**executor);
    }

    {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
        reshardingPauseRecipientBeforeCloning.pauseWhileSet(opCtx.get());
    }

    {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
        _ensureDataReplicationStarted(opCtx.get(), executor, abortToken);
    }

    {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
        reshardingPauseRecipientDuringCloning.pauseWhileSet(opCtx.get());
    }

    return future_util::withCancellation(_dataReplication->awaitCloningDone(), abortToken)
        .thenRunOn(**executor)
        .then([this] { _transitionState(RecipientStateEnum::kApplying); });
}

ExecutorFuture<void>
ReshardingRecipientService::RecipientStateMachine::_applyThenTransitionToSteadyState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& abortToken) {
    if (_recipientCtx.getState() > RecipientStateEnum::kApplying) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    _ensureDataReplicationStarted(opCtx.get(), executor, abortToken);

    return _updateCoordinator(opCtx.get(), executor)
        // ReshardingOplogApplier::applyUntilCloneFinishedTs() won't return a ready future until it
        // has applied at least one batch. We skip waiting on awaitConsistentButStale() to avoid a
        // situation where, in the absence of write activity on the collection being resharding, the
        // recipient remains stuck in kApplying. This is because the coordinator will only have
        // donor shards begin blocking writes (and thus write their final resharding oplog entries)
        // once all recipients have reached kSteadyState. Waiting on awaitConsistentButStale() isn't
        // necessary for resharding's correctness. It is always safe for the coordinator to block
        // writes sooner. It isn't even inefficient due to how recipients clone the collection being
        // resharded using {atClusterTime: *_fetchTimestamp}.
        //
        // TODO SERVER-49897: See if the following lines can be uncommented once a no-op oplog entry
        // would be inserted into the local oplog buffer after the recipient reaches the end of the
        // donor's oplog.
        //
        // .then([this, abortToken] {
        //     return future_util::withCancellation(_dataReplication->awaitConsistentButStale(),
        //                                          abortToken);
        // })
        .then([this] { _transitionState(RecipientStateEnum::kSteadyState); });
}

ExecutorFuture<void> ReshardingRecipientService::RecipientStateMachine::
    _awaitAllDonorsBlockingWritesThenTransitionToStrictConsistency(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken) {
    if (_recipientCtx.getState() > RecipientStateEnum::kSteadyState) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
        _ensureDataReplicationStarted(opCtx.get(), executor, abortToken);
    }

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    return _updateCoordinator(opCtx.get(), executor)
        .then([this, abortToken] {
            {
                auto opCtx = cc().makeOperationContext();
                reshardingPauseRecipientDuringOplogApplication.pauseWhileSet(opCtx.get());
            }

            return future_util::withCancellation(_dataReplication->awaitStrictlyConsistent(),
                                                 abortToken);
        })
        .then([this] {
            auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
            for (const auto& donor : _donorShards) {
                auto stashNss =
                    getLocalConflictStashNamespace(_metadata.getSourceUUID(), donor.getShardId());
                AutoGetCollection stashColl(opCtx.get(), stashNss, MODE_IS);
                uassert(5356800,
                        "Resharding completed with non-empty stash collections",
                        !stashColl || stashColl->isEmpty(opCtx.get()));
            }
        })
        .then([this] {
            _transitionState(RecipientStateEnum::kStrictConsistency);

            const bool isAlsoDonor = [&] {
                auto myShardId = ShardingState::get(cc().getServiceContext())->shardId();
                return std::find_if(_donorShards.begin(),
                                    _donorShards.end(),
                                    [&](const DonorShardFetchTimestamp& donor) {
                                        return donor.getShardId() == myShardId;
                                    }) != _donorShards.end();
            }();

            if (!isAlsoDonor) {
                _critSec.emplace(cc().getServiceContext(), _metadata.getSourceNss());
            }
        });
}

ExecutorFuture<void> ReshardingRecipientService::RecipientStateMachine::
    _awaitCoordinatorHasDecisionPersistedThenTransitionToRenaming(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_recipientCtx.getState() > RecipientStateEnum::kStrictConsistency) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    return _updateCoordinator(opCtx.get(), executor)
        .then([this] { return _coordinatorHasDecisionPersisted.getFuture(); })
        .thenRunOn(**executor)
        .then([this]() { _transitionState(RecipientStateEnum::kRenaming); });
}

void ReshardingRecipientService::RecipientStateMachine::_renameTemporaryReshardingCollection() {
    if (_recipientCtx.getState() > RecipientStateEnum::kRenaming) {
        return;
    }

    const bool isAlsoDonor = [&] {
        auto myShardId = ShardingState::get(cc().getServiceContext())->shardId();
        return std::find_if(_donorShards.begin(),
                            _donorShards.end(),
                            [&](const DonorShardFetchTimestamp& donor) {
                                return donor.getShardId() == myShardId;
                            }) != _donorShards.end();
    }();

    if (!isAlsoDonor) {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
        RenameCollectionOptions options;
        options.dropTarget = true;
        uassertStatusOK(renameCollection(
            opCtx.get(), _metadata.getTempReshardingNss(), _metadata.getSourceNss(), options));

        _critSec.reset();
    }

    {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
        resharding::data_copy::ensureOplogCollectionsDropped(
            opCtx.get(), _metadata.getReshardingUUID(), _metadata.getSourceUUID(), _donorShards);
    }

    _transitionState(RecipientStateEnum::kDone);
}

void ReshardingRecipientService::RecipientStateMachine::_transitionState(
    RecipientStateEnum newState) {
    invariant(newState != RecipientStateEnum::kCreatingCollection &&
              newState != RecipientStateEnum::kError);

    auto newRecipientCtx = _recipientCtx;
    newRecipientCtx.setState(newState);
    _transitionState(std::move(newRecipientCtx), boost::none);
}

void ReshardingRecipientService::RecipientStateMachine::_transitionState(
    RecipientShardContext&& newRecipientCtx,
    boost::optional<ReshardingRecipientService::RecipientStateMachine::CloneDetails>&&
        cloneDetails) {
    invariant(newRecipientCtx.getState() != RecipientStateEnum::kAwaitingFetchTimestamp);

    // For logging purposes.
    auto oldState = _recipientCtx.getState();
    auto newState = newRecipientCtx.getState();

    _updateRecipientDocument(std::move(newRecipientCtx), std::move(cloneDetails));

    _metrics()->setRecipientState(newState);

    LOGV2_INFO(5279506,
               "Transitioned resharding recipient state",
               "newState"_attr = RecipientState_serializer(newState),
               "oldState"_attr = RecipientState_serializer(oldState),
               "namespace"_attr = _metadata.getSourceNss(),
               "collectionUUID"_attr = _metadata.getSourceUUID(),
               "reshardingUUID"_attr = _metadata.getReshardingUUID());
}

void ReshardingRecipientService::RecipientStateMachine::_transitionToCreatingCollection(
    ReshardingRecipientService::RecipientStateMachine::CloneDetails cloneDetails) {
    auto newRecipientCtx = _recipientCtx;
    newRecipientCtx.setState(RecipientStateEnum::kCreatingCollection);
    _transitionState(std::move(newRecipientCtx), std::move(cloneDetails));
}

void ReshardingRecipientService::RecipientStateMachine::_transitionToError(Status abortReason) {
    auto newRecipientCtx = _recipientCtx;
    newRecipientCtx.setState(RecipientStateEnum::kError);
    emplaceAbortReasonIfExists(newRecipientCtx, abortReason);
    _transitionState(std::move(newRecipientCtx), boost::none);
}

/**
 * Returns a query filter of the form
 * {
 *     _id: <reshardingUUID>,
 *     recipientShards: {$elemMatch: {
 *         id: <this recipient's ShardId>,
 *         "mutableState.state: {$in: [ <list of valid current states> ]},
 *     }},
 * }
 */
BSONObj ReshardingRecipientService::RecipientStateMachine::_makeQueryForCoordinatorUpdate(
    const ShardId& shardId, RecipientStateEnum newState) {
    // The recipient only updates the coordinator when it transitions to states which the
    // coordinator depends on for its own transitions. The table maps the recipient states which
    // could be updated on the coordinator to the only states the recipient could have already
    // persisted to the current coordinator document in order for its transition to the newState to
    // be valid.
    static const stdx::unordered_map<RecipientStateEnum, std::vector<RecipientStateEnum>>
        validPreviousStateMap = {
            {RecipientStateEnum::kApplying, {RecipientStateEnum::kUnused}},
            {RecipientStateEnum::kSteadyState, {RecipientStateEnum::kApplying}},
            {RecipientStateEnum::kStrictConsistency, {RecipientStateEnum::kSteadyState}},
            {RecipientStateEnum::kError,
             {RecipientStateEnum::kUnused,
              RecipientStateEnum::kApplying,
              RecipientStateEnum::kSteadyState}},
            {RecipientStateEnum::kDone,
             {RecipientStateEnum::kUnused,
              RecipientStateEnum::kApplying,
              RecipientStateEnum::kSteadyState,
              RecipientStateEnum::kStrictConsistency,
              RecipientStateEnum::kError}},
        };

    auto it = validPreviousStateMap.find(newState);
    invariant(it != validPreviousStateMap.end());

    // The network isn't perfectly reliable so it is possible for update commands sent by
    // _updateCoordinator() to be received out of order by the coordinator. To overcome this
    // behavior, the recipient shard includes the list of valid current states as part of
    // the update to transition to the next state. This way, the update from a delayed
    // message won't match the document if it or any later state transitions have already
    // occurred.
    BSONObjBuilder queryBuilder;
    {
        _metadata.getReshardingUUID().appendToBuilder(
            &queryBuilder, ReshardingCoordinatorDocument::kReshardingUUIDFieldName);

        BSONObjBuilder recipientShardsBuilder(
            queryBuilder.subobjStart(ReshardingCoordinatorDocument::kRecipientShardsFieldName));
        {
            BSONObjBuilder elemMatchBuilder(recipientShardsBuilder.subobjStart("$elemMatch"));
            {
                elemMatchBuilder.append(RecipientShardEntry::kIdFieldName, shardId);

                BSONObjBuilder mutableStateBuilder(
                    elemMatchBuilder.subobjStart(RecipientShardEntry::kMutableStateFieldName + "." +
                                                 RecipientShardContext::kStateFieldName));
                {
                    BSONArrayBuilder inBuilder(mutableStateBuilder.subarrayStart("$in"));
                    for (const auto& state : it->second) {
                        inBuilder.append(RecipientState_serializer(state));
                    }
                }
            }
        }
    }

    return queryBuilder.obj();
}

ExecutorFuture<void> ReshardingRecipientService::RecipientStateMachine::_updateCoordinator(
    OperationContext* opCtx, const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
    auto clientOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    return WaitForMajorityService::get(opCtx->getServiceContext())
        .waitUntilMajority(clientOpTime, CancellationToken::uncancelable())
        .thenRunOn(**executor)
        .then([this] {
            auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

            auto shardId = ShardingState::get(opCtx.get())->shardId();

            BSONObjBuilder updateBuilder;
            {
                BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));
                {
                    setBuilder.append(ReshardingCoordinatorDocument::kRecipientShardsFieldName +
                                          ".$." + RecipientShardEntry::kMutableStateFieldName,
                                      _recipientCtx.toBSON());
                }
            }

            uassertStatusOK(
                Grid::get(opCtx.get())
                    ->catalogClient()
                    ->updateConfigDocument(
                        opCtx.get(),
                        NamespaceString::kConfigReshardingOperationsNamespace,
                        _makeQueryForCoordinatorUpdate(shardId, _recipientCtx.getState()),
                        updateBuilder.done(),
                        false /* upsert */,
                        ShardingCatalogClient::kMajorityWriteConcern));
        });
}

void ReshardingRecipientService::RecipientStateMachine::insertStateDocument(
    OperationContext* opCtx, const ReshardingRecipientDocument& recipientDoc) {
    PersistentTaskStore<ReshardingRecipientDocument> store(
        NamespaceString::kRecipientReshardingOperationsNamespace);
    store.add(opCtx, recipientDoc, kNoWaitWriteConcern);
}

void ReshardingRecipientService::RecipientStateMachine::_updateRecipientDocument(
    RecipientShardContext&& newRecipientCtx,
    boost::optional<ReshardingRecipientService::RecipientStateMachine::CloneDetails>&&
        cloneDetails) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    PersistentTaskStore<ReshardingRecipientDocument> store(
        NamespaceString::kRecipientReshardingOperationsNamespace);

    BSONObjBuilder updateBuilder;
    {
        BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));
        setBuilder.append(ReshardingRecipientDocument::kMutableStateFieldName,
                          newRecipientCtx.toBSON());

        if (cloneDetails) {
            setBuilder.append(ReshardingRecipientDocument::kCloneTimestampFieldName,
                              cloneDetails->cloneTimestamp);

            BSONArrayBuilder donorShardsArrayBuilder;
            for (const auto& donor : cloneDetails->donorShards) {
                donorShardsArrayBuilder.append(donor.toBSON());
            }

            setBuilder.append(ReshardingRecipientDocument::kDonorShardsFieldName,
                              donorShardsArrayBuilder.arr());
        }

        setBuilder.doneFast();
    }

    store.update(opCtx.get(),
                 BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName
                      << _metadata.getReshardingUUID()),
                 updateBuilder.done(),
                 kNoWaitWriteConcern);

    _recipientCtx = newRecipientCtx;

    if (cloneDetails) {
        _cloneTimestamp = cloneDetails->cloneTimestamp;
        _donorShards = std::move(cloneDetails->donorShards);
    }
}

void ReshardingRecipientService::RecipientStateMachine::_removeRecipientDocument() {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    PersistentTaskStore<ReshardingRecipientDocument> store(
        NamespaceString::kRecipientReshardingOperationsNamespace);
    store.remove(opCtx.get(),
                 BSON(ReshardingRecipientDocument::kReshardingUUIDFieldName
                      << _metadata.getReshardingUUID()),
                 kNoWaitWriteConcern);
}

ReshardingMetrics* ReshardingRecipientService::RecipientStateMachine::_metrics() const {
    return ReshardingMetrics::get(cc().getServiceContext());
}

void ReshardingRecipientService::RecipientStateMachine::_onAbortOrStepdown(WithLock,
                                                                           Status status) {
    if (_dataReplication) {
        _dataReplication->shutdown();
    }

    if (!_allDonorsPreparedToDonate.getFuture().isReady()) {
        _allDonorsPreparedToDonate.setError(status);
    }

    if (!_coordinatorHasDecisionPersisted.getFuture().isReady()) {
        _coordinatorHasDecisionPersisted.setError(status);
    }
}

CancellationToken ReshardingRecipientService::RecipientStateMachine::_initAbortSource(
    const CancellationToken& stepdownToken) {
    stdx::lock_guard<Latch> lk(_mutex);
    _abortSource = CancellationSource(stepdownToken);
    return _abortSource->token();
}

}  // namespace mongo
