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


#include "mongo/db/global_catalog/ddl/drop_database_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/notify_sharding_event_gen.h"
#include "mongo/db/global_catalog/ddl/shard_metadata_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/global_catalog/ddl/shardsvr_commit_create_database_metadata_command.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/local_catalog/ddl/list_collections_filter.h"
#include "mongo/db/local_catalog/shard_role_catalog/flush_database_cache_updates_gen.h"
#include "mongo/db/local_catalog/shard_role_catalog/participant_block_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(dropDatabaseCoordinatorPauseAfterConfigCommit);

BSONObj makeDatabaseQuery(const DatabaseName& dbName, const DatabaseVersion& dbVersion) {
    // Making the dbVersion timestamp part of the query ensures idempotency.
    return BSON(DatabaseType::kDbNameFieldName
                << DatabaseNameUtil::serialize(dbName, SerializationContext::stateCommandRequest())
                << DatabaseType::kVersionFieldName + "." + DatabaseVersion::kTimestampFieldName
                << dbVersion.getTimestamp());
}

void removeDatabaseMetadataFromShard(OperationContext* opCtx,
                                     const DatabaseName& dbName,
                                     const OperationSessionInfo& osi,
                                     const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                                     const CancellationToken& token) {
    const auto thisShardId = ShardingState::get(opCtx)->shardId();
    sharding_ddl_util::commitDropDatabaseMetadataToShardCatalog(
        opCtx, dbName, thisShardId, osi, executor, token);
}

/**
 * Fetches database metadata from the global catalog and installs it in the shard catalog. This
 * operation is necessary when the FCV is transitioning to 9.0 to prevent potential races with
 * _shardsvrCloneAuthoritativeMetadata during the upgrade phase.
 *
 * TODO (SERVER-98118): Remove this method once v9.0 become last-lts.
 */
