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


#include "mongo/db/global_catalog/ddl/convert_to_capped_coordinator.h"

#include "mongo/db/collection_crud/capped_utils.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/notify_sharding_event_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/ddl/list_collections_gen.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/executor/async_rpc.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

MONGO_FAIL_POINT_DEFINE(convertToCappedFailBeforeCappingTheCollection);
MONGO_FAIL_POINT_DEFINE(convertToCappedFailAfterCappingTheCollection);

namespace {

void logConvertToCappedOnChangelog(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const long long size,
                                   bool start,
                                   const boost::optional<UUID>& collectionUuid = boost::none) {
    const auto message = [&]() {
        BSONObjBuilder bob;
        bob.append("size", size);
        if (collectionUuid) {
            bob.append("uuid", collectionUuid->toString());
        }
        return bob.obj();
    }();

    ShardingLogging::get(opCtx)->logChange(opCtx,
                                           str::stream()
                                               << "convertToCapped." << (start ? "start" : "end"),
                                           nss,
                                           message,
                                           defaultMajorityWriteConcernDoNotUse());
}

void convertToCappedOnShard(OperationContext* opCtx,
                            const NamespaceString& nss,
                            long long size,
                            const ShardId& shardId,
                            const UUID& targetUUID,
                            const OperationSessionInfo& osi,
                            const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                            const CancellationToken& token) {
    ShardsvrConvertToCappedParticipant request(nss);
    request.setSize(size);
    request.setTargetUUID(targetUUID);

    generic_argument_util::setMajorityWriteConcern(request);
    generic_argument_util::setOperationSessionInfo(request, osi);

    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrConvertToCappedParticipant>>(
        **executor, token, request);
    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, {shardId});
}

bool isCollectionCappedWithRequestedSize(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         long long size,
                                         const ShardId& shardId,
                                         const UUID& expectedUUID) {
    ListCollections listCollections;
    listCollections.setDbName(nss.dbName());

    BSONObjBuilder filterBuilder;
    expectedUUID.appendToBuilder(&filterBuilder, "info.uuid"_sd);
    filterBuilder.append("name", nss.coll());
    filterBuilder.append("options.capped", true);
    filterBuilder.append("options.size", size);
    listCollections.setFilter(filterBuilder.obj());

    const auto destinationShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));

    auto collectionResponse =
        uassertStatusOK(destinationShard->runExhaustiveCursorCommand(
                            opCtx,
                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                            nss.dbName(),
                            listCollections.toBSON(),
                            Seconds(30)))
            .docs;

    return !collectionResponse.empty();
}


}  // namespace

