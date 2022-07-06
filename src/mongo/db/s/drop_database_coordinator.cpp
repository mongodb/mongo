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


#include "mongo/db/s/drop_database_coordinator.h"

#include "mongo/db/api_parameters.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/type_shard_database.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/flush_database_cache_updates_gen.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

void removeDatabaseMetadataFromConfig(OperationContext* opCtx,
                                      StringData dbName,
                                      const DatabaseVersion& dbVersion) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    ON_BLOCK_EXIT([&, dbName = dbName.toString()] {
        Grid::get(opCtx)->catalogCache()->purgeDatabase(dbName);
    });

    // Remove the database entry from the metadata. Making the dbVersion uuid part of the query
    // ensures idempotency.
    const Status status = catalogClient->removeConfigDocuments(
        opCtx,
        NamespaceString::kConfigDatabasesNamespace,
        BSON(DatabaseType::kNameFieldName
             << dbName.toString()
             << DatabaseType::kVersionFieldName + "." + DatabaseVersion::kUuidFieldName
             << dbVersion.getUuid()),
        ShardingCatalogClient::kMajorityWriteConcern);
    uassertStatusOKWithContext(status,
                               str::stream()
                                   << "Could not remove database metadata from config server for '"
                                   << dbName << "'.");
}

class ScopedDatabaseCriticalSection {
public:
    ScopedDatabaseCriticalSection(OperationContext* opCtx,
                                  const std::string dbName,
                                  const BSONObj reason)
        : _opCtx(opCtx), _dbName(std::move(dbName)), _reason(std::move(reason)) {
        // TODO SERVER-67438 Once ScopedDatabaseCriticalSection holds a DatabaseName obj, use dbName
        // directly
        DatabaseName databaseName(boost::none, _dbName);
        Lock::DBLock dbLock(_opCtx, databaseName, MODE_X);
        auto dss = DatabaseShardingState::get(_opCtx, _dbName);
        auto dssLock = DatabaseShardingState::DSSLock::lockExclusive(_opCtx, dss);
        dss->enterCriticalSectionCatchUpPhase(_opCtx, dssLock, _reason);
        dss->enterCriticalSectionCommitPhase(_opCtx, dssLock, _reason);
    }

    ~ScopedDatabaseCriticalSection() {
        UninterruptibleLockGuard guard(_opCtx->lockState());
        // TODO SERVER-67438 Once ScopedDatabaseCriticalSection holds a DatabaseName obj, use dbName
        // directly
        DatabaseName databaseName(boost::none, _dbName);
        Lock::DBLock dbLock(_opCtx, databaseName, MODE_X);
        auto dss = DatabaseShardingState::get(_opCtx, _dbName);
        dss->exitCriticalSection(_opCtx, _reason);
    }

private:
    OperationContext* _opCtx;
    const std::string _dbName;
    const BSONObj _reason;
};


}  // namespace

void DropDatabaseCoordinator::_dropShardedCollection(
    OperationContext* opCtx,
    const CollectionType& coll,
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    const auto& nss = coll.getNss();

    // Acquire the collection distributed lock in order to synchronize with an eventual ongoing
    // moveChunk and to prevent new ones from happening.
    const auto coorName = DDLCoordinatorType_serializer(_coordId.getOperationType());
    auto collDistLock = uassertStatusOK(DistLockManager::get(opCtx)->lock(
        opCtx, nss.ns(), coorName, DistLockManager::kDefaultLockTimeout));

    sharding_ddl_util::removeCollAndChunksMetadataFromConfig(
        opCtx, coll, ShardingCatalogClient::kMajorityWriteConcern);

    _updateSession(opCtx);
    sharding_ddl_util::removeTagsMetadataFromConfig(opCtx, nss, getCurrentSession());

    const auto primaryShardId = ShardingState::get(opCtx)->shardId();
    _updateSession(opCtx);

    // We need to send the drop to all the shards because both movePrimary and
    // moveChunk leave garbage behind for sharded collections.
    auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    // Remove primary shard from participants
    participants.erase(std::remove(participants.begin(), participants.end(), primaryShardId),
                       participants.end());
    sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
        opCtx, nss, participants, **executor, getCurrentSession());

    // The sharded collection must be dropped on the primary shard after it has been dropped on all
    // of the other shards to ensure it can only be re-created as unsharded with a higher optime
    // than all of the drops.
    sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
        opCtx, nss, {primaryShardId}, **executor, getCurrentSession());
}