void cloneAuthoritativeDatabaseMetadata(OperationContext* opCtx,
                                        const DatabaseName& dbName,
                                        const BSONObj& critSecReason) {
    auto recoveryService = ShardingRecoveryService::get(opCtx);
    recoveryService->acquireRecoverableCriticalSectionBlockWrites(
        opCtx,
        NamespaceString(dbName),
        critSecReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
        false /* clearDbMetadata */);
    recoveryService->promoteRecoverableCriticalSectionToBlockAlsoReads(
        opCtx,
        NamespaceString(dbName),
        critSecReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto dbMetadata =
        catalogClient->getDatabase(opCtx, dbName, repl::ReadConcernLevel::kMajorityReadConcern);

    const auto thisShardId = ShardingState::get(opCtx)->shardId();

    tassert(10162500,
            fmt::format("Expecting to have fetched database metadata from a database which "
                        "this shard owns. DatabaseName: {}. Database primary shard: {}. "
                        "This shard: {}",
                        dbName.toStringForErrorMsg(),
                        dbMetadata.getPrimary().toString(),
                        thisShardId.toString()),
            thisShardId == dbMetadata.getPrimary());

    commitCreateDatabaseMetadataLocally(opCtx, dbMetadata);

    recoveryService->releaseRecoverableCriticalSection(
        opCtx,
        NamespaceString(dbName),
        critSecReason,
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
        ShardingRecoveryService::NoCustomAction(),
        false /* throwIfReasonDiffers */);
}

/**
 * Runs the transaction to persist the drop of the requested database name on the global catalog,
 * returning the cluster time at which the operation was logged on config.placementHistory.
 */
Timestamp commitDropDatabaseOnGlobalCatalog(
    OperationContext* opCtx,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    const DatabaseName& dbName,
    const DatabaseVersion& dbVersion,
    const std::function<OperationSessionInfo(OperationContext*)> buildNewSessionFn) {

    // Run the remove database command on the config server and placementHistory update in a
    // multistatement transaction

    // Ensure that this function will only return once the transaction gets majority committed (and
    // restore the original write concern on exit).
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);

    const auto commitTime = [&] {
        const auto currentTime = VectorClock::get(opCtx)->getTime();
        return currentTime.clusterTime().asTimestamp();
    }();

    const auto transactionChain = [&](const txn_api::TransactionClient& txnClient,
                                      ExecutorPtr txnExec) {
        write_ops::DeleteOpEntry deleteDatabaseEntryOp{makeDatabaseQuery(dbName, dbVersion), false};

        write_ops::DeleteCommandRequest deleteDatabaseEntry(
            NamespaceString::kConfigDatabasesNamespace, {deleteDatabaseEntryOp});

        return txnClient.runCRUDOp(deleteDatabaseEntry, {0})
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& deleteDatabaseEntryResponse) {
                uassertStatusOKWithContext(
                    deleteDatabaseEntryResponse.toStatus(),
                    str::stream() << "Could not remove database metadata from config server for '"
                                  << dbName.toStringForErrorMsg() << "'.");

                NamespacePlacementType placementInfo(NamespaceString(dbName), commitTime, {});

                write_ops::InsertCommandRequest insertPlacementEntry(
                    NamespaceString::kConfigsvrPlacementHistoryNamespace, {placementInfo.toBSON()});
                return txnClient.runCRUDOp(insertPlacementEntry, {1});
            })
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& insertPlacementEntryResponse) {
                uassertStatusOK(insertPlacementEntryResponse.toStatus());

                // Upserts a document {_id: <dbName>, version: {timestamp: <timestamp>}} to
                // 'config.dropPendingDBs' collection on the config server. This blocks
                // createDatabase coordinator from committing the creation of database with the same
                // name to the sharding catalog.
                // We are upserting instead of inserting because, in multiversion, there could be
                // garbage left behind by old binaries. The garbage is removed in FCV upgrade, but
                // before that, the coordinator must be resilient to it.
                // TODO (SERVER-94362): turn this back to an insert command.
                write_ops::UpdateCommandRequest updateConfigDropPendingDBsEntry(
                    NamespaceString::kConfigDropPendingDBsNamespace);
                updateConfigDropPendingDBsEntry.setUpdates({[&] {
                    write_ops::UpdateOpEntry entry;
                    entry.setQ(BSON(DatabaseType::kDbNameFieldName << DatabaseNameUtil::serialize(
                                        dbName, SerializationContext::stateCommandRequest())));
                    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(BSON(
                        DatabaseType::kVersionFieldName << BSON(DatabaseVersion::kTimestampFieldName
                                                                << dbVersion.getTimestamp()))));
                    entry.setUpsert(true);
                    return entry;
                }()});
                return txnClient.runCRUDOp(updateConfigDropPendingDBsEntry, {2});
            })
            .thenRunOn(txnExec)
            .then([](const BatchedCommandResponse& insertConfigDropPendingDBsEntryResponse) {
                uassertStatusOK(insertConfigDropPendingDBsEntryResponse.toStatus());
            })
            .semi();
    };

    auto wc = WriteConcernOptions{WriteConcernOptions::kMajority,
                                  WriteConcernOptions::SyncMode::UNSET,
                                  WriteConcernOptions::kNoTimeout};

    const auto& osi = buildNewSessionFn(opCtx);
    sharding_ddl_util::runTransactionOnShardingCatalog(
        opCtx, std::move(transactionChain), wc, osi, executor);

    return commitTime;
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
    return BSON(
        "command" << "dropCollection fromDropDatabase"
                  << "nss"
                  << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
}

}  // namespace

