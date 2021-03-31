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

#include "mongo/db/s/drop_database_coordinator.h"

#include "mongo/db/api_parameters.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {
namespace {

void dropShardedCollection(OperationContext* opCtx,
                           const CollectionType& coll,
                           std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    sharding_ddl_util::removeCollMetadataFromConfig(opCtx, coll);

    const auto primaryShardId = ShardingState::get(opCtx)->shardId();
    const ShardsvrDropCollectionParticipant dropCollectionParticipant(coll.getNss());
    const auto cmdObj =
        CommandHelpers::appendMajorityWriteConcern(dropCollectionParticipant.toBSON({}));

    // The collection needs to be dropped first on the db primary shard
    // because otherwise changestreams won't receive the drop event.
    sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx, coll.getNss().db(), cmdObj, {primaryShardId}, **executor);

    // We need to send the drop to all the shards because both movePrimary and
    // moveChunk leave garbage behind for sharded collections.
    auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    // Remove prumary shard from participants
    participants.erase(std::remove(participants.begin(), participants.end(), primaryShardId),
                       participants.end());
    sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx, coll.getNss().db(), cmdObj, participants, **executor);
}

void removeDatabaseMetadataFromConfig(OperationContext* opCtx, StringData dbName) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    ON_BLOCK_EXIT([&, dbName = dbName.toString()] {
        Grid::get(opCtx)->catalogCache()->purgeDatabase(dbName);
    });

    // Remove the database entry from the metadata.
    const Status status =
        catalogClient->removeConfigDocuments(opCtx,
                                             DatabaseType::ConfigNS,
                                             BSON(DatabaseType::name(dbName.toString())),
                                             ShardingCatalogClient::kMajorityWriteConcern);
    uassertStatusOKWithContext(status,
                               str::stream()
                                   << "Could not remove database metadata from config server for '"
                                   << dbName << "'.");
}

}  // namespace

DropDatabaseCoordinator::DropDatabaseCoordinator(const BSONObj& initialState)
    : ShardingDDLCoordinator(initialState),
      _doc(DropDatabaseCoordinatorDocument::parse(
          IDLParserErrorContext("DropDatabaseCoordinatorDocument"), initialState)),
      _dbName(nss().db()) {}

boost::optional<BSONObj> DropDatabaseCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    BSONObjBuilder cmdBob;
    if (const auto& optComment = getForwardableOpMetadata().getComment()) {
        cmdBob.append(optComment.get().firstElement());
    }
    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append("desc", "DropDatabaseCoordinator");
    bob.append("op", "command");
    bob.append("ns", nss().toString());
    bob.append("command", cmdBob.obj());
    bob.append("currentPhase", _doc.getPhase());
    bob.append("active", true);
    return bob.obj();
}

void DropDatabaseCoordinator::_insertStateDocument(OperationContext* opCtx, StateDoc&& doc) {
    auto coorMetadata = doc.getShardingDDLCoordinatorMetadata();
    coorMetadata.setRecoveredFromDisk(true);
    doc.setShardingDDLCoordinatorMetadata(coorMetadata);

    PersistentTaskStore<StateDoc> store(NamespaceString::kShardingDDLCoordinatorsNamespace);
    store.add(opCtx, doc, WriteConcerns::kMajorityWriteConcern);
    _doc = std::move(doc);
}

void DropDatabaseCoordinator::_updateStateDocument(OperationContext* opCtx, StateDoc&& newDoc) {
    PersistentTaskStore<StateDoc> store(NamespaceString::kShardingDDLCoordinatorsNamespace);
    store.update(opCtx,
                 BSON(StateDoc::kIdFieldName << _doc.getId().toBSON()),
                 newDoc.toBSON(),
                 WriteConcerns::kMajorityWriteConcern);

    _doc = std::move(newDoc);
}