void DropDatabaseCoordinator::_clearDatabaseInfoOnSecondaries(OperationContext* opCtx) {
    Status signalStatus = shardmetadatautil::updateShardDatabasesEntry(
        opCtx,
        BSON(ShardDatabaseType::kNameFieldName << _dbName),
        BSONObj(),
        BSON(ShardDatabaseType::kEnterCriticalSectionCounterFieldName << 1),
        false /*upsert*/);
    uassert(ErrorCodes::OperationFailed,
            str::stream() << "Failed to persist critical section signal for "
                             "secondaries due to: "
                          << signalStatus.toString(),
            signalStatus.isOK());
    // Wait for majority write concern on the secondaries
    WriteConcernResult ignoreResult;
    auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(
        opCtx, latestOpTime, ShardingCatalogClient::kMajorityWriteConcern, &ignoreResult));
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

                if (!_firstExecution) {
                    // Perform a noop write on the participants in order to advance the txnNumber
                    // for this coordinator's lsid so that requests with older txnNumbers can no
                    // longer execute.
                    _updateSession(opCtx);
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                        opCtx, getCurrentSession(), **executor);
                }

                ShardingLogging::get(opCtx)->logChange(opCtx, "dropDatabase.start", _dbName);

                // Drop all collections under this DB
                auto const catalogClient = Grid::get(opCtx)->catalogClient();
                const auto allCollectionsForDb = catalogClient->getCollections(
                    opCtx, _dbName, repl::ReadConcernLevel::kMajorityReadConcern);

                // Make sure we were primary when we read the collections metadata so it is safe
                // to proceed using the collection uuids to perform destructive operations
                sharding_ddl_util::performNoopMajorityWriteLocally(opCtx);

                // ensure we do not delete collections of a different DB
                if (!_firstExecution && _doc.getDatabaseVersion()) {
                    try {
                        const auto db = catalogClient->getDatabase(
                            opCtx, _dbName, repl::ReadConcernLevel::kMajorityReadConcern);
                        if (_doc.getDatabaseVersion()->getUuid() != db.getVersion().getUuid()) {
                            return;  // skip to FlushDatabaseCacheUpdates
                        }
                    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                        return;  // skip to FlushDatabaseCacheUpdates
                    }
                }

                if (_doc.getCollInfo()) {
                    const auto& coll = _doc.getCollInfo().get();
                    LOGV2_DEBUG(5494504,
                                2,
                                "Completing collection drop from previous primary",
                                "namespace"_attr = coll.getNss());
                    _dropShardedCollection(opCtx, coll, executor);
                }

                for (const auto& coll : allCollectionsForDb) {
                    const auto& nss = coll.getNss();
                    LOGV2_DEBUG(5494505, 2, "Dropping collection", "namespace"_attr = nss);

                    sharding_ddl_util::stopMigrations(opCtx, nss, coll.getUuid());

                    auto newStateDoc = _doc;
                    newStateDoc.setCollInfo(coll);
                    _updateStateDocument(opCtx, std::move(newStateDoc));

                    _dropShardedCollection(opCtx, coll, executor);
                }

                const auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                {
                    // Acquire the database critical section in order to disallow implicit
                    // collection creations from happening concurrently with dropDatabase
                    const auto critSecReason = BSON("dropDatabase" << _dbName);
                    auto scopedCritSec = ScopedDatabaseCriticalSection(
                        opCtx, _dbName.toString(), std::move(critSecReason));

                    auto dropDatabaseParticipantCmd = ShardsvrDropDatabaseParticipant();
                    dropDatabaseParticipantCmd.setDbName(_dbName);
                    const auto cmdObj = CommandHelpers::appendMajorityWriteConcern(
                        dropDatabaseParticipantCmd.toBSON({}));

                    // The database needs to be dropped first on the db primary shard
                    // because otherwise changestreams won't receive the drop event.
                    {
                        DBDirectClient dbDirectClient(opCtx);
                        const auto commandResponse =
                            dbDirectClient.runCommand(OpMsgRequest::fromDBAndBody(_dbName, cmdObj));
                        uassertStatusOK(
                            getStatusFromCommandResult(commandResponse->getCommandReply()));

                        WriteConcernResult ignoreResult;
                        const auto latestOpTime =
                            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
                        uassertStatusOK(
                            waitForWriteConcern(opCtx,
                                                latestOpTime,
                                                ShardingCatalogClient::kMajorityWriteConcern,
                                                &ignoreResult));
                    }

                    // Remove primary shard from participants
                    const auto primaryShardId = ShardingState::get(opCtx)->shardId();
                    auto participants = allShardIds;
                    participants.erase(
                        std::remove(participants.begin(), participants.end(), primaryShardId),
                        participants.end());
                    // Drop DB on all other shards, attaching the dbVersion to the request to ensure
                    // idempotency.
                    try {
                        sharding_ddl_util::sendAuthenticatedCommandToShards(
                            opCtx,
                            _dbName,
                            appendDbVersionIfPresent(cmdObj, *metadata().getDatabaseVersion()),
                            participants,
                            **executor);
                    } catch (ExceptionFor<ErrorCodes::StaleDbVersion>&) {
                        // The DB metadata could have been removed by a network-partitioned former
                        // primary
                    }

                    // Clear the database sharding state info before exiting the critical section so
                    // that all subsequent write operations with the old database version will fail
                    // due to StaleDbVersion.
                    _clearDatabaseInfoOnSecondaries(opCtx);

                    removeDatabaseMetadataFromConfig(
                        opCtx, _dbName, *metadata().getDatabaseVersion());
                }
            }))
        .then([this, executor = executor, anchor = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);
            {
                const auto primaryShardId = ShardingState::get(opCtx)->shardId();
                auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                participants.erase(
                    std::remove(participants.begin(), participants.end(), primaryShardId),
                    participants.end());
                // Send _flushDatabaseCacheUpdates to all shards
                FlushDatabaseCacheUpdatesWithWriteConcern flushDbCacheUpdatesCmd(
                    _dbName.toString());
                flushDbCacheUpdatesCmd.setSyncFromConfig(true);
                flushDbCacheUpdatesCmd.setDbName(_dbName);

                IgnoreAPIParametersBlock ignoreApiParametersBlock{opCtx};
                sharding_ddl_util::sendAuthenticatedCommandToShards(
                    opCtx,
                    "admin",
                    CommandHelpers::appendMajorityWriteConcern(flushDbCacheUpdatesCmd.toBSON({})),
                    participants,
                    **executor);
            }

            ShardingLogging::get(opCtx)->logChange(opCtx, "dropDatabase", _dbName);
            LOGV2(5494506, "Database dropped", "db"_attr = _dbName);
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            if (!status.isA<ErrorCategory::NotPrimaryError>() &&
                !status.isA<ErrorCategory::ShutdownError>()) {
                LOGV2_ERROR(5494507,
                            "Error running drop database",
                            "db"_attr = _dbName,
                            "error"_attr = redact(status));
            }
            return status;
        });
}

}  // namespace mongo