void ConvertToCappedCoordinator::_checkPreconditions(OperationContext* opCtx) {

    // Preemptively check that the size will be correctly parsed by the subsequent convertToCapped
    // request.
    uassertStatusOK(CollectionOptions::checkAndAdjustCappedSize(_doc.getSize()));

    {
        const auto acquisition = acquireCollectionOrViewMaybeLockFree(
            opCtx,
            CollectionAcquisitionRequest(nss(),
                                         PlacementConcern::kPretendUnsharded,
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kRead));

        // Since `convertToCapped`  internally calls `cloneCollectionAsCapped`, the error message
        // mentions the latter (to keep the message consistent between RSs and sharded clusters)
        uassert(ErrorCodes::CommandNotSupportedOnView,
                str::stream() << "cloneCollectionAsCapped not supported for views: "
                              << nss().toStringForErrorMsg(),
                !acquisition.isView());

        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "source collection " << nss().toStringForErrorMsg()
                              << " does not exist",
                acquisition.collectionExists());

        // Check if the collection is already capped. This check can entirely be done in the
        // DBPrimary even if the collection lives on another shard since a valid collection metadata
        // must exist always on the DBPrimary.
        const auto& coll = acquisition.getCollectionPtr();
        uassert(ErrorCodes::RequestAlreadyFulfilled,
                str::stream() << "Collection " << toStringForLogging(nss()) << " already capped",
                !(coll->isCapped() && (coll->getCappedMaxSize() == _doc.getSize())));
    }

    const auto chunkManager = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(opCtx, nss()));

    uassert(ErrorCodes::NamespaceCannotBeSharded,
            "Can't convert a sharded collection to a capped collection",
            !chunkManager.isSharded());

    if (chunkManager.hasRoutingTable()) {
        tassert(10644528,
                "Expected tracked collection to be capped to be unsplittable",
                chunkManager.isUnsplittable());

        uassert(ErrorCodes::CommandNotSupportedOnView,
                "Can't convert a timeseries collection to a capped collection",
                !chunkManager.getTimeseriesFields());

        std::set<ShardId> shards;
        chunkManager.getAllShardIds(&shards);
        tassert(10644529,
                "Expected the collection to be capped to have a chunk on some shard",
                !shards.empty());
        _doc.setDataShard(*(shards.begin()));

        _doc.setOriginalCollection(sharding_ddl_util::getCollectionFromConfigServer(opCtx, nss()));
    } else {
        // The collection is located on the DBPrimary if it's not tracked by the sharding catalog.
        const auto& selfShardId = ShardingState::get(opCtx)->shardId();
        _doc.setDataShard(selfShardId);
    }

    // Compute and persist the targetUUID of the newly capped collection before sending
    // the capped command to the dataShard. Make sure any existing collection has that UUID.
    auto targetUUID = UUID::gen();
    while (true) {
        try {
            const auto acquisition = acquireCollectionOrViewMaybeLockFree(
                opCtx,
                CollectionAcquisitionRequest(NamespaceStringOrUUID{nss().dbName(), targetUUID},
                                             PlacementConcern::kPretendUnsharded,
                                             repl::ReadConcernArgs::get(opCtx),
                                             AcquisitionPrerequisites::kRead));

            if (acquisition.collectionExists()) {
                targetUUID = UUID::gen();
                continue;
            }
        } catch (const DBException&) {
        }
        break;
    }

    _doc.setTargetUUID(targetUUID);
}

