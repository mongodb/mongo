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

#include "mongo/db/s/drop_collection_coordinator.h"

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/participant_block_gen.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/sharded_index_catalog_commands_gen.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_index_catalog_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

void DropCollectionCoordinator::dropCollectionLocally(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      bool fromMigrate) {

    boost::optional<UUID> collectionUUID;
    {
        Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);

        // Get collectionUUID
        collectionUUID = [&]() -> boost::optional<UUID> {
            auto localCatalog = CollectionCatalog::get(opCtx);
            const auto coll = localCatalog->lookupCollectionByNamespace(opCtx, nss);
            if (coll) {
                return coll->uuid();
            }
            return boost::none;
        }();

        // Clear CollectionShardingRuntime entry.
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss)
            ->clearFilteringMetadataForDroppedCollection(opCtx);
    }

    dropCollectionShardingIndexCatalog(opCtx, nss);

    // Remove all range deletion task documents present on disk for the collection to drop. This is
    // a best-effort tentative considering that migrations are not blocked, hence some new document
    // may be inserted before actually dropping the collection.
    if (collectionUUID) {
        // The multi-document remove command cannot be run in  transactions, so run it using
        // an alternative client.
        auto newClient = opCtx->getServiceContext()->makeClient("removeRangeDeletions-" +
                                                                collectionUUID->toString());
        {
            stdx::lock_guard<Client> lk(*newClient.get());
            newClient->setSystemOperationKillableByStepdown(lk);
        }
        AlternativeClientRegion acr{newClient};
        auto executor =
            Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();

        CancelableOperationContext alternativeOpCtx(
            cc().makeOperationContext(), opCtx->getCancellationToken(), executor);

        try {
            removePersistentRangeDeletionTasksByUUID(alternativeOpCtx.get(), *collectionUUID);
        } catch (const DBException& e) {
            LOGV2_ERROR(6501601,
                        "Failed to remove persistent range deletion tasks on drop collection",
                        logAttrs(nss),
                        "collectionUUID"_attr = (*collectionUUID).toString(),
                        "error"_attr = e);
            throw;
        }
    }

    try {
        DropReply unused;
        uassertStatusOK(
            dropCollection(opCtx,
                           nss,
                           &unused,
                           DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops,
                           fromMigrate));
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // Note that even if the namespace was not found we have to execute the code below!
        LOGV2_DEBUG(5280920,
                    1,
                    "Namespace not found while trying to delete local collection",
                    logAttrs(nss));
    }

    // Force the refresh of the catalog cache to purge outdated information. Note also that this
    // code is indirectly used to notify to secondary nodes to clear their filtering information.
    const auto catalog = Grid::get(opCtx)->catalogCache();
    uassertStatusOK(catalog->getCollectionRoutingInfoWithRefresh(opCtx, nss));
    CatalogCacheLoader::get(opCtx).waitForCollectionFlush(opCtx, nss);

    // Ensures the remove of range deletions and the refresh of the catalog cache will be waited for
    // majority at the end of the command
    repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
}

ExecutorFuture<void> DropCollectionCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor = executor, anchor = shared_from_this()] {
            if (_isPre70Compatible())
                return;

            if (_doc.getPhase() < Phase::kFreezeCollection)
                _checkPreconditionsAndSaveArgumentsOnDoc();
        })
        .then(_buildPhaseHandler(Phase::kFreezeCollection,
                                 [this, executor = executor, anchor = shared_from_this()] {
                                     _freezeMigrations(executor);
                                 }))

        .then([this, executor = executor, anchor = shared_from_this()] {
            if (_isPre70Compatible())
                return;

            _buildPhaseHandler(Phase::kEnterCriticalSection,
                               [this, executor = executor, anchor = shared_from_this()] {
                                   _enterCriticalSection(executor);
                               })();
        })
        .then(_buildPhaseHandler(Phase::kDropCollection,
                                 [this, executor = executor, anchor = shared_from_this()] {
                                     _commitDropCollection(executor);
                                 }))
        .then([this, executor = executor, anchor = shared_from_this()] {
            if (_isPre70Compatible())
                return;

            _buildPhaseHandler(Phase::kReleaseCriticalSection,
                               [this, executor = executor, anchor = shared_from_this()] {
                                   _exitCriticalSection(executor);
                               })();
        });
}

void DropCollectionCoordinator::_saveCollInfo(OperationContext* opCtx) {

    try {
        auto coll = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss());
        _doc.setCollInfo(std::move(coll));
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The collection is not sharded or doesn't exist.
        _doc.setCollInfo(boost::none);
    }
}

void DropCollectionCoordinator::_checkPreconditionsAndSaveArgumentsOnDoc() {
    auto opCtxHolder = cc().makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    getForwardableOpMetadata().setOn(opCtx);

    // If the request had an expected UUID for the collection being dropped, we should verify that
    // it matches the one from the local catalog
    {
        AutoGetCollection coll{opCtx,
                               nss(),
                               MODE_IS,
                               AutoGetCollection::Options{}
                                   .viewMode(auto_get_collection::ViewMode::kViewsPermitted)
                                   .expectedUUID(_doc.getCollectionUUID())};
    }

    _saveCollInfo(opCtx);
}

