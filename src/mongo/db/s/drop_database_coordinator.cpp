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

#include <absl/container/node_hash_map.h>
#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/ddl_lock_manager.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/participant_block_gen.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_ddl_coordinator.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/db/s/type_shard_database.h"
#include "mongo/db/s/type_shard_database_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/async_rpc_util.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/database_version.h"
#include "mongo/s/database_version_gen.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/flush_database_cache_updates_gen.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharding_state.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/pcre_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

void removeDatabaseFromConfigAndUpdatePlacementHistory(
    OperationContext* opCtx,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    const DatabaseName& dbName,
    const DatabaseVersion& dbVersion,
    const OperationSessionInfo& osi) {

    // Run the remove database command on the config server and placemetHistory update in a
    // multistatement transaction

    // Ensure that this function will only return once the transaction gets majority committed (and
    // restore the original write concern on exit).
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);

    /*
     * The transactionChain callback may be run on a separate thread. For this reason, all the
     * referenced parameters have to be captured by value (shared_ptrs are used to reduce the memory
     * footprint).
     */
    const auto transactionChain = [opCtx, dbName, dbVersion](
                                      const txn_api::TransactionClient& txnClient,
                                      ExecutorPtr txnExec) {
        // Making the dbVersion timestamp part of the query ensures idempotency.
        write_ops::DeleteOpEntry deleteDatabaseEntryOp{
            BSON(DatabaseType::kDbNameFieldName
                 << DatabaseNameUtil::serialize(dbName, SerializationContext::stateCommandRequest())
                 << DatabaseType::kVersionFieldName + "." + DatabaseVersion::kTimestampFieldName
                 << dbVersion.getTimestamp()),
            false};

        write_ops::DeleteCommandRequest deleteDatabaseEntry(
            NamespaceString::kConfigDatabasesNamespace, {deleteDatabaseEntryOp});

        return txnClient.runCRUDOp(deleteDatabaseEntry, {0})
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& deleteDatabaseEntryResponse) {
                uassertStatusOKWithContext(
                    deleteDatabaseEntryResponse.toStatus(),
                    str::stream() << "Could not remove database metadata from config server for '"
                                  << dbName.toStringForErrorMsg() << "'.");

                const auto currentTime = VectorClock::get(opCtx)->getTime();
                const auto currentTimestamp = currentTime.clusterTime().asTimestamp();
                NamespacePlacementType placementInfo(NamespaceString(dbName), currentTimestamp, {});

                write_ops::InsertCommandRequest insertPlacementEntry(
                    NamespaceString::kConfigsvrPlacementHistoryNamespace, {placementInfo.toBSON()});
                return txnClient.runCRUDOp(insertPlacementEntry, {1});
            })
            .thenRunOn(txnExec)
            .then([](const BatchedCommandResponse& insertPlacementEntryResponse) {
                uassertStatusOK(insertPlacementEntryResponse.toStatus());
            })
            .semi();
    };

    auto wc = WriteConcernOptions{WriteConcernOptions::kMajority,
                                  WriteConcernOptions::SyncMode::UNSET,
                                  WriteConcernOptions::kNoTimeout};
    // This always runs in the shard role so should use a cluster transaction to guarantee targeting
    // the config server.
    bool useClusterTransaction = true;
    sharding_ddl_util::runTransactionOnShardingCatalog(
        opCtx, std::move(transactionChain), wc, osi, useClusterTransaction, executor);
}

bool isDbAlreadyDropped(OperationContext* opCtx,
                        const boost::optional<mongo::DatabaseVersion>& dbVersion,
                        const DatabaseName& dbName) {
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
                << "nss"
                << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
}

}  // namespace

