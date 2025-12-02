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

#include "mongo/db/global_catalog/ddl/notify_sharding_event_gen.h"
#include "mongo/db/global_catalog/ddl/placement_history_cleaner.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/global_catalog/ddl/shardsvr_join_ddl_coordinators_request_gen.h"
#include "mongo/db/shard_role/shard_catalog/drop_collection.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"

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

void broadcastPlacementHistoryChangedNotification(
    OperationContext* opCtx,
    const Timestamp& committedAt,
    const OperationSessionInfo& osi,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    auto allShards = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

    ShardsvrNotifyShardingEventRequest request(
        notify_sharding_event::kPlacementHistoryMetadataChanged,
        PlacementHistoryMetadataChanged(committedAt).toBSON());

    request.setDbName(DatabaseName::kAdmin);
    generic_argument_util::setMajorityWriteConcern(request);
    generic_argument_util::setOperationSessionInfo(request, osi);

    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrNotifyShardingEventRequest>>(
        **executor, token, std::move(request));

    auto responses = sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx, opts, std::move(allShards), false /* throwOnError */);
    for (const auto& response : responses) {
        auto status = AsyncRequestsSender::Response::getEffectiveStatus(response);
        if (status.isOK()) {
            continue;
        }

        if (status == ErrorCodes::UnsupportedShardingEventNotification) {
            // Swallow the error, which is expected when the recipient runs a legacy binary that
            // does not support the kPlacementHistoryMetadataChanged notification type.
            LOGV2_WARNING(10916800,
                          "Skipping kPlacementHistoryMetadataChanged notification",
                          "recipient"_attr = response.shardId);
        } else {
            uassertStatusOK(status);
        }
    }
}

bool anyShardInTheCluster(OperationContext* opCtx) {
    auto* shardRegistry = Grid::get(opCtx)->shardRegistry();
    return shardRegistry->getNumShards(opCtx) != 0;
}

}  // namespace

bool InitializePlacementHistoryCoordinator::_mustAlwaysMakeProgress() {
    return _doc.getPhase() > Phase::kCheckPreconditions;
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
        .then(_buildPhaseHandler(Phase::kCheckPreconditions,
                                 [](auto* opCtx) {
                                     // Disregard this request if the cluster is 'empty':
                                     // config.placementHistory has no meaning within such a state
                                     // and the first shard addition is expected to later trigger
                                     // the initialization of its content.
                                     uassert(
                                         ErrorCodes::RequestAlreadyFulfilled,
                                         "Skipping initialization of config.placementHistory: "
                                         "there is currently no shard registered in the cluster",
                                         anyShardInTheCluster(opCtx));
                                 }))
        .then([this, anchor = shared_from_this()] {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            ShardingLogging::get(opCtx)->logChange(opCtx, "resetPlacementHistory.start", nss());
            // Ensure that there is no concurrent access from the periodic cleaning job (which may
            // have been re-activated during the execution of this Coordinator during a node step
            // up).
            PlacementHistoryCleaner::get(opCtx)->pause();
        })
        .then(_buildPhaseHandler(Phase::kBlockDDLs,
                                 [](auto* opCtx) { blockAndDrainConflictingDDLs(opCtx); }))
        .then(_buildPhaseHandler(
            Phase::kDefineInitializationTime,
            [this, anchor = shared_from_this()](auto* opCtx) {
                // Block chunk migration commits: this, combined with the preemption of DDLs
                // (applied in Phase::kBlockDDLs) and topology changes (maintained throughout the
                // execution of this coordinator, see _getAdditionalLocksToAcquire()), allows to
                // operate in isolation from any operation that may write placement changes on the
                // global catalog. Use this section to:
                // - Delete the existing content of config.placementHistory, which may be
                // incomplete/inconsistent; a collection drop is performed to reduce execution time.
                // - Establish the point-in-time for the snapshot read that will support its
                //   recreation during the kInitialization phase.
                auto noChunkMigrationCommitsRegion =
                    ShardingCatalogManager::get(opCtx)->acquireChunkOpLockForSnapshotReadOnCatalog(
                        opCtx);
                const auto now = VectorClock::get(opCtx)->getTime();
                DropReply dropReply;
                uassertStatusOK(mongo::dropCollection(
                    opCtx,
                    NamespaceString::kConfigsvrPlacementHistoryNamespace,
                    &dropReply,
                    DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops));
                auto initializationTime = now.configTime().asTimestamp();

                _doc.setInitializationTime(initializationTime);
            }))
        .then(_buildPhaseHandler(Phase::kUnblockDDLs,
                                 [](auto* opCtx) { unblockConflictingDDLs(opCtx); }))
        .then(_buildPhaseHandler(
            Phase::kComputeInitialization,
            [this, anchor = shared_from_this()](auto* opCtx) {
                tassert(10900200,
                        "Cannot initialize config.placementHistory without a PIT",
                        _doc.getInitializationTime().has_value());

                const auto& initTimeRef = _doc.getInitializationTime().value();
                LOGV2(10900201,
                      "Initializing config.placementHistory",
                      "initializationTime"_attr = initTimeRef);
                auto* shardingCatalogManager = ShardingCatalogManager::get(opCtx);
                // Recreate the collection and its supporting index, following the drop performed
                // within Phase::kDefineInitializationTime.
                uassertStatusOK(
                    shardingCatalogManager->createIndexForConfigPlacementHistory(opCtx));
                try {
                    shardingCatalogManager->initializePlacementHistory(opCtx, initTimeRef);
                } catch (const ExceptionFor<ErrorCodes::SnapshotTooOld>& e) {
                    // Spurious failures may arise when the chosen initializationTime falls off the
                    // snapshot read history window; when this happens, retry the initialization
                    // with a more recent timestamp (obtained after re-establishing the proper
                    // isolation constraints).
                    // This is achieved by resetting the state of the recovery doc and remapping the
                    // exception to a retryable error.
                    LOGV2_WARNING(
                        10900202,
                        "config.placementHistory initialization failed: snapshot read not "
                        "available at the requested PIT. If the problem persists, consider raising "
                        "the value of server parameter minSnapshotHistoryWindowInSeconds.",
                        "err"_attr = redact(e),
                        "initializationTime"_attr = initTimeRef);
                    auto newDoc = _doc;
                    newDoc.setPhase(Phase::kBlockDDLs);
                    newDoc.setInitializationTime(boost::none);
                    _updateStateDocument(opCtx, std::move(newDoc));
                    uasserted(ErrorCodes::ExceededTimeLimit,
                              "config.placementHistory initialization failed");
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kFinalize,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                const auto osi = getNewSession(opCtx);
                broadcastPlacementHistoryChangedNotification(
                    opCtx, _doc.getInitializationTime().value(), osi, executor, token);
                PlacementHistoryCleaner::get(opCtx)->resume(opCtx);
                ShardingLogging::get(opCtx)->logChange(opCtx, "resetPlacementHistory.end", nss());
            }))
        .onError([](const Status& status) {
            if (status == ErrorCodes::RequestAlreadyFulfilled) {
                LOGV2(10916801, "Skipping initialization of config.placementHistory");
                return Status::OK();
            };

            return status;
        });
}

}  // namespace mongo