void DropCollectionCoordinator::_freezeMigrations(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    auto opCtxHolder = cc().makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    getForwardableOpMetadata().setOn(opCtx);

    if (_isPre70Compatible()) {
        _saveCollInfo(opCtx);

        // TODO SERVER-73627: Remove once 7.0 becomes last LTS.
        // This check can be removed since it is also present in _checkPreconditions.
        {
            AutoGetCollection coll{opCtx,
                                   nss(),
                                   MODE_IS,
                                   AutoGetCollection::Options{}
                                       .viewMode(auto_get_collection::ViewMode::kViewsPermitted)
                                       .expectedUUID(_doc.getCollectionUUID())};
        }

        // Persist the collection info before sticking to using it's uuid. This ensures this node is
        // still the RS primary, so it was also the primary at the moment we read the collection
        // metadata.
        _updateStateDocument(opCtx, StateDoc(_doc));
    }

    BSONObjBuilder logChangeDetail;
    if (_doc.getCollInfo()) {
        logChangeDetail.append("collectionUUID", _doc.getCollInfo()->getUuid().toBSON());
    }

    ShardingLogging::get(opCtx)->logChange(
        opCtx, "dropCollection.start", nss().ns(), logChangeDetail.obj());

    if (_doc.getCollInfo()) {
        sharding_ddl_util::stopMigrations(opCtx, nss(), _doc.getCollInfo()->getUuid());
    }
}

void DropCollectionCoordinator::_enterCriticalSection(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    LOGV2_DEBUG(7038100, 2, "Acquiring critical section", logAttrs(nss()));

    auto opCtxHolder = cc().makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    getForwardableOpMetadata().setOn(opCtx);

    _updateSession(opCtx);
    ShardsvrParticipantBlock blockCRUDOperationsRequest(nss());
    blockCRUDOperationsRequest.setBlockType(mongo::CriticalSectionBlockTypeEnum::kReadsAndWrites);
    blockCRUDOperationsRequest.setReason(_critSecReason);
    blockCRUDOperationsRequest.setAllowViews(true);

    const auto cmdObj =
        CommandHelpers::appendMajorityWriteConcern(blockCRUDOperationsRequest.toBSON({}));
    sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx,
        nss().db(),
        cmdObj.addFields(getCurrentSession().toBSON()),
        Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx),
        **executor);

    LOGV2_DEBUG(7038101, 2, "Acquired critical section", logAttrs(nss()));
}

void DropCollectionCoordinator::_commitDropCollection(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    auto opCtxHolder = cc().makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    getForwardableOpMetadata().setOn(opCtx);

    const auto collIsSharded = bool(_doc.getCollInfo());

    LOGV2_DEBUG(5390504, 2, "Dropping collection", logAttrs(nss()), "sharded"_attr = collIsSharded);

    _updateSession(opCtx);
    if (collIsSharded) {
        invariant(_doc.getCollInfo());
        const auto& coll = _doc.getCollInfo().value();

        // This always runs in the shard role so should use a cluster transaction to guarantee
        // targeting the config server.
        bool useClusterTransaction = true;
        sharding_ddl_util::removeCollAndChunksMetadataFromConfig(
            opCtx,
            Grid::get(opCtx)->shardRegistry()->getConfigShard(),
            Grid::get(opCtx)->catalogClient(),
            coll,
            ShardingCatalogClient::kMajorityWriteConcern,
            getCurrentSession(),
            useClusterTransaction,
            **executor);
    }

    // Remove tags even if the collection is not sharded or didn't exist
    _updateSession(opCtx);
    sharding_ddl_util::removeTagsMetadataFromConfig(opCtx, nss(), getCurrentSession());

    // get a Lsid and an incremented txnNumber. Ensures we are the primary
    _updateSession(opCtx);

    const auto primaryShardId = ShardingState::get(opCtx)->shardId();

    // We need to send the drop to all the shards because both movePrimary and
    // moveChunk leave garbage behind for sharded collections.
    auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    // Remove primary shard from participants
    participants.erase(std::remove(participants.begin(), participants.end(), primaryShardId),
                       participants.end());

    sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
        opCtx, nss(), participants, **executor, getCurrentSession(), true /*fromMigrate*/);

    // The sharded collection must be dropped on the primary shard after it has been
    // dropped on all of the other shards to ensure it can only be re-created as
    // unsharded with a higher optime than all of the drops.
    sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
        opCtx, nss(), {primaryShardId}, **executor, getCurrentSession(), false /*fromMigrate*/);

    // Remove potential query analyzer document only after purging the collection from
    // the catalog. This ensures no leftover documents referencing an old incarnation of
    // a collection.
    sharding_ddl_util::removeQueryAnalyzerMetadataFromConfig(opCtx, nss(), boost::none);

    ShardingLogging::get(opCtx)->logChange(opCtx, "dropCollection", nss().ns());
    LOGV2(5390503, "Collection dropped", logAttrs(nss()));
}

void DropCollectionCoordinator::_exitCriticalSection(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    LOGV2_DEBUG(7038102, 2, "Releasing critical section", logAttrs(nss()));

    auto opCtxHolder = cc().makeOperationContext();
    auto* opCtx = opCtxHolder.get();
    getForwardableOpMetadata().setOn(opCtx);

    _updateSession(opCtx);
    ShardsvrParticipantBlock unblockCRUDOperationsRequest(nss());
    unblockCRUDOperationsRequest.setBlockType(CriticalSectionBlockTypeEnum::kUnblock);
    unblockCRUDOperationsRequest.setReason(_critSecReason);
    unblockCRUDOperationsRequest.setAllowViews(true);

    const auto cmdObj =
        CommandHelpers::appendMajorityWriteConcern(unblockCRUDOperationsRequest.toBSON({}));
    sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx,
        nss().db(),
        cmdObj.addFields(getCurrentSession().toBSON()),
        Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx),
        **executor);

    LOGV2_DEBUG(7038103, 2, "Released critical section", logAttrs(nss()));
}

}  // namespace mongo
