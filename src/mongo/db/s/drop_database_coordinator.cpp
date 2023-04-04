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
#include "mongo/db/catalog_raii.h"
#include "mongo/db/cluster_transaction_api.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/participant_block_gen.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/type_shard_database.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/flush_database_cache_updates_gen.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

void removeDatabaseFromConfigAndUpdatePlacementHistory(
    OperationContext* opCtx,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    StringData& dbName,
    const DatabaseVersion& dbVersion) {

    // Run the remove database command on the config server and placemetHistory update in a
    // multistatement transaction

    // Ensure that this function will only return once the transaction gets majority committed (and
    // restore the original write concern on exit).
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);

    WriteConcernOptions originalWC = opCtx->getWriteConcern();
    opCtx->setWriteConcern(WriteConcernOptions{WriteConcernOptions::kMajority,
                                               WriteConcernOptions::SyncMode::UNSET,
                                               WriteConcernOptions::kNoTimeout});

    ScopeGuard guard([&, dbName = dbName.toString()] {
        opCtx->setWriteConcern(originalWC);
        Grid::get(opCtx)->catalogCache()->purgeDatabase(dbName);
    });

    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(executor);

    auto txnClient = std::make_unique<txn_api::details::SEPTransactionClient>(
        opCtx,
        inlineExecutor,
        sleepInlineExecutor,
        std::make_unique<txn_api::details::ClusterSEPTransactionClientBehaviors>(
            opCtx->getServiceContext()));
    /*
     * The transactionChain callback may be run on a separate thread. For this reason, all the
     * referenced parameters have to be captured by value (shared_ptrs are used to reduce the memory
     * footprint).
     */
    const auto transactionChain = [opCtx, dbName = dbName.toString(), dbVersion](
                                      const txn_api::TransactionClient& txnClient,
                                      ExecutorPtr txnExec) {
        // Making the dbVersion timestamp part of the query ensures idempotency.
        write_ops::DeleteOpEntry deleteDatabaseEntryOp{
            BSON(DatabaseType::kNameFieldName
                 << dbName
                 << DatabaseType::kVersionFieldName + "." + DatabaseVersion::kTimestampFieldName
                 << dbVersion.getTimestamp()),
            false};

        write_ops::DeleteCommandRequest deleteDatabaseEntry(
            NamespaceString::kConfigDatabasesNamespace, {deleteDatabaseEntryOp});

        return txnClient.runCRUDOp(deleteDatabaseEntry, {})
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& deleteDatabaseEntryResponse) {
                uassertStatusOKWithContext(
                    deleteDatabaseEntryResponse.toStatus(),
                    str::stream() << "Could not remove database metadata from config server for '"
                                  << dbName << "'.");

                // pre-check to guarantee idempotence: in case of a retry, the placement history
                // entry may already exist
                if (deleteDatabaseEntryResponse.getN() == 0) {
                    BatchedCommandResponse noOp;
                    noOp.setN(0);
                    noOp.setStatus(Status::OK());
                    return SemiFuture<BatchedCommandResponse>(std::move(noOp));
                }

                const auto currentTime = VectorClock::get(opCtx)->getTime();
                const auto currentTimestamp = currentTime.clusterTime().asTimestamp();

                NamespacePlacementType placementInfo(NamespaceString(dbName), currentTimestamp, {});

                write_ops::InsertCommandRequest insertPlacementEntry(
                    NamespaceString::kConfigsvrPlacementHistoryNamespace, {placementInfo.toBSON()});
                return txnClient.runCRUDOp(insertPlacementEntry, {});
            })
            .thenRunOn(txnExec)
            .then([](const BatchedCommandResponse& insertPlacementEntryResponse) {
                uassertStatusOK(insertPlacementEntryResponse.toStatus());
            })
            .semi();
    };

    txn_api::SyncTransactionWithRetries txn(opCtx,
                                            sleepInlineExecutor,
                                            nullptr /*resourceYielder*/,
                                            inlineExecutor,
                                            std::move(txnClient));
    txn.run(opCtx, transactionChain);
}

