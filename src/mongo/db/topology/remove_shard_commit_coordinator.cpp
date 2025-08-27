/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/topology/remove_shard_commit_coordinator.h"

#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/topology/remove_shard_exception.h"
#include "mongo/db/topology/topology_change_helpers.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

ExecutorFuture<void> RemoveShardCommitCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kCheckPreconditions,
            [this, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                // TODO(SERVER-97816): Remove this call once 9.0 becomes lastLTS.
                topology_change_helpers::resetDDLBlockingForTopologyChangeIfNeeded(opCtx);

                _checkShardExistsAndIsDraining(opCtx);
                _setReplicaSetNameOnDocument(opCtx);
            }))
        .then(_buildPhaseHandler(
            Phase::kJoinMigrationsAndCheckRangeDeletions,
            [this, anchor = shared_from_this()](auto* opCtx) {
                return _doc.getIsTransitionToDedicated();
            },
            [this, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                _joinMigrationsAndCheckRangeDeletions(opCtx);
            }))
        .then(_buildPhaseHandler(
            Phase::kStopDDLsAndCleanupData,
            [this, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                _stopDDLOperations(opCtx);
                _checkShardIsEmpty(opCtx);
                if (_doc.getIsTransitionToDedicated()) {
                    _dropLocalCollections(opCtx);
                }
            }))
        .then(_buildPhaseHandler(Phase::kCommit,
                                 [this, executor = executor, anchor = shared_from_this()](
                                     auto* opCtx) { _commitRemoveShard(opCtx, executor); }))
        .then(_buildPhaseHandler(
            Phase::kResumeDDLs,
            [this, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                _updateClusterCardinalityParameterIfNeeded(opCtx);
                _resumeDDLOperations(opCtx);
                _finalizeShardRemoval(opCtx);
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (status == ErrorCodes::RequestAlreadyFulfilled) {
                return Status::OK();
            }

            if (_doc.getPhase() < Phase::kStopDDLsAndCleanupData) {
                return status;
            }

            const auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            if (!_mustAlwaysMakeProgress() && !_isRetriableErrorForDDLCoordinator(status)) {
                triggerCleanup(opCtx, status);
            }

            return status;
        });
}

ExecutorFuture<void> RemoveShardCommitCoordinator::_cleanupOnAbort(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const Status& status) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, token, executor = executor, status, anchor = shared_from_this()] {
            const auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            _resumeDDLOperations(opCtx);
        });
}

void RemoveShardCommitCoordinator::_checkShardExistsAndIsDraining(OperationContext* opCtx) {
    // Since we released the addRemoveShardLock between checking the preconditions and here, it is
    // possible that the shard has already been removed.
    auto optShard = topology_change_helpers::getShardIfExists(
        opCtx, ShardingCatalogManager::get(opCtx)->localConfigShard(), _doc.getShardId());
    if (!optShard.is_initialized()) {
        _finalizeShardRemoval(opCtx);
        uasserted(ErrorCodes::RequestAlreadyFulfilled,
                  str::stream() << "Shard " << _doc.getShardId()
                                << " has already been removed from the cluster");
    }
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Shard " << _doc.getShardId() << " is not currently draining",
            optShard->getDraining());
}

void RemoveShardCommitCoordinator::_setReplicaSetNameOnDocument(OperationContext* opCtx) {
    auto shard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, _doc.getShardId()));
    _doc.setReplicaSetName(shard->getConnString().getReplicaSetName());
}

void RemoveShardCommitCoordinator::_joinMigrationsAndCheckRangeDeletions(OperationContext* opCtx) {
    topology_change_helpers::joinMigrations(opCtx);
    // The config server may be added as a shard again, so we locally drop its drained
    // sharded collections to enable that without user intervention. But we have to wait for
    // the range deleter to quiesce to give queries and stale routers time to discover the
    // migration, to match the usual probabilistic guarantees for migrations.
    auto pendingRangeDeletions = topology_change_helpers::getRangeDeletionCount(opCtx);
    if (pendingRangeDeletions > 0) {
        LOGV2(9782400,
              "removeShard: waiting for range deletions",
              "pendingRangeDeletions"_attr = pendingRangeDeletions);
        RemoveShardProgress progress(ShardDrainingStateEnum::kPendingDataCleanup);
        progress.setPendingRangeDeletions(pendingRangeDeletions);
        uasserted(
            RemoveShardDrainingInfo(progress),
            "Range deletions must complete before transitioning to a dedicated config server.");
    }
}

void RemoveShardCommitCoordinator::_stopDDLOperations(OperationContext* opCtx) {
    // Note that we do not attach session information here because the block of DDL coordinators is
    // done via a cluster parameter and so the only remote write commands are run as part of that
    // coordinator which is responsible for handling the replay protection of those updates.
    topology_change_helpers::blockDDLCoordinatorsAndDrain(opCtx, /*persistRecoveryDocument*/ false);

    globalFailPointRegistry().find("hangRemoveShardAfterDrainingDDL")->pauseWhileSet();
}