void DropDatabaseCoordinator::_dropShardedCollection(
    OperationContext* opCtx,
    const CollectionType& coll,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    const auto& nss = coll.getNss();

    {
        ShardsvrParticipantBlock blockCRUDOperationsRequest(nss);
        blockCRUDOperationsRequest.setBlockType(
            mongo::CriticalSectionBlockTypeEnum::kReadsAndWrites);
        blockCRUDOperationsRequest.setReason(getReasonForDropCollection(nss));
        async_rpc::GenericArgs args;
        async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
        async_rpc::AsyncRPCCommandHelpers::appendOSI(args, getNewSession(opCtx));
        auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
            **executor, token, blockCRUDOperationsRequest, args);
        sharding_ddl_util::sendAuthenticatedCommandToShards(
            opCtx, opts, Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx));
    }

    // This always runs in the shard role so should use a cluster transaction to guarantee
    // targeting the config server.
    bool useClusterTransaction = true;
    sharding_ddl_util::removeCollAndChunksMetadataFromConfig(
        opCtx,
        Grid::get(opCtx)->shardRegistry()->getConfigShard(),
        Grid::get(opCtx)->catalogClient(),
        coll,
        ShardingCatalogClient::kMajorityWriteConcern,
        getNewSession(opCtx),
        useClusterTransaction,
        **executor);

    sharding_ddl_util::removeTagsMetadataFromConfig(opCtx, nss, getNewSession(opCtx));

    const auto primaryShardId = ShardingState::get(opCtx)->shardId();

    // We need to send the drop to all the shards because both movePrimary and
    // moveChunk leave garbage behind for sharded collections.
    auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    // Remove primary shard from participants
    participants.erase(std::remove(participants.begin(), participants.end(), primaryShardId),
                       participants.end());
    sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
        opCtx,
        nss,
        participants,
        **executor,
        getNewSession(opCtx),
        true /* fromMigrate */,
        false /* dropSystemCollections */);

    // The sharded collection must be dropped on the primary shard after it has been dropped on all
    // of the other shards to ensure it can only be re-created as unsharded with a higher optime
    // than all of the drops.
    sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
        opCtx,
        nss,
        {primaryShardId},
        **executor,
        getNewSession(opCtx),
        false /* fromMigrate */,
        false /* dropSystemCollections */);

    {
        ShardsvrParticipantBlock unblockCRUDOperationsRequest(nss);
        unblockCRUDOperationsRequest.setBlockType(CriticalSectionBlockTypeEnum::kUnblock);
        unblockCRUDOperationsRequest.setReason(getReasonForDropCollection(nss));
        async_rpc::GenericArgs args;
        async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
        async_rpc::AsyncRPCCommandHelpers::appendOSI(args, getNewSession(opCtx));
        auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
            **executor, token, unblockCRUDOperationsRequest, args);
        sharding_ddl_util::sendAuthenticatedCommandToShards(
            opCtx, opts, Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx));
    }
}

void DropDatabaseCoordinator::_clearDatabaseInfoOnPrimary(OperationContext* opCtx) {
    AutoGetDb autoDb(opCtx, _dbName, MODE_X);
    auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx, _dbName);
    scopedDss->clearDbInfo(opCtx);
}