ExecutorFuture<void> ConvertToCappedCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor = executor, anchor = shared_from_this()]() {
            if (_doc.getPhase() == Phase::kUnset) {
                auto opCtxHolder = makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                // Best effort check of preconditions
                _checkPreconditions(opCtx);
            }
        })
        .then(_buildPhaseHandler(
            Phase::kAcquireCriticalSectionOnCoordinator,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                ShardingRecoveryService::get(opCtx)->acquireRecoverableCriticalSectionBlockWrites(
                    opCtx,
                    nss(),
                    _critSecReason,
                    ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

                // Check preconditions again under the critical section because we're guaranteed no
                // catalog changes can happen at this point.
                _checkPreconditions(opCtx);

                ShardingRecoveryService::get(opCtx)
                    ->promoteRecoverableCriticalSectionToBlockAlsoReads(
                        opCtx,
                        nss(),
                        _critSecReason,
                        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (status == ErrorCodes::RequestAlreadyFulfilled) {
                // If the collection is already capped, jump directly to the last phase
                _enterPhase(Phase::kReleaseCriticalSectionOnCoordinator);
                return Status::OK();
            };

            return status;
        })
        .then(_buildPhaseHandler(
            Phase::kDropCollectionOnShardsNotOwningData,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                if (_doc.getOriginalCollection().has_value()) {
                    // Drop collection form any shard that is not db primary and does not owning
                    // data (getting rid of possible stale incarnations due to SERVER-87010).
                    std::vector<ShardId> participantsNotOwningData;

                    auto allShards = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                    const auto& dataShardId = *_doc.getDataShard();
                    const auto& selfShardId = ShardingState::get(opCtx)->shardId();
                    for (const auto& shardId : allShards) {
                        if (shardId != dataShardId && shardId != selfShardId) {
                            participantsNotOwningData.push_back(shardId);
                        }
                    }

                    const auto& session = getNewSession(opCtx);
                    sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
                        opCtx,
                        nss(),
                        participantsNotOwningData,
                        **executor,
                        session,
                        true /* fromMigrate */,
                        false /* dropSystemCollections */,
                        _doc.getOriginalCollection()->getUuid());
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kAcquireCriticalSectionOnDataShard,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                if (*_doc.getDataShard() != ShardingState::get(opCtx)->shardId()) {
                    _enterCriticalSectionOnDataShard(
                        opCtx, executor, token, CriticalSectionBlockTypeEnum::kReadsAndWrites);
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kConvertCollectionToCappedOnDataShard,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                logConvertToCappedOnChangelog(opCtx, nss(), _doc.getSize(), true /* start */);

                if (MONGO_unlikely(convertToCappedFailBeforeCappingTheCollection.shouldFail())) {
                    uasserted(ErrorCodes::InternalError,
                              "Reproducing an error. This is part of a test.");
                }

                {
                    const auto session = getNewSession(opCtx);
                    convertToCappedOnShard(opCtx,
                                           nss(),
                                           _doc.getSize(),
                                           *_doc.getDataShard(),
                                           *_doc.getTargetUUID(),
                                           session,
                                           executor,
                                           token);
                }

                if (MONGO_unlikely(convertToCappedFailAfterCappingTheCollection.shouldFail())) {
                    convertToCappedFailAfterCappingTheCollection.pauseWhileSet();
                    uasserted(ErrorCodes::InternalError,
                              "Reproducing an error. This is part of a test.");
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kConvertCollectionToCappedOnCoordinator,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                // The collection must be updated also on the DBPrimary shard in case the
                // dataShard is not the DBPrimary shard.
                if (*_doc.getDataShard() != ShardingState::get(opCtx)->shardId()) {
                    // If the primary is not a data bearing shard, op entries must not be visible to
                    // change stream readers.
                    convertToCapped(
                        opCtx, nss(), _doc.getSize(), true /*fromMigrate*/, *_doc.getTargetUUID());
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kUpdateShardingCatalog,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                const auto [localCollUuid, defaultCollator] = [&]() {
                    auto collection = acquireCollectionMaybeLockFree(
                        opCtx,
                        CollectionAcquisitionRequest(nss(),
                                                     PlacementConcern::kPretendUnsharded,
                                                     repl::ReadConcernArgs::get(opCtx),
                                                     AcquisitionPrerequisites::kRead));
                    auto defaultCollator = collection.getCollectionPtr()->getDefaultCollator();
                    return std::make_tuple(collection.uuid(),
                                           defaultCollator ? defaultCollator->getSpec().toBSON()
                                                           : BSONObj());
                }();

                if (_doc.getOriginalCollection().has_value()) {
                    {
                        const auto session = getNewSession(opCtx);
                        // Delete the sharding catalog entries referring the previous incarnation
                        sharding_ddl_util::removeCollAndChunksMetadataFromConfig(
                            opCtx,
                            Grid::get(opCtx)->shardRegistry()->getConfigShard(),
                            Grid::get(opCtx)->catalogClient(),
                            *_doc.getOriginalCollection(),
                            defaultMajorityWriteConcernDoNotUse(),
                            session,
                            **executor);
                    }

                    const auto [coll, chunkDescriptor] =
                        sharding_ddl_util::generateMetadataForUnsplittableCollectionCreation(
                            opCtx, nss(), localCollUuid, defaultCollator, *(_doc.getDataShard()));

                    auto createCollectionOnShardingCatalogOps =
                        sharding_ddl_util::getOperationsToCreateOrShardCollectionOnShardingCatalog(
                            coll,
                            chunkDescriptor,
                            chunkDescriptor.front().getVersion(),
                            {*(_doc.getDataShard())});

                    sharding_ddl_util::runTransactionWithStmtIdsOnShardingCatalog(
                        opCtx,
                        **executor,
                        getNewSession(opCtx),
                        std::move(createCollectionOnShardingCatalogOps));

                    // Checkpoint the configTime to ensure that, in the case of a stepdown/crash,
                    // the new primary will start-up from a configTime that is inclusive of the
                    // metadata changes that were committed on the sharding catalog.
                    VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);

                    // When targeting a tracked collection, change stream readers need to receive
                    // a post-commit notification about the placement change affecting the uncapped
                    // incarnation.
                    const auto& commitTime = coll.getTimestamp();

                    NamespacePlacementChanged notification(nss(), commitTime);
                    auto buildNewSessionFn = [this](OperationContext* opCtx) {
                        return getNewSession(opCtx);
                    };

                    sharding_ddl_util::generatePlacementChangeNotificationOnShard(
                        opCtx,
                        notification,
                        *_doc.getDataShard(),
                        buildNewSessionFn,
                        executor,
                        token);
                }

                logConvertToCappedOnChangelog(
                    opCtx, nss(), _doc.getSize(), false /* end */, localCollUuid);
            }))
        .then(_buildPhaseHandler(
            Phase::kReleaseCriticalSectionOnDataShard,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                if (*_doc.getDataShard() != ShardingState::get(opCtx)->shardId()) {
                    _exitCriticalSectionOnDataShard(opCtx, executor, token);
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kReleaseCriticalSectionOnCoordinator,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
                    opCtx,
                    nss(),
                    _critSecReason,
                    defaultMajorityWriteConcernDoNotUse(),
                    ShardingRecoveryService::FilteringMetadataClearer(),
                    true /* throwIfReasonDiffers */);
            }))
        .onError([this, executor = executor, anchor = shared_from_this()](const Status& status) {
            const auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            // If the convertToCapped command fails on the dataShard, not retry the operation if
            // we can ensure the collection hasn't been capped.
            if (_doc.getPhase() == Phase::kConvertCollectionToCappedOnDataShard) {
                try {
                    // Perform a noop write on the participant in order to advance the txnNumber
                    // for this coordinator's lsid so that requests with older txnNumbers can no
                    // longer execute.
                    //
                    // Additionally we want to wait for the completion of any ongoing command to
                    // ensure that the subsequent check will see the correct status of the
                    // collection.
                    _performNoopRetryableWriteOnParticipantShardsAndConfigsvr(
                        opCtx, getNewSession(opCtx), **executor);

                    if (!isCollectionCappedWithRequestedSize(opCtx,
                                                             nss(),
                                                             _doc.getSize(),
                                                             *_doc.getDataShard(),
                                                             *_doc.getTargetUUID())) {
                        // The conversion to capped failed so there was no catalog change, it is
                        // then safe to simply return the error to the router that will retry
                        triggerCleanup(opCtx, status);
                        MONGO_UNREACHABLE_TASSERT(10083518);
                    }
                } catch (const DBException& e) {
                    LOGV2_WARNING(8577202,
                                  "Failed to check if the collection has been capped.",
                                  logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                                           "error"_attr = redact(e)});
                }
            }

            // If the coordinator succeeded to convert the collection to capped and the collection
            // is tracked, the sharding catalog must be updated. Thus throw the error and retry
            // relying on _mustAlwaysMakeProgress that will always be true reached this phase.
            if (_mustAlwaysMakeProgress() || _isRetriableErrorForDDLCoordinator(status)) {
                // Retry the operation.
                return status;
            }

            if (_doc.getPhase() >= Phase::kAcquireCriticalSectionOnCoordinator) {
                triggerCleanup(opCtx, status);
                MONGO_UNREACHABLE_TASSERT(10083519);
            }

            return status;
        });
}

