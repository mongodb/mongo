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

#include "mongo/db/global_catalog/ddl/drop_collection_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/notify_sharding_event_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/ddl/drop_gen.h"
#include "mongo/db/local_catalog/drop_collection.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/participant_block_gen.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/logv2/log.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

void DropCollectionCoordinator::dropCollectionLocally(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      bool fromMigrate,
                                                      bool dropSystemCollections,
                                                      const boost::optional<UUID>& expectedUUID,
                                                      bool requireCollectionEmpty) {

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

        if (collectionUUID && expectedUUID && *collectionUUID != *expectedUUID) {
            // Ignore the drop collection if the collections exists locally and there is a mismatch
            // between the current uuid and the expected one.
            LOGV2_DEBUG(8363400,
                        1,
                        "Skipping dropping the collection due to mismatched collection uuid",
                        "nss"_attr = nss,
                        "uuid"_attr = *collectionUUID,
                        "expectedUUID"_attr = *expectedUUID);
            return;
        }

        if (requireCollectionEmpty) {
            bool isEmpty = [&]() {
                auto localCatalog = CollectionCatalog::get(opCtx);
                const auto coll = localCatalog->lookupCollectionByNamespace(opCtx, nss);
                if (coll) {
                    return coll->isEmpty(opCtx);
                }
                return true;
            }();
            if (!isEmpty) {
                // Ignore the drop collection if the collection has records locally and it's
                // explicitely required to skip non-empty collections.
                LOGV2_DEBUG(9525700,
                            1,
                            "Skipping dropping the collection because it is not empty",
                            "nss"_attr = nss);
                return;
            }
        }
    }

    // Remove all range deletion task documents present on disk for the collection to drop. This is
    // a best-effort tentative considering that migrations are not blocked, hence some new document
    // may be inserted before actually dropping the collection.
    if (collectionUUID) {
        // The multi-document remove command cannot be run in  transactions, so run it using
        // an alternative client.
        auto newClient =
            opCtx->getService()->makeClient("removeRangeDeletions-" + collectionUUID->toString());
        AlternativeClientRegion acr{newClient};
        auto executor =
            Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();

        CancelableOperationContext alternativeOpCtx(
            cc().makeOperationContext(), opCtx->getCancellationToken(), executor);

        try {
            rangedeletionutil::removePersistentRangeDeletionTasksByUUID(alternativeOpCtx.get(),
                                                                        *collectionUUID);
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
        uassertStatusOK(dropCollection(
            opCtx,
            nss,
            &unused,
            (dropSystemCollections
                 ? DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops
                 : DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops),
            fromMigrate));
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // Note that even if the namespace was not found we have to execute the code below!
        LOGV2_DEBUG(5280920,
                    1,
                    "Namespace not found while trying to delete local collection",
                    logAttrs(nss));
    }

    // Force the refresh of the filtering metadata cache to purge outdated information.
    // The logic below will cause config.cache.collections.<nss> to be dropped and secondary nodes
    // to clean their filtering metadata (once the flushed data get replicated).
    FilteringMetadataCache::get(opCtx)->forceCollectionPlacementRefresh(opCtx, nss);
    FilteringMetadataCache::get(opCtx)->waitForCollectionFlush(opCtx, nss);

    // Ensures that the removal of filtering metadata and range deletions will be waited
    // for majority at the end of the command.
    repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
}

ExecutorFuture<void> DropCollectionCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor = executor, anchor = shared_from_this()] {
            if (_doc.getPhase() < Phase::kFreezeCollection)
                _checkPreconditionsAndSaveArgumentsOnDoc();
        })
        .then(_buildPhaseHandler(Phase::kFreezeCollection,
                                 [this, executor = executor, anchor = shared_from_this()](
                                     auto* opCtx) { _freezeMigrations(opCtx, executor); }))

        .then(
            _buildPhaseHandler(Phase::kEnterCriticalSection,
                               [this, token, executor = executor, anchor = shared_from_this()](
                                   auto* opCtx) { _enterCriticalSection(opCtx, executor, token); }))
        .then(
            _buildPhaseHandler(Phase::kDropCollection,
                               [this, token, executor = executor, anchor = shared_from_this()](
                                   auto* opCtx) { _commitDropCollection(opCtx, executor, token); }))
        .then(
            _buildPhaseHandler(Phase::kReleaseCriticalSection,
                               [this, token, executor = executor, anchor = shared_from_this()](
                                   auto* opCtx) { _exitCriticalSection(opCtx, executor, token); }));
}