void DropDatabaseCoordinator::_enterPhase(Phase newPhase) {
    StateDoc newDoc(_doc);
    newDoc.setPhase(newPhase);

    LOGV2_DEBUG(5494501,
                2,
                "Drop database coordinator phase transition",
                "namespace"_attr = nss(),
                "newPhase"_attr = DropDatabaseCoordinatorPhase_serializer(newDoc.getPhase()),
                "oldPhase"_attr = DropDatabaseCoordinatorPhase_serializer(_doc.getPhase()));

    auto opCtx = cc().makeOperationContext();
    if (_doc.getPhase() == Phase::kUnset) {
        _insertStateDocument(opCtx.get(), std::move(newDoc));
        return;
    }
    _updateStateDocument(opCtx.get(), std::move(newDoc));
}

ExecutorFuture<void> DropDatabaseCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_executePhase(
            Phase::kDrop,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                if (_doc.getCollInfo()) {
                    const auto& coll = _doc.getCollInfo().get();
                    LOGV2_DEBUG(5494504,
                                2,
                                "Completing collection drop from previous primary",
                                "namespace"_attr = coll.getNss());
                    dropShardedCollection(opCtx, coll, executor);
                }

                ShardingLogging::get(opCtx)->logChange(opCtx, "dropDatabase.start", _dbName);

                // Drop all collections under this DB
                auto const catalogClient = Grid::get(opCtx)->catalogClient();
                const auto allCollectionsForDb = catalogClient->getCollections(
                    opCtx, _dbName, repl::ReadConcernLevel::kMajorityReadConcern);

                for (const auto& coll : allCollectionsForDb) {
                    const auto& nss = coll.getNss();
                    LOGV2_DEBUG(5494505, 2, "Dropping collection", "namespace"_attr = nss);

                    sharding_ddl_util::stopMigrations(opCtx, nss);

                    auto newStateDoc = _doc;
                    newStateDoc.setCollInfo(coll);
                    _updateStateDocument(opCtx, std::move(newStateDoc));

                    dropShardedCollection(opCtx, coll, executor);
                }

                const auto primaryShardId = ShardingState::get(opCtx)->shardId();
                auto dropDatabaseParticipantCmd = ShardsvrDropDatabaseParticipant();
                dropDatabaseParticipantCmd.setDbName(_dbName);
                const auto cmdObj = CommandHelpers::appendMajorityWriteConcern(
                    dropDatabaseParticipantCmd.toBSON({}));

                // The database needs to be dropped first on the db primary shard
                // because otherwise changestreams won't receive the drop event.
                sharding_ddl_util::sendAuthenticatedCommandToShards(
                    opCtx, _dbName, cmdObj, {primaryShardId}, **executor);

                const auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                // Remove prumary shard from participants
                auto participants = allShardIds;
                participants.erase(
                    std::remove(participants.begin(), participants.end(), primaryShardId),
                    participants.end());
                // Drop DB on all other shards
                sharding_ddl_util::sendAuthenticatedCommandToShards(
                    opCtx, _dbName, cmdObj, participants, **executor);

                removeDatabaseMetadataFromConfig(opCtx, _dbName);

                {
                    // Send _flushDatabaseCacheUpdates to all shards
                    IgnoreAPIParametersBlock ignoreApiParametersBlock{opCtx};
                    sharding_ddl_util::sendAuthenticatedCommandToShards(
                        opCtx,
                        "admin",
                        BSON("_flushDatabaseCacheUpdates" << _dbName),
                        allShardIds,
                        **executor);
                }

                ShardingLogging::get(opCtx)->logChange(opCtx, "dropDatabase", _dbName);
                LOGV2(5494506, "Database dropped", "namespace"_attr = nss());
            }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (!status.isA<ErrorCategory::NotPrimaryError>() &&
                !status.isA<ErrorCategory::ShutdownError>()) {
                LOGV2_ERROR(5494507,
                            "Error running drop database",
                            "namespace"_attr = nss(),
                            "error"_attr = redact(status));
            }
            return status;
        });
}

}  // namespace mongo
