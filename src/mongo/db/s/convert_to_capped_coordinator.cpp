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


#include "mongo/db/s/convert_to_capped_coordinator.h"

#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/sharding_state.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

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
                                           ShardingCatalogClient::kMajorityWriteConcern);
}


bool isCollectionCappedWithRequestedSize(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const long long size) {
    const auto acquisition = acquireCollectionOrViewMaybeLockFree(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     AcquisitionPrerequisites::kPretendUnsharded,
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead));

    // Since `convertToCapped`  internally calls `cloneCollectionAsCapped`, the error message
    // mentions the latter (to keep the message consistent between RSs and sharded clusters)
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "cloneCollectionAsCapped not supported for views: "
                          << nss.toStringForErrorMsg(),
            !acquisition.isView());

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "source collection " << nss.toStringForErrorMsg() << " does not exist",
            acquisition.collectionExists());

    const auto& coll = acquisition.getCollectionPtr();
    return coll->isCapped() && (coll->getCappedMaxDocs() == size);
}

}  // namespace

void ConvertToCappedCoordinator::_checkPreconditions(OperationContext* opCtx) {
    uassert(ErrorCodes::RequestAlreadyFulfilled,
            str::stream() << "Collection " << toStringForLogging(nss()) << " already capped",
            !isCollectionCappedWithRequestedSize(opCtx, nss(), _doc.getSize()));

    const auto& [chunkManager, _] = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithPlacementRefresh(opCtx,
                                                                                       nss()));

    uassert(ErrorCodes::IllegalOperation,
            "Can't convert a sharded collection to a capped collection",
            !chunkManager.isSharded());

    if (chunkManager.hasRoutingTable()) {
        invariant(chunkManager.isUnsplittable());

        const auto& selfShardId = ShardingState::get(opCtx)->shardId();
        std::set<ShardId> shards;
        chunkManager.getAllShardIds(&shards);

        // TODO SERVER-85772: allow convertToCapped to work on unsplittable collections
        // located outside the dbPrimary
        uassert(ErrorCodes::IllegalOperation,
                "Can't convert to capped a collection residing outside the primary shard",
                *(shards.begin()) == selfShardId);

        _doc.setIsTrackedCollection(true);
    }
}

