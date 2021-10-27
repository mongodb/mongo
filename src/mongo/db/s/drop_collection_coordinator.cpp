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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/db/s/drop_collection_coordinator.h"

#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {

DropCollectionCoordinator::DropCollectionCoordinator(ShardingDDLCoordinatorService* service,
                                                     const BSONObj& initialState)
    : ShardingDDLCoordinator(service, initialState),
      _doc(DropCollectionCoordinatorDocument::parse(
          IDLParserErrorContext("DropCollectionCoordinatorDocument"), initialState)) {}

boost::optional<BSONObj> DropCollectionCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    BSONObjBuilder cmdBob;
    if (const auto& optComment = getForwardableOpMetadata().getComment()) {
        cmdBob.append(optComment.get().firstElement());
    }
    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append("desc", "DropCollectionCoordinator");
    bob.append("op", "command");
    bob.append("ns", nss().toString());
    bob.append("command", cmdBob.obj());
    bob.append("currentPhase", _doc.getPhase());
    bob.append("active", true);
    return bob.obj();
}

DropReply DropCollectionCoordinator::dropCollectionLocally(OperationContext* opCtx,
                                                           const NamespaceString& nss) {
    {
        // Clear CollectionShardingRuntime entry
        Lock::DBLock dbLock(opCtx, nss.db(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        auto* csr = CollectionShardingRuntime::get(opCtx, nss);
        csr->clearFilteringMetadata(opCtx);
    }

    DropReply result;
    uassertStatusOK(dropCollection(
        opCtx, nss, &result, DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));

    // Force the refresh of the catalog cache to purge outdated information
    const auto catalog = Grid::get(opCtx)->catalogCache();
    uassertStatusOK(catalog->getCollectionRoutingInfoWithRefresh(opCtx, nss));
    CatalogCacheLoader::get(opCtx).waitForCollectionFlush(opCtx, nss);
    repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);

    return result;
}

void DropCollectionCoordinator::_enterPhase(Phase newPhase) {
    StateDoc newDoc(_doc);
    newDoc.setPhase(newPhase);

    LOGV2_DEBUG(5390501,
                2,
                "Drop collection coordinator phase transition",
                "namespace"_attr = nss(),
                "newPhase"_attr = DropCollectionCoordinatorPhase_serializer(newDoc.getPhase()),
                "oldPhase"_attr = DropCollectionCoordinatorPhase_serializer(_doc.getPhase()));

    if (_doc.getPhase() == Phase::kUnset) {
        _doc = _insertStateDocument(std::move(newDoc));
        return;
    }
    _doc = _updateStateDocument(cc().makeOperationContext().get(), std::move(newDoc));
}

void DropCollectionCoordinator::_performNoopRetryableWriteOnParticipants(
    OperationContext* opCtx, const std::shared_ptr<executor::TaskExecutor>& executor) {
    auto shardsAndConfigsvr = [&] {
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        auto participants = shardRegistry->getAllShardIds(opCtx);
        participants.emplace_back(shardRegistry->getConfigShard()->getId());
        return participants;
    }();

    _doc = _updateSession(opCtx, _doc);
    sharding_ddl_util::performNoopRetryableWriteOnShards(
        opCtx, shardsAndConfigsvr, getCurrentSession(_doc), executor);
}

ExecutorFuture<void> DropCollectionCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_executePhase(
            Phase::kFreezeCollection,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                try {
                    auto coll = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss());
                    _doc.setCollInfo(std::move(coll));
                } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                    // The collection is not sharded or doesn't exist.
                    _doc.setCollInfo(boost::none);
                }

                BSONObjBuilder logChangeDetail;
                if (_doc.getCollInfo()) {
                    logChangeDetail.append("collectionUUID",
                                           _doc.getCollInfo()->getUuid().toBSON());
                }

                ShardingLogging::get(opCtx)->logChange(
                    opCtx, "dropCollection.start", nss().ns(), logChangeDetail.obj());

                // Persist the collection info before sticking to using it's uuid. This ensures this
                // node is still the RS primary, so it was also the primary at the moment we read
                // the collection metadata.
                _doc = _updateStateDocument(opCtx, StateDoc(_doc));

                if (_doc.getCollInfo()) {
                    sharding_ddl_util::stopMigrations(opCtx, nss(), _doc.getCollInfo()->getUuid());
                }
            }))
        .then(_executePhase(
            Phase::kDropCollection,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                if (!_firstExecution) {
                    // Perform a noop write on the participants in order to advance the txnNumber
                    // for this coordinator's lsid so that requests with older txnNumbers can no
                    // longer execute.
                    _performNoopRetryableWriteOnParticipants(opCtx, **executor);
                }

                const auto collIsSharded = bool(_doc.getCollInfo());

                LOGV2_DEBUG(5390504,
                            2,
                            "Dropping collection",
                            "namespace"_attr = nss(),
                            "sharded"_attr = collIsSharded);

                if (collIsSharded) {
                    invariant(_doc.getCollInfo());
                    const auto& coll = _doc.getCollInfo().get();
                    sharding_ddl_util::removeCollAndChunksMetadataFromConfig(
                        opCtx, coll, ShardingCatalogClient::kMajorityWriteConcern);
                }

                // Remove tags even if the collection is not sharded or didn't exist
                _doc = _updateSession(opCtx, _doc);
                sharding_ddl_util::removeTagsMetadataFromConfig(
                    opCtx, nss(), getCurrentSession(_doc));

                // get a Lsid and an incremented txnNumber. Ensures we are the primary
                _doc = _updateSession(opCtx, _doc);

                const auto primaryShardId = ShardingState::get(opCtx)->shardId();

                // We need to send the drop to all the shards because both movePrimary and
                // moveChunk leave garbage behind for sharded collections.
                auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                // Remove primary shard from participants
                participants.erase(
                    std::remove(participants.begin(), participants.end(), primaryShardId),
                    participants.end());

                sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
                    opCtx, nss(), participants, **executor, getCurrentSession(_doc));

                // The sharded collection must be dropped on the primary shard after it has been
                // dropped on all of the other shards to ensure it can only be re-created as
                // unsharded with a higher optime than all of the drops.
                sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
                    opCtx, nss(), {primaryShardId}, **executor, getCurrentSession(_doc));

                ShardingLogging::get(opCtx)->logChange(opCtx, "dropCollection", nss().ns());
                LOGV2(5390503, "Collection dropped", "namespace"_attr = nss());
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (!status.isA<ErrorCategory::NotPrimaryError>() &&
                !status.isA<ErrorCategory::ShutdownError>()) {
                LOGV2_ERROR(5280901,
                            "Error running drop collection",
                            "namespace"_attr = nss(),
                            "error"_attr = redact(status));
            }
            return status;
        });
}

}  // namespace mongo