void DropDatabaseCoordinator::_dropTrackedCollection(
    OperationContext* opCtx,
    const CollectionType& coll,
    const ShardId& changeStreamsNotifierShardId,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    const auto& nss = coll.getNss();

    {
        ShardsvrParticipantBlock blockCRUDOperationsRequest(nss);
        blockCRUDOperationsRequest.setBlockType(
            mongo::CriticalSectionBlockTypeEnum::kReadsAndWrites);
        blockCRUDOperationsRequest.setReason(getReasonForDropCollection(nss));
        generic_argument_util::setMajorityWriteConcern(blockCRUDOperationsRequest);
        generic_argument_util::setOperationSessionInfo(blockCRUDOperationsRequest,
                                                       getNewSession(opCtx));
        auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
            **executor, token, blockCRUDOperationsRequest);
        sharding_ddl_util::sendAuthenticatedCommandToShards(
            opCtx, opts, Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx));
    }

    sharding_ddl_util::removeCollAndChunksMetadataFromConfig(
        opCtx,
        Grid::get(opCtx)->shardRegistry()->getConfigShard(),
        Grid::get(opCtx)->catalogClient(),
        coll,
        defaultMajorityWriteConcernDoNotUse(),
        getNewSession(opCtx),
        **executor,
        false /*logCommitOnConfigPlacementHistory*/);

    {
        const auto session = getNewSession(opCtx);
        sharding_ddl_util::removeTagsMetadataFromConfig(opCtx, nss, session);
    }

    // Remove the query sampling configuration documents for the collection, if it exists.
    sharding_ddl_util::removeQueryAnalyzerMetadata(opCtx, {coll.getUuid()});

    const auto primaryShardId = ShardingState::get(opCtx)->shardId();

    // We need to send the drop to all the shards because both movePrimary and
    // moveChunk leave garbage behind for sharded collections.
    auto locallyDropCollectionOnParticipants = [&](const std::vector<ShardId>& shards,
                                                   bool fromMigrate) {
        sharding_ddl_util::sendDropCollectionParticipantCommandToShards(
            opCtx,
            nss,
            shards,
            **executor,
            getNewSession(opCtx),
            fromMigrate,
            false /* dropSystemCollections */);
    };
    auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    // Remove primary shard from participants
    participants.erase(std::remove_if(participants.begin(),
                                      participants.end(),
                                      [&](const auto& shard) {
                                          return shard == primaryShardId ||
                                              shard == changeStreamsNotifierShardId;
                                      }),
                       participants.end());
    locallyDropCollectionOnParticipants(participants, true /*fromMigrate*/);

    // The sharded collection must be dropped on the primary shard after it has been dropped on all
    // of the other shards to ensure it can only be re-created as unsharded with a higher optime
    // than all of the drops.
    locallyDropCollectionOnParticipants({changeStreamsNotifierShardId}, false /*fromMigrate*/);

    if (primaryShardId != changeStreamsNotifierShardId) {
        locallyDropCollectionOnParticipants({primaryShardId}, true /*fromMigrate*/);
    }

    const auto commitTime = [&]() {
        const auto currentTime = VectorClock::get(opCtx)->getTime();
        return currentTime.clusterTime().asTimestamp();
    }();

    NamespacePlacementType placementInfo(nss, commitTime, {});
    placementInfo.setUuid(_doc.getCollInfo()->getUuid());
    const auto session = getNewSession(opCtx);

    sharding_ddl_util::logDropCollectionCommitOnConfigPlacementHistory(
        opCtx, placementInfo, session, **executor);

    // Generate the namespacePlacementChanged op entry to support the post-commit activity of
    // change stream readers.
    NamespacePlacementChanged notification(nss, commitTime);
    auto buildNewSessionFn = [this](OperationContext* opCtx) {
        return getNewSession(opCtx);
    };
    sharding_ddl_util::generatePlacementChangeNotificationOnShard(
        opCtx, notification, changeStreamsNotifierShardId, buildNewSessionFn, executor, token);

    {
        ShardsvrParticipantBlock unblockCRUDOperationsRequest(nss);
        unblockCRUDOperationsRequest.setBlockType(CriticalSectionBlockTypeEnum::kUnblock);
        unblockCRUDOperationsRequest.setReason(getReasonForDropCollection(nss));
        generic_argument_util::setMajorityWriteConcern(unblockCRUDOperationsRequest);
        generic_argument_util::setOperationSessionInfo(unblockCRUDOperationsRequest,
                                                       getNewSession(opCtx));
        auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
            **executor, token, unblockCRUDOperationsRequest);
        sharding_ddl_util::sendAuthenticatedCommandToShards(
            opCtx, opts, Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx));
    }
}