void RemoveShardCommitCoordinator::_checkShardIsEmpty(OperationContext* opCtx) {
    auto drainingProgress = topology_change_helpers::getDrainingProgress(
        opCtx,
        ShardingCatalogManager::get(opCtx)->localConfigShard(),
        _doc.getShardId().toString());

    if (!drainingProgress.isFullyDrained()) {
        LOGV2(9782501,
              "removeShard: more draining to do after having blocked DDLCoordinators",
              "chunkCount"_attr = drainingProgress.totalChunks,
              "shardedChunkCount"_attr = drainingProgress.removeShardCounts.getChunks(),
              "unshardedCollectionsCount"_attr =
                  drainingProgress.removeShardCounts.getCollectionsToMove(),
              "databaseCount"_attr = drainingProgress.removeShardCounts.getDbs(),
              "jumboCount"_attr = drainingProgress.removeShardCounts.getJumboChunks());
        RemoveShardProgress progress(ShardDrainingStateEnum::kOngoing);
        progress.setRemaining(drainingProgress.removeShardCounts);
        uasserted(RemoveShardDrainingInfo(progress),
                  "Draining of the shard being removed must complete before removing the shard.");
    }
}

void RemoveShardCommitCoordinator::_dropLocalCollections(OperationContext* opCtx) {
    auto trackedDbs = ShardingCatalogManager::get(opCtx)->localCatalogClient()->getAllDBs(
        opCtx, repl::ReadConcernLevel::kLocalReadConcern);

    if (auto pendingCleanupState = topology_change_helpers::dropLocalCollectionsAndDatabases(
            opCtx, trackedDbs, _doc.getShardId().toString())) {
        uasserted(RemoveShardDrainingInfo(*pendingCleanupState),
                  "All collections must be empty before removing the shard.");
    }

    DBDirectClient client(opCtx);
    BSONObj result;
    if (!client.dropCollection(NamespaceString::kLogicalSessionsNamespace,
                               ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
                               &result)) {
        uassertStatusOK(getStatusFromCommandResult(result));
    }
}

void RemoveShardCommitCoordinator::_commitRemoveShard(
    OperationContext* opCtx, std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    LOGV2(9782601, "Going to remove shard", "shardId"_attr = _doc.getShardId().toString());

    Lock::ExclusiveLock shardMembershipLock =
        ShardingCatalogManager::get(opCtx)->acquireShardMembershipLockForTopologyChange(opCtx);

    topology_change_helpers::commitRemoveShard(
        shardMembershipLock,
        opCtx,
        ShardingCatalogManager::get(opCtx)->localConfigShard(),
        _doc.getShardId().toString(),
        **executor);

    // The shard which was just removed must be reflected in the shard registry, before the
    // replica set monitor is removed, otherwise the shard would be referencing a dropped RSM.
    Grid::get(opCtx)->shardRegistry()->reload(opCtx);

    if (!_doc.getIsTransitionToDedicated()) {
        // Don't remove the config shard's RSM because it is used to target the config server.
        ReplicaSetMonitor::remove(std::string{*_doc.getReplicaSetName()});
    }
}

void RemoveShardCommitCoordinator::_resumeDDLOperations(OperationContext* opCtx) {
    // Note that we do not attach session information here because the block of DDL coordinators is
    // done via a cluster parameter and so the only remote write commands are run as part of that
    // coordinator which is responsible for handling the replay protection of those updates.
    topology_change_helpers::unblockDDLCoordinators(opCtx, /*removeRecoveryDocument*/ false);
}

void RemoveShardCommitCoordinator::_updateClusterCardinalityParameterIfNeeded(
    OperationContext* opCtx) {
    globalFailPointRegistry()
        .find("hangRemoveShardBeforeUpdatingClusterCardinalityParameter")
        ->pauseWhileSet();

    // Only update the parameter if the coordinator was started with
    // `shouldUpdateClusterCardinality` set to true.
    if (!_doc.getShouldUpdateClusterCardinality())
        return;

    // Call the helper which acquires the clusterCardinalityParameterLock so we don't need to expose
    // that mutex. This may issue an additional setClusterParameter command when the number of
    // shards is 2, but as this should be a noop (and we don't expect this path to be taken anyways)
    // this is ok.
    uassertStatusOK(
        ShardingCatalogManager::get(opCtx)->updateClusterCardinalityParameterIfNeeded(opCtx));
}

void RemoveShardCommitCoordinator::_finalizeShardRemoval(OperationContext* opCtx) {
    _result = RemoveShardProgress(ShardDrainingStateEnum::kCompleted);
    // Record finish in changelog
    auto catalogManager = ShardingCatalogManager::get(opCtx);
    ShardingLogging::get(opCtx)->logChange(
        opCtx,
        "removeShard",
        NamespaceString::kEmpty,
        BSON("shard" << _doc.getShardId().toString()),
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
        catalogManager->localConfigShard(),
        catalogManager->localCatalogClient());
}

void RemoveShardCommitCoordinator::checkIfOptionsConflict(const BSONObj& stateDoc) const {
    // Only one remove shard can run at any time, so all the user supplied parameters must match.
    const auto otherDoc = RemoveShardCommitCoordinatorDocument::parse(
        stateDoc, IDLParserContext("RemoveShardCommitCoordinatorDocument"));

    const auto optionsMatch = [&] {
        stdx::lock_guard lk(_docMutex);
        return _doc.getShardId() == otherDoc.getShardId();
    }();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another removeShard with different arguments is already running with "
                             "different options",
            optionsMatch);
}

RemoveShardProgress RemoveShardCommitCoordinator::getResult(OperationContext* opCtx) {
    getCompletionFuture().get(opCtx);
    tassert(10644502, "Expected _result to be initialized", _result.is_initialized());
    return *_result;
}

}  // namespace mongo
