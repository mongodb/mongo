/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/initialize_placement_history_coordinator.h"

#include "mongo/db/global_catalog/ddl/placement_history_cleaner.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/global_catalog/ddl/shardsvr_join_ddl_coordinators_request_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

void joinDDLCoordinators(OperationContext* opCtx) {
    ShardsvrJoinDDLCoordinators joinDDLsRequest;
    joinDDLsRequest.setDbName(DatabaseName::kAdmin);

    const auto allReplicaSets = [&] {
        auto shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
        // A dedicated config server can also coordinate DDL operations; include it if not already
        // present.
        if (auto configShardId = Grid::get(opCtx)->shardRegistry()->getConfigShard()->getId();
            std::find(shardIds.begin(), shardIds.end(), configShardId) == shardIds.end()) {
            shardIds.emplace_back(std::move(configShardId));
        }
        return shardIds;
    }();

    const auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    sharding_util::sendCommandToShards(
        opCtx, DatabaseName::kAdmin, joinDDLsRequest.toBSON(), allReplicaSets, executor);
}

void broadcastHistoryInitializationState(OperationContext* opCtx, bool isInProgress) {
    ConfigsvrSetClusterParameter setClusterParameterReq(
        BSON("placementHistoryInitializationInProgress" << BSON("inProgress" << isInProgress)));
    setClusterParameterReq.setDbName(DatabaseName::kAdmin);
    setClusterParameterReq.set_compatibleWithTopologyChange(true);

    while (true) {
        try {
            DBDirectClient client(opCtx);
            BSONObj res;
            client.runCommand(DatabaseName::kAdmin, setClusterParameterReq.toBSON(), res);
            uassertStatusOK(getStatusFromWriteCommandReply(res));
            break;
        } catch (const ExceptionFor<ErrorCodes::ConflictingOperationInProgress>&) {
            // The error can be caused by a concurrent request targeting an unrelated cluster
            // parameter; retry until succeeding.
            opCtx->sleepFor(Milliseconds(500));
            continue;
        }
    }
}

void blockAndDrainConflictingDDLs(OperationContext* opCtx) {
    // Perform a preliminary draining as a best effort to complete long running DDLs without
    // blocking the execution of regular ones.
    joinDDLCoordinators(opCtx);
    // Declare the initialization of config.placementHistory as "in progress" to disable the
    // instantiation of new incompatible DDL coordinators.
    broadcastHistoryInitializationState(opCtx, true);
    // Drain Incompatible DDL coordinators that are still inflight.
    joinDDLCoordinators(opCtx);
}

void unblockConflictingDDLs(OperationContext* opCtx) {
    broadcastHistoryInitializationState(opCtx, false /*isInProgress*/);
}
}  // namespace

bool InitializePlacementHistoryCoordinator::_mustAlwaysMakeProgress() {
    return _doc.getPhase() > Phase::kUnset;
}

std::set<NamespaceString> InitializePlacementHistoryCoordinator::_getAdditionalLocksToAcquire(
    OperationContext* opCtx) {
    // The execution of this coordinator requires a stable topology on multiple phases.
    // Request the acquisition of an X lock on config.shards to block shard add/remove operations.
    return {NamespaceString::kConfigsvrShardsNamespace};
}

ExecutorFuture<void> InitializePlacementHistoryCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, anchor = shared_from_this()] {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            // Ensure that there is no concurrent access from the periodic cleaning job (which may
            // have been re-activated during the execution of this Coordinator during a node step
            // up).
            PlacementHistoryCleaner::get(opCtx)->pause();
        })
        .then(_buildPhaseHandler(Phase::kBlockDDLs,
                                 [](auto* opCtx) { blockAndDrainConflictingDDLs(opCtx); }))
        .then(_buildPhaseHandler(Phase::kDefineInitializationTime,
                                 [](auto* opCtx) {
                                     // TODO SERVER-109002 Block chunk migration commits and set the
                                     // init time as the current VectorClock::configTime.
                                 }))
        .then(_buildPhaseHandler(Phase::kUnblockDDLs,
                                 [](auto* opCtx) { unblockConflictingDDLs(opCtx); }))
        .then(_buildPhaseHandler(Phase::kComputeInitialization,
                                 [](auto* opCtx) {
                                     // TODO SERVER-109002
                                     // - Apply the chosen init time to generate the placement
                                     // history snapshot;
                                     // - Handle 'SnapshotTooOld' errors.
                                     ShardingCatalogManager::get(opCtx)->initializePlacementHistory(
                                         opCtx);
                                 }))
        .then(_buildPhaseHandler(Phase::kFinalize, [](auto* opCtx) {
            PlacementHistoryCleaner::get(opCtx)->resume(opCtx);
        }));
}

}  // namespace mongo