bool ConvertToCappedCoordinator::_mustAlwaysMakeProgress() {
    // If the collection was originally tracked on the sharding catalog, the coodinator must always
    // make forward progress after converting the collection to capped in order to align local and
    // sharding catalog.
    const bool isCollectionTrackedOnTheShardingCatalog = _doc.getOriginalCollection().has_value();
    return isCollectionTrackedOnTheShardingCatalog &&
        _doc.getPhase() >= Phase::kConvertCollectionToCappedOnDataShard;
}

ExecutorFuture<void> ConvertToCappedCoordinator::_cleanupOnAbort(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const Status& status) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, token, executor = executor, status, anchor = shared_from_this()] {
            const auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            if (_doc.getPhase() >= Phase::kAcquireCriticalSectionOnCoordinator) {
                if (*_doc.getDataShard() != ShardingState::get(opCtx)->shardId()) {
                    _exitCriticalSectionOnDataShard(opCtx, executor, token);
                }

                ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
                    opCtx,
                    nss(),
                    _critSecReason,
                    defaultMajorityWriteConcernDoNotUse(),
                    ShardingRecoveryService::NoCustomAction(),
                    false /* throwIfReasonDiffers */);
            }
        });
}

void ConvertToCappedCoordinator::_enterCriticalSectionOnDataShard(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token,
    CriticalSectionBlockTypeEnum blockType) {
    ShardsvrParticipantBlock blockCRUDOperationsRequest(nss());
    blockCRUDOperationsRequest.setBlockType(blockType);
    blockCRUDOperationsRequest.setReason(_critSecReason);

    generic_argument_util::setMajorityWriteConcern(blockCRUDOperationsRequest);
    generic_argument_util::setOperationSessionInfo(blockCRUDOperationsRequest,
                                                   getNewSession(opCtx));
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
        **executor, token, blockCRUDOperationsRequest);
    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, {*_doc.getDataShard()});
}