// TODO SERVER-73627: Remove once 7.0 becomes last LTS
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
        auto scopedDss =
            DatabaseShardingState::assertDbLockedAndAcquireExclusive(_opCtx, databaseName);
        scopedDss->enterCriticalSectionCatchUpPhase(_opCtx, _reason);
        scopedDss->enterCriticalSectionCommitPhase(_opCtx, _reason);
    }

    ~ScopedDatabaseCriticalSection() {
        // TODO (SERVER-71444): Fix to be interruptible or document exception.
        UninterruptibleLockGuard guard(_opCtx->lockState());  // NOLINT.
        // TODO SERVER-67438 Once ScopedDatabaseCriticalSection holds a DatabaseName obj, use dbName
        // directly
        DatabaseName databaseName(boost::none, _dbName);
        Lock::DBLock dbLock(_opCtx, databaseName, MODE_X);
        auto scopedDss =
            DatabaseShardingState::assertDbLockedAndAcquireExclusive(_opCtx, databaseName);
        scopedDss->exitCriticalSection(_opCtx, _reason);
    }

    ScopedDatabaseCriticalSection(const ScopedDatabaseCriticalSection&) = delete;
    ScopedDatabaseCriticalSection(ScopedDatabaseCriticalSection&&) = delete;

private:
    OperationContext* _opCtx;
    const std::string _dbName;
    const BSONObj _reason;
};

bool isDbAlreadyDropped(OperationContext* opCtx,
                        const boost::optional<mongo::DatabaseVersion>& dbVersion,
                        const StringData& dbName) {
    if (dbVersion) {
        try {
            auto const catalogClient = Grid::get(opCtx)->catalogClient();
            const auto db = catalogClient->getDatabase(
                opCtx, dbName, repl::ReadConcernLevel::kMajorityReadConcern);
            if (dbVersion->getUuid() != db.getVersion().getUuid()) {
                // The database was dropped and re-created with a different UUID
                return true;
            }
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            // The database was already dropped
            return true;
        }
    }
    return false;
}

BSONObj getReasonForDropCollection(const NamespaceString& nss) {
    return BSON("command"
                << "dropCollection fromDropDatabase"
                << "nss" << nss.ns());
}

}  // namespace

void DropDatabaseCoordinator::_dropShardedCollection(
    OperationContext* opCtx,
    const CollectionType& coll,
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    const auto& nss = coll.getNss();

    // Acquire the collection distributed lock in order to synchronize with an eventual ongoing
    // moveChunk and to prevent new ones from happening.
    const auto coorName = DDLCoordinatorType_serializer(_coordId.getOperationType());
    auto collDDLLock = DDLLockManager::get(opCtx)->lock(
        opCtx, nss.ns(), coorName, DDLLockManager::kDefaultLockTimeout);

    if (!_isPre70Compatible()) {
        _updateSession(opCtx);
        ShardsvrParticipantBlock blockCRUDOperationsRequest(nss);
        blockCRUDOperationsRequest.setBlockType(
            mongo::CriticalSectionBlockTypeEnum::kReadsAndWrites);
        blockCRUDOperationsRequest.setReason(getReasonForDropCollection(nss));
        blockCRUDOperationsRequest.setAllowViews(true);
        const auto cmdObj =
            CommandHelpers::appendMajorityWriteConcern(blockCRUDOperationsRequest.toBSON({}));
        sharding_ddl_util::sendAuthenticatedCommandToShards(
            opCtx,
            nss.db(),
            cmdObj.addFields(getCurrentSession().toBSON()),
            Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx),
            **executor);
    }

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
        opCtx, nss, participants, **executor, getCurrentSession(), true /* fromMigrate */);

    // The sharded collection must be dropped on the primary shard after it has been dropped on all
    // of the other shards to ensure it can only be re-created as unsharded with a higher optime
    // than all of the drops.
    sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
        opCtx, nss, {primaryShardId}, **executor, getCurrentSession(), false /* fromMigrate */);

    // Remove collection's query analyzer configuration document, if it exists.
    sharding_ddl_util::removeQueryAnalyzerMetadataFromConfig(opCtx, nss, coll.getUuid());

    if (!_isPre70Compatible()) {
        _updateSession(opCtx);
        ShardsvrParticipantBlock unblockCRUDOperationsRequest(nss);
        unblockCRUDOperationsRequest.setBlockType(CriticalSectionBlockTypeEnum::kUnblock);
        unblockCRUDOperationsRequest.setReason(getReasonForDropCollection(nss));
        unblockCRUDOperationsRequest.setAllowViews(true);

        const auto cmdObj =
            CommandHelpers::appendMajorityWriteConcern(unblockCRUDOperationsRequest.toBSON({}));
        sharding_ddl_util::sendAuthenticatedCommandToShards(
            opCtx,
            nss.db(),
            cmdObj.addFields(getCurrentSession().toBSON()),
            Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx),
            **executor);
    }
}

