/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/rename_collection_coordinator.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {
namespace {

boost::optional<CollectionType> getShardedCollection(OperationContext* opCtx,
                                                     const NamespaceString& nss) {
    try {
        return Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss);
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The collection is unsharded or doesn't exist
        return boost::none;
    }
}

}  // namespace

RenameCollectionCoordinator::RenameCollectionCoordinator(const BSONObj& initialState)
    : ShardingDDLCoordinator(initialState),
      _doc(RenameCollectionCoordinatorDocument::parse(
          IDLParserErrorContext("RenameCollectionCoordinatorDocument"), initialState)) {}

void RenameCollectionCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc = RenameCollectionCoordinatorDocument::parse(
        IDLParserErrorContext("RenameCollectionCoordinatorDocument"), doc);

    const auto& selfReq = _doc.getRenameCollectionRequest().toBSON();
    const auto& otherReq = otherDoc.getRenameCollectionRequest().toBSON();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another rename collection for namespace " << nss()
                          << "is being executed with different parameters: " << selfReq,
            SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
}

std::vector<DistLockManager::ScopedDistLock> RenameCollectionCoordinator::_acquireAdditionalLocks(
    OperationContext* opCtx) {
    const auto coorName = DDLCoordinatorType_serializer(_coorMetadata.getId().getOperationType());

    auto distLockManager = DistLockManager::get(opCtx);
    auto targetCollDistLock = uassertStatusOK(distLockManager->lock(
        opCtx, _doc.getTo().ns(), coorName, DistLockManager::kDefaultLockTimeout));

    std::vector<DistLockManager::ScopedDistLock> vec;
    vec.push_back(targetCollDistLock.moveToAnotherThread());
    return vec;
}

boost::optional<BSONObj> RenameCollectionCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    BSONObjBuilder cmdBob;
    if (const auto& optComment = getForwardableOpMetadata().getComment()) {
        cmdBob.append(optComment.get().firstElement());
    }
    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append("desc", "RenameCollectionCoordinator");
    bob.append("op", "command");
    bob.append("ns", nss().toString());
    bob.append("to", _doc.getTo().toString());
    bob.append("command", cmdBob.obj());
    bob.append("currentPhase", _doc.getPhase());
    bob.append("active", true);
    return bob.obj();
}

void RenameCollectionCoordinator::_insertStateDocument(StateDoc&& doc) {
    auto coorMetadata = doc.getShardingDDLCoordinatorMetadata();
    coorMetadata.setRecoveredFromDisk(true);
    doc.setShardingDDLCoordinatorMetadata(coorMetadata);

    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<StateDoc> store(NamespaceString::kShardingDDLCoordinatorsNamespace);
    store.add(opCtx.get(), doc, WriteConcerns::kMajorityWriteConcern);
    _doc = std::move(doc);
}

void RenameCollectionCoordinator::_updateStateDocument(StateDoc&& newDoc) {
    auto opCtx = cc().makeOperationContext();
    PersistentTaskStore<StateDoc> store(NamespaceString::kShardingDDLCoordinatorsNamespace);
    store.update(opCtx.get(),
                 BSON(StateDoc::kIdFieldName << _doc.getId().toBSON()),
                 newDoc.toBSON(),
                 WriteConcerns::kMajorityWriteConcern);

    _doc = std::move(newDoc);
}

void RenameCollectionCoordinator::_enterPhase(Phase newPhase) {
    StateDoc newDoc(_doc);
    newDoc.setPhase(newPhase);

    LOGV2_DEBUG(5460501,
                2,
                "Rename collection coordinator phase transition",
                "fromNs"_attr = nss(),
                "toNs"_attr = _doc.getTo(),
                "newPhase"_attr = RenameCollectionCoordinatorPhase_serializer(newDoc.getPhase()),
                "oldPhase"_attr = RenameCollectionCoordinatorPhase_serializer(_doc.getPhase()));

    if (_doc.getPhase() == Phase::kUnset) {
        _insertStateDocument(std::move(newDoc));
        return;
    }
    _updateStateDocument(std::move(newDoc));
}