void ConvertToCappedCoordinator::_exitCriticalSectionOnDataShard(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    ShardsvrParticipantBlock unblockCRUDOperationsRequest(nss());
    unblockCRUDOperationsRequest.setBlockType(CriticalSectionBlockTypeEnum::kUnblock);
    unblockCRUDOperationsRequest.setReason(_critSecReason);
    unblockCRUDOperationsRequest.setClearFilteringMetadata(true);

    generic_argument_util::setMajorityWriteConcern(unblockCRUDOperationsRequest);
    generic_argument_util::setOperationSessionInfo(unblockCRUDOperationsRequest,
                                                   getNewSession(opCtx));
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
        **executor, token, unblockCRUDOperationsRequest);
    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, {*_doc.getDataShard()});
}

logv2::DynamicAttributes ConvertToCappedCoordinator::getCoordinatorLogAttrs() const {
    return logv2::DynamicAttributes{getBasicCoordinatorAttrs(),
                                    "size"_attr = _doc.getSize(),
                                    "targetUUID"_attr = _doc.getTargetUUID()};
}

void ConvertToCappedCoordinator::_performNoopRetryableWriteOnParticipantShardsAndConfigsvr(
    OperationContext* opCtx,
    const OperationSessionInfo& osi,
    const std::shared_ptr<executor::TaskExecutor>& executor) {
    const ShardId configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard()->getId();
    const ShardId coordShardId = ShardingState::get(opCtx)->shardId();

    tassert(8577203, "Data shard not found.", _doc.getDataShard());
    const ShardId dataShard = *_doc.getDataShard();

    std::vector<ShardId> participants;
    participants.emplace_back(coordShardId);

    if (configShard != coordShardId) {
        participants.emplace_back(configShard);
    }
    if (dataShard != coordShardId && dataShard != configShard) {
        participants.emplace_back(dataShard);
    }
    sharding_ddl_util::performNoopRetryableWriteOnShards(opCtx, participants, osi, executor);
}


}  // namespace mongo