void DropDatabaseCoordinator::_clearDatabaseInfoOnSecondaries(OperationContext* opCtx) {
    Status signalStatus = shardmetadatautil::updateShardDatabasesEntry(
        opCtx,
        BSON(ShardDatabaseType::kDbNameFieldName
             << DatabaseNameUtil::serialize(_dbName, SerializationContext::stateCommandRequest())),
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
    const auto dbNss = NamespaceString(_dbName);
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kDrop,
            [this, token, dbNss, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                if (!_firstExecution) {
                    // Perform a noop write on the participants in order to advance the txnNumber
                    // for this coordinator's lsid so that requests with older txnNumbers can no
                    // longer execute.
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                        opCtx, getNewSession(opCtx), **executor);
                }

                ShardingLogging::get(opCtx)->logChange(opCtx, "dropDatabase.start", dbNss);

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
                    VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);
                    return;  // skip to FlushDatabaseCacheUpdates
                }

                if (_doc.getCollInfo()) {
                    const auto coll = _doc.getCollInfo().value();
                    LOGV2_DEBUG(5494504,
                                2,
                                "Completing collection drop from previous primary",
                                logAttrs(coll.getNss()));
                    _dropShardedCollection(opCtx, coll, executor, token);
                }

                for (const auto& coll : allCollectionsForDb) {
                    const auto& nss = coll.getNss();
                    LOGV2_DEBUG(5494505, 2, "Dropping collection", logAttrs(nss));

                    sharding_ddl_util::stopMigrations(
                        opCtx, nss, coll.getUuid(), getNewSession(opCtx));

                    auto newStateDoc = _doc;
                    newStateDoc.setCollInfo(coll);
                    _updateStateDocument(opCtx, std::move(newStateDoc));

                    _dropShardedCollection(opCtx, coll, executor, token);
                }

                // First of all, we will get all namespaces that still have zones associated to
                // database _dbName from 'config.tags'. As we already have removed all zones
                // associated with each sharded collections from database _dbName, the returned
                // zones are owned by unsharded or nonexistent collections. After that, we will
                // removed these remaining zones.
                const auto& nssWithZones =
                    catalogClient->getAllNssThatHaveZonesForDatabase(opCtx, _dbName);
                for (const auto& nss : nssWithZones) {
                    sharding_ddl_util::removeTagsMetadataFromConfig(
                        opCtx, nss, getNewSession(opCtx));
                }

                // Remove the query sampling configuration documents for all collections in this
                // database, if they exist.
                auto db = DatabaseNameUtil::serialize(_dbName,
                                                      SerializationContext::stateCommandRequest());
                const std::string regex = "^" + pcre_util::quoteMeta(db) + "\\..*";
                sharding_ddl_util::removeQueryAnalyzerMetadataFromConfig(
                    opCtx,
                    BSON(analyze_shard_key::QueryAnalyzerDocument::kNsFieldName
                         << BSON("$regex" << regex)));

                const auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                {
                    // Acquire the database critical section in order to disallow implicit
                    // collection creations from happening concurrently with dropDatabase
                    auto recoveryService = ShardingRecoveryService::get(opCtx);
                    recoveryService->acquireRecoverableCriticalSectionBlockWrites(
                        opCtx, dbNss, _critSecReason, ShardingCatalogClient::kLocalWriteConcern);
                    recoveryService->promoteRecoverableCriticalSectionToBlockAlsoReads(
                        opCtx, dbNss, _critSecReason, ShardingCatalogClient::kLocalWriteConcern);

                    auto dropDatabaseParticipantCmd = ShardsvrDropDatabaseParticipant();
                    dropDatabaseParticipantCmd.setDbName(_dbName);
                    const auto cmdObj = CommandHelpers::appendMajorityWriteConcern(
                        dropDatabaseParticipantCmd.toBSON({}));

                    // The database needs to be dropped first on the db primary shard
                    // because otherwise changestreams won't receive the drop event.
                    {
                        DBDirectClient dbDirectClient(opCtx);
                        const auto commandResponse =
                            dbDirectClient.runCommand(OpMsgRequestBuilder::create(
                                auth::ValidatedTenancyScope::get(opCtx), _dbName, cmdObj));
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

                    async_rpc::GenericArgs args;
                    async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
                    async_rpc::AsyncRPCCommandHelpers::appendDbVersionIfPresent(
                        args, *metadata().getDatabaseVersion());
                    // Drop DB on all other shards, attaching the dbVersion to the request to ensure
                    // idempotency.
                    auto opts = std::make_shared<
                        async_rpc::AsyncRPCOptions<ShardsvrDropDatabaseParticipant>>(
                        **executor, token, dropDatabaseParticipantCmd, args);
                    try {
                        sharding_ddl_util::sendAuthenticatedCommandToShards(
                            opCtx, opts, participants);
                    } catch (ExceptionFor<ErrorCodes::StaleDbVersion>&) {
                        // The DB metadata could have been removed by a network-partitioned former
                        // primary
                    }

                    // Clear the database sharding state info before exiting the critical section so
                    // that all subsequent write operations with the old database version will fail
                    // due to StaleDbVersion.
                    _clearDatabaseInfoOnPrimary(opCtx);
                    _clearDatabaseInfoOnSecondaries(opCtx);

                    const auto& osi = getNewSession(opCtx);
                    removeDatabaseFromConfigAndUpdatePlacementHistory(
                        opCtx, **executor, _dbName, *metadata().getDatabaseVersion(), osi);

                    VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);
                }
            }))
        .then([this, executor = executor, dbNss, anchor = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);
            ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
                opCtx,
                dbNss,
                _critSecReason,
                WriteConcerns::kMajorityWriteConcernNoTimeout,
                /* throwIfReasonDiffers */ false);
        })
        .then([this, token, dbNss, executor = executor, anchor = shared_from_this()] {
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
                const auto db = DatabaseNameUtil::serialize(
                    _dbName, SerializationContext::stateCommandRequest());
                FlushDatabaseCacheUpdatesWithWriteConcern flushDbCacheUpdatesCmd(db);
                flushDbCacheUpdatesCmd.setSyncFromConfig(true);
                flushDbCacheUpdatesCmd.setDbName(DatabaseName::kAdmin);
                async_rpc::GenericArgs args;
                async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);

                IgnoreAPIParametersBlock ignoreApiParametersBlock{opCtx};
                auto opts = std::make_shared<
                    async_rpc::AsyncRPCOptions<FlushDatabaseCacheUpdatesWithWriteConcern>>(
                    **executor, token, flushDbCacheUpdatesCmd, args);
                sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, participants);
            }

            ShardingLogging::get(opCtx)->logChange(opCtx, "dropDatabase", dbNss);
            LOGV2(5494506, "Database dropped", "db"_attr = _dbName);
        });
}

}  // namespace mongo