ExecutorFuture<void> DropDatabaseCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    const auto dbNss = NamespaceString(_dbName);
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kDrop,
            [this, token, dbNss, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                if (!_firstExecution) {
                    // Perform a noop write on the participants in order to advance the txnNumber
                    // for this coordinator's lsid so that requests with older txnNumbers can no
                    // longer execute.
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                        opCtx, getNewSession(opCtx), **executor);
                }

                ShardingLogging::get(opCtx)->logChange(opCtx, "dropDatabase.start", dbNss);
                const auto primaryShardId = ShardingState::get(opCtx)->shardId();

                // Make sure we were primary when we read the collections metadata so it is safe
                // to proceed using the collection uuids to perform destructive operations
                sharding_ddl_util::performNoopMajorityWriteLocally(opCtx);

                // ensure we do not delete collections of a different DB
                if (!_firstExecution &&
                    isDbAlreadyDropped(opCtx, _doc.getDatabaseVersion(), _dbName)) {
                    VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);
                    return;  // skip to FlushDatabaseCacheUpdates
                }

                auto const catalogClient = Grid::get(opCtx)->catalogClient();

                if (_doc.getAuthoritativeMetadataAccessLevel() ==
                    AuthoritativeMetadataAccessLevelEnum::kWritesAllowed) {
                    cloneAuthoritativeDatabaseMetadata(opCtx, _dbName, _critSecReason);
                }

                // Drop all collections under this DB
                const auto allTrackedCollectionsForDb = catalogClient->getCollections(
                    opCtx, _dbName, repl::ReadConcernLevel::kMajorityReadConcern);

                // Check if the operation was previously interrupted in the middle of a sharded
                // collection drop; if so, resume the step.
                if (_doc.getCollInfo()) {
                    const auto coll = _doc.getCollInfo().value();
                    const auto& collChangeStreamsNotifierShardId =
                        _doc.getCollChangeStreamsNotifier().value_or(primaryShardId);
                    LOGV2_DEBUG(5494504,
                                2,
                                "Completing drop of tracked collection from previous primary",
                                logAttrs(coll.getNss()),
                                "CollChangeStreamsNotifierId"_attr =
                                    collChangeStreamsNotifierShardId);
                    _dropTrackedCollection(
                        opCtx, coll, collChangeStreamsNotifierShardId, executor, token);
                }

                for (const auto& coll : allTrackedCollectionsForDb) {
                    const auto& nss = coll.getNss();

                    // Ensure that no chunk migration may target the next collection to be dropped
                    // before persisting its identity on the recovery doc (This condition won't be
                    // reinforced in case of a stepdown)
                    sharding_ddl_util::stopMigrations(
                        opCtx, nss, coll.getUuid(), getNewSession(opCtx));

                    auto newStateDoc = _doc;
                    newStateDoc.setCollInfo(coll);
                    newStateDoc.setCollChangeStreamsNotifier([&]() {
                        auto changeStreamsNotifier = primaryShardId;
                        if (feature_flags::gFeatureFlagChangeStreamPreciseShardTargeting.isEnabled(
                                VersionContext::getDecoration(opCtx),
                                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                            auto dataBearingShardId =
                                sharding_ddl_util::pickShardOwningCollectionChunks(opCtx,
                                                                                   coll.getUuid());
                            if (dataBearingShardId) {
                                changeStreamsNotifier = *dataBearingShardId;
                            } else {
                                LOGV2_WARNING(10488700,
                                              "Unable to retrieve the identity of a data bearing "
                                              "shard for the "
                                              "collection beng dropped (possibly due to a metadata "
                                              "inconsistency)",
                                              "nss"_attr = nss);
                            }
                        }

                        return changeStreamsNotifier;
                    }());
                    _updateStateDocument(opCtx, std::move(newStateDoc));

                    const auto& changeStreamsNotifier = *_doc.getCollChangeStreamsNotifier();
                    LOGV2_DEBUG(5494505,
                                2,
                                "Dropping tracked collection",
                                logAttrs(nss),
                                "collChangeStreamsNotifierId"_attr = changeStreamsNotifier);

                    _dropTrackedCollection(opCtx, coll, changeStreamsNotifier, executor, token);
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

                // Remove the query sampling configuration documents for all unsharded collections
                // in this database, if they exist.
                {
                    std::vector<UUID> unshardedCollUUIDs;
                    DBDirectClient dbClient(opCtx);
                    // NOTE: UUIDs are retrieved through a filter that excludes timeseries
                    // collections; this is fine, since no query analyzer can be configured on them.
                    const auto collInfos = dbClient.getCollectionInfos(
                        _dbName, ListCollectionsFilter::makeTypeCollectionFilter());
                    for (const auto& collInfo : collInfos) {
                        unshardedCollUUIDs.push_back(UUID::parse(collInfo.getObjectField("info")));
                    }

                    sharding_ddl_util::removeQueryAnalyzerMetadata(opCtx,
                                                                   std::move(unshardedCollUUIDs));
                }

                const auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                {
                    // Acquire the database critical section in order to disallow implicit
                    // collection creations from happening concurrently with dropDatabase
                    const bool clearDbMetadata = _doc.getAuthoritativeMetadataAccessLevel() ==
                        AuthoritativeMetadataAccessLevelEnum::kNone;
                    auto recoveryService = ShardingRecoveryService::get(opCtx);
                    recoveryService->acquireRecoverableCriticalSectionBlockWrites(
                        opCtx,
                        dbNss,
                        _critSecReason,
                        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
                        clearDbMetadata);
                    recoveryService->promoteRecoverableCriticalSectionToBlockAlsoReads(
                        opCtx,
                        dbNss,
                        _critSecReason,
                        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

                    auto dropDatabaseParticipantCmd = ShardsvrDropDatabaseParticipant();
                    dropDatabaseParticipantCmd.setDbName(_dbName);
                    generic_argument_util::setMajorityWriteConcern(dropDatabaseParticipantCmd);

                    // Perform the database drop on the local catalog of the primary shard.
                    {
                        dropDatabaseParticipantCmd.setFromMigrate(false);

                        DBDirectClient dbDirectClient(opCtx);
                        const auto commandResponse = dbDirectClient.runCommand(
                            OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::get(opCtx),
                                                        _dbName,
                                                        dropDatabaseParticipantCmd.toBSON()));
                        uassertStatusOK(
                            getStatusFromCommandResult(commandResponse->getCommandReply()));

                        WriteConcernResult ignoreResult;
                        const auto latestOpTime =
                            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
                        uassertStatusOK(waitForWriteConcern(opCtx,
                                                            latestOpTime,
                                                            defaultMajorityWriteConcernDoNotUse(),
                                                            &ignoreResult));
                    }

                    // Perform the database drop on the local catalog of any other shard;
                    {
                        dropDatabaseParticipantCmd.setFromMigrate(true);

                        // Remove primary shard from participants
                        auto participants = allShardIds;
                        participants.erase(
                            std::remove(participants.begin(), participants.end(), primaryShardId),
                            participants.end());

                        generic_argument_util::setMajorityWriteConcern(dropDatabaseParticipantCmd);

                        // Send participant commands with replay protection to ensure idempotency
                        // and resilience against network partitioning. The model used to achieve
                        // replay protection depends on whether the shards are database metadata
                        // authoritative.
                        // - If the shards are not database metadata authoritative, we attach the
                        //   dbVersion to the request and ignore StaleDbVersion exceptions.
                        // - If the shards are database metadata authoritative, we use the OSI
                        // protocol.
                        if (_doc.getAuthoritativeMetadataAccessLevel() >=
                            AuthoritativeMetadataAccessLevelEnum::kWritesAllowed) {
                            const auto session = getNewSession(opCtx);
                            generic_argument_util::setOperationSessionInfo(
                                dropDatabaseParticipantCmd, session);

                            auto opts = std::make_shared<
                                async_rpc::AsyncRPCOptions<ShardsvrDropDatabaseParticipant>>(
                                **executor, token, dropDatabaseParticipantCmd);

                            sharding_ddl_util::sendAuthenticatedCommandToShards(
                                opCtx, opts, participants);
                        } else {
                            generic_argument_util::setDbVersionIfPresent(
                                dropDatabaseParticipantCmd, *metadata().getDatabaseVersion());

                            auto opts = std::make_shared<
                                async_rpc::AsyncRPCOptions<ShardsvrDropDatabaseParticipant>>(
                                **executor, token, dropDatabaseParticipantCmd);
                            try {
                                sharding_ddl_util::sendAuthenticatedCommandToShards(
                                    opCtx, opts, participants);
                            } catch (ExceptionFor<ErrorCodes::StaleDbVersion>&) {
                                // The DB metadata could have been removed by a network-partitioned
                                // former primary
                            }
                        }
                    }

                    if (_doc.getAuthoritativeMetadataAccessLevel() >=
                        AuthoritativeMetadataAccessLevelEnum::kWritesAllowed) {
                        const auto& session = getNewSession(opCtx);
                        removeDatabaseMetadataFromShard(opCtx, _dbName, session, executor, token);
                    }

                    {
                        auto buildNewSessionFn = [this](OperationContext* opCtx) {
                            return getNewSession(opCtx);
                        };

                        // After performing the db drop on the local catalog, do the same on the
                        // global one, then generate the placement changing notification for change
                        // stream readers tracking the db name.
                        auto commitTime =
                            commitDropDatabaseOnGlobalCatalog(opCtx,
                                                              **executor,
                                                              _dbName,
                                                              *metadata().getDatabaseVersion(),
                                                              buildNewSessionFn);

                        NamespacePlacementChanged notification(dbNss, commitTime);

                        sharding_ddl_util::generatePlacementChangeNotificationOnShard(
                            opCtx,
                            notification,
                            primaryShardId,
                            buildNewSessionFn,
                            executor,
                            token);
                    }

                    VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);
                }
            }))
        .then([this, executor = executor, dbNss, anchor = shared_from_this()] {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            if (MONGO_unlikely(dropDatabaseCoordinatorPauseAfterConfigCommit.shouldFail())) {
                dropDatabaseCoordinatorPauseAfterConfigCommit.pauseWhileSet();
            }

            const bool clearDbMetadata = _doc.getAuthoritativeMetadataAccessLevel() ==
                AuthoritativeMetadataAccessLevelEnum::kNone;

            std::unique_ptr<ShardingRecoveryService::BeforeReleasingCustomAction> actionPtr;
            if (clearDbMetadata) {
                actionPtr = std::make_unique<ShardingRecoveryService::FilteringMetadataClearer>();
            } else {
                actionPtr = std::make_unique<ShardingRecoveryService::NoCustomAction>();
            }

            ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
                opCtx,
                dbNss,
                _critSecReason,
                defaultMajorityWriteConcern(),
                *actionPtr,
                false /* throwIfReasonDiffers */);
        })
        .then([this, token, dbNss, executor = executor, anchor = shared_from_this()] {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            if (_doc.getAuthoritativeMetadataAccessLevel() ==
                AuthoritativeMetadataAccessLevelEnum::kNone) {
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
                generic_argument_util::setMajorityWriteConcern(flushDbCacheUpdatesCmd);

                IgnoreAPIParametersBlock ignoreApiParametersBlock{opCtx};
                auto opts = std::make_shared<
                    async_rpc::AsyncRPCOptions<FlushDatabaseCacheUpdatesWithWriteConcern>>(
                    **executor, token, flushDbCacheUpdatesCmd);
                sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, participants);
            }

            ShardingLogging::get(opCtx)->logChange(opCtx, "dropDatabase", dbNss);
            LOGV2(5494506, "Database dropped", "db"_attr = _dbName);
        })
        .then([this, anchor = shared_from_this()] {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            BatchedCommandRequest request([&] {
                write_ops::DeleteOpEntry deleteDatabaseEntryOp{
                    makeDatabaseQuery(_dbName, *metadata().getDatabaseVersion()), false};

                write_ops::DeleteCommandRequest deleteDatabaseEntry(
                    NamespaceString::kConfigDropPendingDBsNamespace, {deleteDatabaseEntryOp});

                return deleteDatabaseEntry;
            }());

            auto configServer = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            uassertStatusOK(configServer
                                ->runBatchWriteCommand(opCtx,
                                                       Milliseconds::max(),
                                                       request,
                                                       defaultMajorityWriteConcernDoNotUse(),
                                                       Shard::RetryPolicy::kIdempotent)
                                .toStatus());
        });
}

}  // namespace mongo