void DropCollectionCoordinator::_checkPreconditionsAndSaveArgumentsOnDoc() {
    auto opCtxHolder = makeOperationContext();
    auto* opCtx = opCtxHolder.get();

    // If the request had an expected UUID for the collection being dropped, we should verify that
    // it matches the one from the local catalog
    {
        AutoGetCollection coll{opCtx,
                               nss(),
                               MODE_IS,
                               AutoGetCollection::Options{}
                                   .viewMode(auto_get_collection::ViewMode::kViewsPermitted)
                                   .expectedUUID(_doc.getCollectionUUID())};

        // The drop operation is aborted if the namespace does not exist or does not comply with
        // naming restrictions. Non-system namespaces require additional logic that cannot be done
        // at this level, such as the time series collection must be resolved to remove the
        // corresponding bucket collection, or tag documents associated to non-existing collections
        // must be cleaned up.
        if (nss().isSystem()) {
            uassert(ErrorCodes::NamespaceNotFound,
                    fmt::format("namespace {} does not exist", nss().toStringForErrorMsg()),
                    *coll);

            uassertStatusOK(isDroppableCollection(opCtx, nss()));
        }
    }

    try {
        auto coll = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss());
        _doc.setCollInfo(std::move(coll));
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The collection is not sharded or doesn't exist.
        _doc.setCollInfo(boost::none);
    }
}

void DropCollectionCoordinator::_freezeMigrations(
    OperationContext* opCtx, std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    BSONObjBuilder logChangeDetail;
    if (_doc.getCollInfo()) {
        logChangeDetail.append("collectionUUID", _doc.getCollInfo()->getUuid().toBSON());
    }

    ShardingLogging::get(opCtx)->logChange(
        opCtx, "dropCollection.start", nss(), logChangeDetail.obj());

    if (_doc.getCollInfo()) {
        const auto collUUID = _doc.getCollInfo()->getUuid();
        const auto session = getNewSession(opCtx);
        sharding_ddl_util::stopMigrations(opCtx, nss(), collUUID, session);
    }
}

void DropCollectionCoordinator::_enterCriticalSection(
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    LOGV2_DEBUG(7038100, 2, "Acquiring critical section", logAttrs(nss()));

    ShardsvrParticipantBlock blockCRUDOperationsRequest(nss());
    blockCRUDOperationsRequest.setBlockType(mongo::CriticalSectionBlockTypeEnum::kReadsAndWrites);
    blockCRUDOperationsRequest.setReason(_critSecReason);

    generic_argument_util::setMajorityWriteConcern(blockCRUDOperationsRequest);
    generic_argument_util::setOperationSessionInfo(blockCRUDOperationsRequest,
                                                   getNewSession(opCtx));
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
        **executor, token, blockCRUDOperationsRequest);
    sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx, opts, Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx));

    LOGV2_DEBUG(7038101, 2, "Acquired critical section", logAttrs(nss()));
}