void DropDatabaseCoordinator::_clearDatabaseInfoOnPrimary(OperationContext* opCtx) {
    DatabaseName dbName(boost::none, _dbName);
    AutoGetDb autoDb(opCtx, dbName, MODE_X);
    auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx, dbName);
    scopedDss->clearDbInfo(opCtx);
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
        .then(_buildPhaseHandler(
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
                if (!_firstExecution &&
                    isDbAlreadyDropped(opCtx, _doc.getDatabaseVersion(), _dbName)) {
                    if (_isPre70Compatible()) {
                        // Clear the database sharding state so that all subsequent write operations
                        // with the old database version will fail due to StaleDbVersion.
                        // Note: because we are using an scoped critical section it could happen
                        // that the dbversion being deleted is recovered once we return. It is a
                        // rare occurence, but it might lead to a situation where the now former
                        // primary will believe to still be primary.
                        _clearDatabaseInfoOnPrimary(opCtx);
                        _clearDatabaseInfoOnSecondaries(opCtx);
                    }
                    VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);
                    return;  // skip to FlushDatabaseCacheUpdates
                }

                if (_doc.getCollInfo()) {
                    const auto& coll = _doc.getCollInfo().value();
                    LOGV2_DEBUG(5494504,
                                2,
                                "Completing collection drop from previous primary",
                                logAttrs(coll.getNss()));
                    _dropShardedCollection(opCtx, coll, executor);
                }

                for (const auto& coll : allCollectionsForDb) {
                    const auto& nss = coll.getNss();
                    LOGV2_DEBUG(5494505, 2, "Dropping collection", logAttrs(nss));

                    sharding_ddl_util::stopMigrations(opCtx, nss, coll.getUuid());

                    auto newStateDoc = _doc;
                    newStateDoc.setCollInfo(coll);
                    _updateStateDocument(opCtx, std::move(newStateDoc));

                    _dropShardedCollection(opCtx, coll, executor);
                }

                // First of all, we will get all namespaces that still have zones associated to
                // database _dbName from 'config.tags'. As we already have removed all zones
                // associated with each sharded collections from database _dbName, the returned
                // zones are owned by unsharded or nonexistent collections. After that, we will
                // removed these remaining zones.
                const auto& nssWithZones =
                    catalogClient->getAllNssThatHaveZonesForDatabase(opCtx, _dbName);
                for (const auto& nss : nssWithZones) {
                    _updateSession(opCtx);
                    sharding_ddl_util::removeTagsMetadataFromConfig(
                        opCtx, nss, getCurrentSession());
                }

                const auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                {
                    // Acquire the database critical section in order to disallow implicit
                    // collection creations from happening concurrently with dropDatabase
                    boost::optional<ScopedDatabaseCriticalSection> scopedCritSec;
                    // Only use the recoverable critical section in new versions.
                    if (!_isPre70Compatible()) {
                        auto recoveryService = ShardingRecoveryService::get(opCtx);
                        recoveryService->acquireRecoverableCriticalSectionBlockWrites(
                            opCtx,
                            NamespaceString(_dbName),
                            _critSecReason,
                            ShardingCatalogClient::kLocalWriteConcern);
                        recoveryService->promoteRecoverableCriticalSectionToBlockAlsoReads(
                            opCtx,
                            NamespaceString(_dbName),
                            _critSecReason,
                            ShardingCatalogClient::kLocalWriteConcern);
                    } else {
                        scopedCritSec.emplace(opCtx, _dbName.toString(), _critSecReason);
                    }

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
                    _clearDatabaseInfoOnPrimary(opCtx);
                    _clearDatabaseInfoOnSecondaries(opCtx);

                    removeDatabaseFromConfigAndUpdatePlacementHistory(
                        opCtx, **executor, _dbName, *metadata().getDatabaseVersion());

                    VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);
                }
            }))
        .then([this, executor = executor, anchor = shared_from_this()] {
            if (!_isPre70Compatible()) {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);
                ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
                    opCtx,
                    NamespaceString(_dbName),
                    _critSecReason,
                    WriteConcerns::kMajorityWriteConcernNoTimeout,
                    /* throwIfReasonDiffers */ false);
            }
        })
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