ExecutorFuture<void> RenameCollectionCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_executePhase(
            Phase::kCheckPreconditionsAndFreezeMigrations,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                const auto& fromNss = nss();
                const auto& toNss = _doc.getTo();

                // Make sure the source collection exists
                const auto optSourceCollType = getShardedCollection(opCtx, nss());
                const bool sourceIsSharded = (bool)optSourceCollType;
                if (sourceIsSharded) {
                    uassert(ErrorCodes::CommandFailed,
                            str::stream() << "Source and destination collections must be on the "
                                             "same database because "
                                          << fromNss << " is sharded.",
                            fromNss.db() == toNss.db());
                } else {
                    Lock::DBLock dbLock(opCtx, fromNss.db(), MODE_IS);
                    Lock::CollectionLock collLock(opCtx, fromNss, MODE_IS);
                    const auto sourceCollPtr =
                        CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, fromNss);

                    uassert(ErrorCodes::NamespaceNotFound,
                            str::stream() << "Collection " << fromNss << " doesn't exist.",
                            sourceCollPtr);

                    if (fromNss.db() != toNss.db()) {
                        sharding_ddl_util::checkDbPrimariesOnTheSameShard(opCtx, fromNss, toNss);
                    }
                }

                _doc.setOptShardedCollInfo(optSourceCollType);

                // Make sure the target namespace is not a view
                {
                    Lock::DBLock dbLock(opCtx, toNss.db(), MODE_IS);
                    const auto db = DatabaseHolder::get(opCtx)->getDb(opCtx, toNss.db());
                    if (db) {
                        uassert(ErrorCodes::CommandNotSupportedOnView,
                                str::stream() << "Can't rename to target collection `" << toNss
                                              << "` because it is a view.",
                                !ViewCatalog::get(db)->lookup(opCtx, toNss.ns()));
                    }
                }

                const auto targetIsSharded = (bool)getShardedCollection(opCtx, toNss);
                _doc.setTargetIsSharded(targetIsSharded);

                sharding_ddl_util::checkShardedRenamePreconditions(
                    opCtx, toNss, _doc.getDropTarget());

                // Block migrations on involved sharded collections
                if (sourceIsSharded) {
                    sharding_ddl_util::stopMigrations(opCtx, nss());
                }

                if (targetIsSharded) {
                    sharding_ddl_util::stopMigrations(opCtx, _doc.getTo());
                }

                ShardingLogging::get(opCtx)->logChange(
                    opCtx,
                    "renameCollection.start",
                    nss().ns(),
                    BSON("source" << fromNss.toString() << "destination" << toNss.toString()),
                    ShardingCatalogClient::kMajorityWriteConcern);
            }))
        .then(_executePhase(
            Phase::kBlockCRUDAndRename,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                const auto& fromNss = nss();

                // On participant shards:
                // - Block CRUD on source and target collection in case at least one
                // of such collections is currently sharded.
                // - Locally drop the target collection
                // - Locally rename source to target
                ShardsvrRenameCollectionParticipant renameCollParticipantRequest(fromNss);
                renameCollParticipantRequest.setDbName(fromNss.db());
                renameCollParticipantRequest.setRenameCollectionRequest(
                    _doc.getRenameCollectionRequest());

                auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                // We need to send the command to all the shards because both movePrimary and
                // moveChunk leave garbage behind for sharded collections.
                sharding_ddl_util::sendAuthenticatedCommandToShards(
                    opCtx,
                    fromNss.db(),
                    CommandHelpers::appendMajorityWriteConcern(
                        renameCollParticipantRequest.toBSON({})),
                    participants,
                    **executor);
            }))
        .then(_executePhase(
            Phase::kRenameMetadata,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                const auto& optFromCollType = _doc.getOptShardedCollInfo();
                if (optFromCollType) {
                    // Rename CSRS metadata
                    auto collType = *optFromCollType;
                    sharding_ddl_util::shardedRenameMetadata(opCtx, collType, _doc.getTo());
                } else if (_doc.getTargetIsSharded()) {
                    // Remove stale target CSRS metadata
                    sharding_ddl_util::removeCollMetadataFromConfig(opCtx, _doc.getTo());
                }
            }))
        .then(_executePhase(
            Phase::kUnblockCRUD,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                const auto& fromNss = nss();

                // On participant shards:
                // - Unblock CRUD on participants for both source and destination collections
                ShardsvrRenameCollectionUnblockParticipant unblockParticipantRequest(fromNss);
                unblockParticipantRequest.setDbName(fromNss.db());
                unblockParticipantRequest.setTo(_doc.getTo());

                auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                sharding_ddl_util::sendAuthenticatedCommandToShards(
                    opCtx,
                    fromNss.db(),
                    CommandHelpers::appendMajorityWriteConcern(
                        unblockParticipantRequest.toBSON({})),
                    participants,
                    **executor);
            }))
        .then(_executePhase(
            Phase::kSetResponse,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                // Retrieve the new collection version
                const auto catalog = Grid::get(opCtx)->catalogCache();
                const auto cm = uassertStatusOK(
                    catalog->getCollectionRoutingInfoWithRefresh(opCtx, _doc.getTo()));
                _response = RenameCollectionResponse(cm.isSharded() ? cm.getVersion()
                                                                    : ChunkVersion::UNSHARDED());

                ShardingLogging::get(opCtx)->logChange(
                    opCtx,
                    "renameCollection.end",
                    nss().ns(),
                    BSON("source" << nss().toString() << "destination" << _doc.getTo().toString()),
                    ShardingCatalogClient::kMajorityWriteConcern);

                LOGV2(5460504, "Collection renamed", "namespace"_attr = nss());
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (!status.isA<ErrorCategory::NotPrimaryError>() &&
                !status.isA<ErrorCategory::ShutdownError>()) {
                LOGV2_ERROR(5460505,
                            "Error running rename collection",
                            "namespace"_attr = nss(),
                            "error"_attr = redact(status));
            }

            return status;
        });
}

}  // namespace mongo