void DropCollectionCoordinator::_commitDropCollection(
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    const auto collIsSharded = bool(_doc.getCollInfo());

    // Define the identity of the shard that will be in charge of notifying change streams.
    // The value needs to be persisted once retrieved: tracked collections require a data-bearing
    // shard (prior to the DDL commit) as the notifier and this information may not be longer
    // available in the routing table in case of stepdown and retry of this coordinator.
    const auto changeStreamsNotifierShardId = [&]() {
        auto primaryShardId = ShardingState::get(opCtx)->shardId();

        // TODO SERVER-73741 remove the feature flag once 9.0 becomes last LTS.
        if (!feature_flags::gFeatureFlagChangeStreamPreciseShardTargeting.isEnabled(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
            !collIsSharded) {
            return primaryShardId;
        }

        if (_doc.getChangeStreamsNotifier()) {
            return _doc.getChangeStreamsNotifier().value();
        }

        auto dataBearingShard = sharding_ddl_util::pickShardOwningCollectionChunks(
            opCtx, _doc.getCollInfo()->getUuid());
        if (!dataBearingShard) {
            LOGV2_WARNING(10361100,
                          "Unable to retrieve the identity of a data bearing shard for the "
                          "collection beng dropped (possibly due to a metadata inconsistency)",
                          "nss"_attr = nss());
            dataBearingShard = primaryShardId;
        }

        auto newDoc = _doc;
        newDoc.setChangeStreamsNotifier(dataBearingShard);
        _updateStateDocument(opCtx, std::move(newDoc));

        return *dataBearingShard;
    }();

    LOGV2_DEBUG(5390504,
                2,
                "Dropping collection",
                logAttrs(nss()),
                "sharded"_attr = collIsSharded,
                "changeStreamsNotifierId"_attr = changeStreamsNotifierShardId);

    // The correctness of the commit sequence depends on the execution order of the following
    // steps:

    // 1. Deletion of the collection routing table (and other metadata) from the global catalog.
    {
        const auto session = getNewSession(opCtx);
        sharding_ddl_util::removeQueryAnalyzerMetadata(opCtx, nss(), session);
    }

    if (collIsSharded) {
        tassert(10644514,
                "Expected collInfo to be set on the coordinator document",
                _doc.getCollInfo());
        const auto coll = _doc.getCollInfo().value();

        sharding_ddl_util::removeCollAndChunksMetadataFromConfig(
            opCtx,
            Grid::get(opCtx)->shardRegistry()->getConfigShard(),
            Grid::get(opCtx)->catalogClient(),
            coll,
            defaultMajorityWriteConcernDoNotUse(),
            getNewSession(opCtx),
            **executor,
            false /*logCommitOnConfigPlacementHistory*/);
    }

    {
        // Remove zones associated to the collection (if any).
        const auto session = getNewSession(opCtx);
        sharding_ddl_util::removeTagsMetadataFromConfig(opCtx, nss(), session);
    }

    // Checkpoint the configTime to ensure that, in the case of a stepdown, the new primary will
    // start-up from a configTime that is inclusive of the metadata removable that was committed
    // during the critical section.
    VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);

    // 2. Drop collection data and local catalog metadata on each shard (including non-data
    //    bearing ones, to ensure that possible leftovers left behind by previous moveChunk/Primary
    //    operations get cleaned up).
    //    The change streams notifier will be the only one emitting a user-visible commit op-entry
    //    (fromMigrate = false).
    const auto sendParticipantCommand = [&](const std::vector<ShardId> recipients,
                                            bool fromMigrate) {
        const auto session = getNewSession(opCtx);
        sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
            opCtx, nss(), recipients, **executor, session, fromMigrate, false);
    };

    auto otherParticipants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    otherParticipants.erase(std::remove(otherParticipants.begin(),
                                        otherParticipants.end(),
                                        changeStreamsNotifierShardId),
                            otherParticipants.end());

    sendParticipantCommand(otherParticipants, true /*fromMigrateParam*/);
    sendParticipantCommand({changeStreamsNotifierShardId}, false /*fromMigrateParam*/);

    if (collIsSharded) {
        // 3. Insert the effects of the commit into config.placementHistory, if not already present.
        const auto commitTime = [&]() {
            const auto currentTime = VectorClock::get(opCtx)->getTime();
            return currentTime.clusterTime().asTimestamp();
        }();


        NamespacePlacementType placementInfo(nss(), commitTime, {});
        placementInfo.setUuid(_doc.getCollInfo()->getUuid());
        const auto session = getNewSession(opCtx);


        sharding_ddl_util::logDropCollectionCommitOnConfigPlacementHistory(
            opCtx, placementInfo, session, **executor);

        // 4. Notify change stream readers about the placement change caused by this commit.
        NamespacePlacementChanged notification(nss(), commitTime);
        auto buildNewSessionFn = [this](OperationContext* opCtx) {
            return getNewSession(opCtx);
        };
        sharding_ddl_util::generatePlacementChangeNotificationOnShard(
            opCtx, notification, changeStreamsNotifierShardId, buildNewSessionFn, executor, token);
    }

    ShardingLogging::get(opCtx)->logChange(opCtx, "dropCollection", nss());
    LOGV2(5390503, "Collection dropped", logAttrs(nss()));
}

void DropCollectionCoordinator::_exitCriticalSection(
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    LOGV2_DEBUG(7038102, 2, "Releasing critical section", logAttrs(nss()));

    ShardsvrParticipantBlock unblockCRUDOperationsRequest(nss());
    unblockCRUDOperationsRequest.setBlockType(CriticalSectionBlockTypeEnum::kUnblock);
    unblockCRUDOperationsRequest.setReason(_critSecReason);

    generic_argument_util::setMajorityWriteConcern(unblockCRUDOperationsRequest);
    generic_argument_util::setOperationSessionInfo(unblockCRUDOperationsRequest,
                                                   getNewSession(opCtx));
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
        **executor, token, unblockCRUDOperationsRequest);
    sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx, opts, Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx));

    LOGV2_DEBUG(7038103, 2, "Released critical section", logAttrs(nss()));
}

}  // namespace mongo