ExecutorFuture<void> ConvertToCappedCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor = executor, anchor = shared_from_this()]() {
            if (_doc.getPhase() == Phase::kUnset) {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);
                // Best effort check of preconditions
                _checkPreconditions(opCtx);
            }
        })
        .then(_buildPhaseHandler(
            Phase::kAcquireCriticalSection,
            [this, token, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                ShardingRecoveryService::get(opCtx)->acquireRecoverableCriticalSectionBlockWrites(
                    opCtx, nss(), _critSecReason, ShardingCatalogClient::kLocalWriteConcern);

                // Check preconditions again under the critical section because we're guaranteed no
                // catalog changes can happen at this point.
                _checkPreconditions(opCtx);

                ShardingRecoveryService::get(opCtx)
                    ->promoteRecoverableCriticalSectionToBlockAlsoReads(
                        opCtx, nss(), _critSecReason, ShardingCatalogClient::kLocalWriteConcern);
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (status == ErrorCodes::RequestAlreadyFulfilled) {
                // If the collection is already capped, jump directly to the last phase
                _enterPhase(Phase::kReleaseCriticalSection);
                return Status::OK();
            };

            return status;
        })
        .then(_buildPhaseHandler(
            Phase::kConvertCollectionToCapped,
            [this, token, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);
                logConvertToCappedOnChangelog(opCtx, nss(), _doc.getSize(), true /* start */);

                try {
                    convertToCapped(opCtx, nss(), _doc.getSize());
                } catch (const DBException& ex) {
                    if (!isCollectionCappedWithRequestedSize(opCtx, nss(), _doc.getSize())) {
                        // The conversion to capped failed so there was no catalog change, it is
                        // then safe to simply return the error to the router that will retry
                        triggerCleanup(opCtx, ex.toStatus());
                        MONGO_UNREACHABLE;
                    }

                    // If the coordinator succeeded to convert the collection to capped, the
                    // sharding catalog must be updated. Thus throw the error and rely on
                    // _mustAlwaysMakeProgress that will always be true reached this phase.
                    throw;
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kUpdateShardingCatalog,
            [this, token, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                const auto localCollUuid = [&]() {
                    auto collection = acquireCollectionMaybeLockFree(
                        opCtx,
                        CollectionAcquisitionRequest(nss(),
                                                     AcquisitionPrerequisites::kPretendUnsharded,
                                                     repl::ReadConcernArgs::get(opCtx),
                                                     AcquisitionPrerequisites::kRead));
                    return collection.uuid();
                }();

                if (_doc.getIsTrackedCollection()) {
                    if (auto optCollEntry =
                            sharding_ddl_util::getCollectionFromConfigServer(opCtx, nss())) {
                        // This always runs in the shard role so should use a cluster transaction to
                        // guarantee targeting the config server
                        const bool useClusterTransaction{true};

                        // Delete the sharding catalog entry referring the previous incarnation
                        sharding_ddl_util::removeCollAndChunksMetadataFromConfig(
                            opCtx,
                            Grid::get(opCtx)->shardRegistry()->getConfigShard(),
                            Grid::get(opCtx)->catalogClient(),
                            *optCollEntry,
                            ShardingCatalogClient::kMajorityWriteConcern,
                            getNewSession(opCtx),
                            useClusterTransaction,
                            **executor);
                    }

                    auto createCollectionOnShardingCatalogOps = sharding_ddl_util::
                        getOperationsToCreateUnsplittableCollectionOnShardingCatalog(
                            opCtx, nss(), localCollUuid, ShardingState::get(opCtx)->shardId());
                    sharding_ddl_util::runTransactionWithStmtIdsOnShardingCatalog(
                        opCtx,
                        **executor,
                        getNewSession(opCtx),
                        std::move(createCollectionOnShardingCatalogOps));
                }

                logConvertToCappedOnChangelog(
                    opCtx, nss(), _doc.getSize(), false /* end */, localCollUuid);
            }))
        .then(_buildPhaseHandler(
            Phase::kReleaseCriticalSection,
            [this, token, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                {
                    const auto preImageColl = acquireCollection(
                        opCtx,
                        CollectionAcquisitionRequest(nss(),
                                                     AcquisitionPrerequisites::kPretendUnsharded,
                                                     repl::ReadConcernArgs::get(opCtx),
                                                     AcquisitionPrerequisites::kRead),
                        MODE_IS);

                    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx,
                                                                                         nss())
                        ->clearFilteringMetadata(opCtx);
                }
                ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
                    opCtx,
                    nss(),
                    _critSecReason,
                    ShardingCatalogClient::kMajorityWriteConcern,
                    true /* throwIfReasonDiffers */);
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (_mustAlwaysMakeProgress() || _isRetriableErrorForDDLCoordinator(status)) {
                return status;
            }

            if (_doc.getPhase() >= Phase::kAcquireCriticalSection) {
                const auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);
                triggerCleanup(opCtx, status);
            }

            return status;
        });
}

bool ConvertToCappedCoordinator::_mustAlwaysMakeProgress() {
    // If the collection was originally tracked on the sharding catalog, the coodinator must always
    // make forward progress after converting the collection to capped in order to align local and
    // sharding catalog.
    return _doc.getIsTrackedCollection() && _doc.getPhase() >= Phase::kConvertCollectionToCapped;
}

ExecutorFuture<void> ConvertToCappedCoordinator::_cleanupOnAbort(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const Status& status) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, token, executor = executor, status, anchor = shared_from_this()] {
            const auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

            if (_doc.getPhase() >= Phase::kAcquireCriticalSection) {
                ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
                    opCtx,
                    nss(),
                    _critSecReason,
                    ShardingCatalogClient::kMajorityWriteConcern,
                    false /* throwIfReasonDiffers */);
            }
        });
}

}  // namespace mongo
