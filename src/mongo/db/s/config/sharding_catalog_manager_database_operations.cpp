/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <cstddef>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/commands/notify_sharding_event_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/locker_api.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/resource_yielder.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/ddl_lock_manager.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/vector_clock.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/pcre_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

using namespace fmt::literals;

}  // namespace

DatabaseType ShardingCatalogManager::createDatabase(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const boost::optional<ShardId>& optPrimaryShard,
    const SerializationContext& serializationContext) {

    if (dbName.isConfigDB()) {
        return DatabaseType(dbName, ShardId::kConfigServerId, DatabaseVersion::makeFixed());
    }

    // It is not allowed to create the 'admin' or 'local' databases, including any alternative
    // casing. It is allowed to create the 'config' database (handled by the early return above),
    // but only with that exact casing.
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "Cannot manually create database '" << dbName.toStringForErrorMsg()
                          << "'",
            !(dbName.equalCaseInsensitive(DatabaseName::kAdmin)) &&
                !(dbName.equalCaseInsensitive(DatabaseName::kLocal)) &&
                !(dbName.equalCaseInsensitive(DatabaseName::kConfig)));

    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid db name specified: " << dbName.toStringForErrorMsg(),
            DatabaseName::isValid(dbName, DatabaseName::DollarInDbNameBehavior::Allow));

    // Make sure to force update of any stale metadata
    ON_BLOCK_EXIT([&] { Grid::get(opCtx)->catalogCache()->purgeDatabase(dbName); });

    auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());

    DBDirectClient client(opCtx);

    // Resolve the shard against the received parameter (which may encode either a shard ID or a
    // connection string).
    if (optPrimaryShard) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "invalid shard name: " << *optPrimaryShard,
                optPrimaryShard->isValid());
    }
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    auto resolvedPrimaryShard = optPrimaryShard
        ? uassertStatusOK(shardRegistry->getShard(opCtx, *optPrimaryShard))
        : nullptr;

    const auto dbNameStr = DatabaseNameUtil::serialize(dbName, serializationContext);
    const auto dbMatchFilter = [&] {
        BSONObjBuilder filterBuilder;
        filterBuilder.append(DatabaseType::kDbNameFieldName, dbNameStr);
        if (resolvedPrimaryShard) {
            filterBuilder.append(DatabaseType::kPrimaryFieldName, resolvedPrimaryShard->getId());
        }
        return filterBuilder.obj();
    }();

    boost::optional<DDLLockManager::ScopedBaseDDLLock> dbLock;

    // First perform an optimistic attempt without taking the lock to check if database exists.
    // If the database is not found take the lock and try again.
    while (true) {
        auto dbObj = client.findOne(NamespaceString::kConfigDatabasesNamespace, dbMatchFilter);
        if (!dbObj.isEmpty()) {
            replClient.setLastOpToSystemLastOpTime(opCtx);
            return DatabaseType::parse(IDLParserContext("DatabaseType"), dbObj);
        }

        if (dbLock) {
            break;
        }

        // Do another loop, with the db lock held in order to avoid taking the expensive path on
        // concurrent create database operations
        dbLock.emplace(opCtx,
                       shard_role_details::getLocker(opCtx),
                       DatabaseNameUtil::deserialize(
                           boost::none, str::toLower(dbNameStr), serializationContext),
                       "createDatabase" /* reason */,
                       MODE_X,
                       true /*waitForRecovery*/);
    }

    // Expensive createDatabase code path

    // Check if a database already exists with the same name (case insensitive), and if so, return
    // the existing entry.
    BSONObjBuilder queryBuilder;
    queryBuilder.appendRegex(
        DatabaseType::kDbNameFieldName, "^{}$"_format(pcre_util::quoteMeta(dbNameStr)), "i");

    auto dbDoc = client.findOne(NamespaceString::kConfigDatabasesNamespace, queryBuilder.obj());
    auto const [primaryShardPtr, database] = [&] {
        if (!dbDoc.isEmpty()) {
            auto actualDb = DatabaseType::parse(IDLParserContext("DatabaseType"), dbDoc);

            uassert(ErrorCodes::DatabaseDifferCase,
                    str::stream() << "can't have 2 databases that just differ on case "
                                  << " have: " << actualDb.getDbName().toStringForErrorMsg()
                                  << " want to add: " << dbName.toStringForErrorMsg(),
                    actualDb.getDbName() == dbName);

            uassert(
                ErrorCodes::NamespaceExists,
                str::stream() << "database already created on a primary which is different from "
                              << resolvedPrimaryShard->getId(),
                !resolvedPrimaryShard || resolvedPrimaryShard->getId() == actualDb.getPrimary());

            // We did a local read of the database entry above and found that the database already
            // exists. However, the data may not be majority committed (a previous createDatabase
            // attempt may have failed with a writeConcern error).
            // Since the current Client doesn't know the opTime of the last write to the database
            // entry, make it wait for the last opTime in the system when we wait for writeConcern.
            replClient.setLastOpToSystemLastOpTime(opCtx);

            return std::make_pair(
                uassertStatusOK(shardRegistry->getShard(opCtx, actualDb.getPrimary())), actualDb);
        } else {
            // The database does not exist. Insert an entry for the new database into the sharding
            // catalog. Assign also a primary shard if the caller hasn't specified one.
            if (!resolvedPrimaryShard) {
                // Assigns an optimal shard on which to place a newly created database from the set
                // of available shards.
                resolvedPrimaryShard = uassertStatusOK(
                    shardRegistry->getShard(opCtx, shardutil::selectLeastLoadedShard(opCtx)));
            }

            ShardingLogging::get(opCtx)->logChange(opCtx,
                                                   "createDatabase.start",
                                                   NamespaceString(dbName),
                                                   /* details */ BSONObj(),
                                                   ShardingCatalogClient::kMajorityWriteConcern,
                                                   _localConfigShard,
                                                   _localCatalogClient.get());

            const auto now = VectorClock::get(opCtx)->getTime();
            const auto clusterTime = now.clusterTime().asTimestamp();

            // Pick a primary shard for the new database.
            DatabaseType db(
                dbName, resolvedPrimaryShard->getId(), DatabaseVersion(UUID::gen(), clusterTime));

            LOGV2(21938, "Registering new database in sharding catalog", "db"_attr = db);

            // The creation of a new database (and its assignation to resolvedPrimaryShard) is
            // described by the notification of multiple events, following a 2-phase protocol:
            // - a "prepare" notification prior to the write into config.databases will ensure that
            // change streams will start collecting events on the new database before the first user
            // write on one of its future collection occurs
            // - a "commitSuccessful" notification after completing the write into config.databases
            // will allow change streams to stop collecting events on the namespace created from
            // shards != resolvedPrimaryShard.
            const auto allShards = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
            {
                DatabasesAdded prepareCommitEvent(
                    {dbName}, false /*areImported*/, CommitPhaseEnum::kPrepare);
                prepareCommitEvent.setPrimaryShard(resolvedPrimaryShard->getId());
                uassertStatusOK(_notifyClusterOnNewDatabases(opCtx, prepareCommitEvent, allShards));
            }

            const auto transactionChain = [db](const txn_api::TransactionClient& txnClient,
                                               ExecutorPtr txnExec) {
                write_ops::InsertCommandRequest insertDatabaseEntryOp(
                    NamespaceString::kConfigDatabasesNamespace);
                insertDatabaseEntryOp.setDocuments({db.toBSON()});
                return txnClient.runCRUDOp(insertDatabaseEntryOp, {})
                    .thenRunOn(txnExec)
                    .then([&txnClient, &txnExec, &db](
                              const BatchedCommandResponse& insertDatabaseEntryResponse) {
                        uassertStatusOK(insertDatabaseEntryResponse.toStatus());
                        NamespacePlacementType placementInfo(
                            NamespaceString(db.getDbName()),
                            db.getVersion().getTimestamp(),
                            std::vector<mongo::ShardId>{db.getPrimary()});
                        write_ops::InsertCommandRequest insertPlacementHistoryOp(
                            NamespaceString::kConfigsvrPlacementHistoryNamespace);
                        insertPlacementHistoryOp.setDocuments({placementInfo.toBSON()});

                        return txnClient.runCRUDOp(insertPlacementHistoryOp, {});
                    })
                    .thenRunOn(txnExec)
                    .then([](const BatchedCommandResponse& insertPlacementHistoryResponse) {
                        uassertStatusOK(insertPlacementHistoryResponse.toStatus());
                    })
                    .semi();
            };

            auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
            auto inlineExecutor = std::make_shared<executor::InlineExecutor>();

            txn_api::SyncTransactionWithRetries txn(
                opCtx, executor, nullptr /*resourceYielder*/, inlineExecutor);
            txn.run(opCtx, transactionChain);

            DatabasesAdded commitCompletedEvent(
                {dbName}, false /*areImported*/, CommitPhaseEnum::kSuccessful);
            const auto notificationOutcome =
                _notifyClusterOnNewDatabases(opCtx, commitCompletedEvent, allShards);
            if (!notificationOutcome.isOK()) {
                LOGV2_WARNING(7175500,
                              "Unable to send out notification of successful createDatabase",
                              "db"_attr = db,
                              "err"_attr = notificationOutcome);
            }

            ShardingLogging::get(opCtx)->logChange(opCtx,
                                                   "createDatabase",
                                                   NamespaceString(dbName),
                                                   /* details */ BSONObj(),
                                                   ShardingCatalogClient::kMajorityWriteConcern,
                                                   _localConfigShard,
                                                   _localCatalogClient.get());

            return std::make_pair(resolvedPrimaryShard, db);
        }
    }();

    // Note, making the primary shard refresh its databaseVersion here is not required for
    // correctness, since either:
    // 1) This is the first time this database is being created. The primary shard will not have a
    //    databaseVersion already cached.
    // 2) The database was dropped and is being re-created. Since dropping a database also sends
    //    _flushDatabaseCacheUpdates to all shards, the primary shard should not have a database
    //    version cached. (Note, it is possible that dropping a database will skip sending
    //    _flushDatabaseCacheUpdates if the config server fails over while dropping the database.)
    // However, routers don't support retrying internally on StaleDbVersion in transactions
    // (SERVER-39704), so if the first operation run against the database is in a transaction, it
    // would fail with StaleDbVersion. Making the primary shard refresh here allows that first
    // transaction to succeed. This allows our transaction passthrough suites and transaction demos
    // to succeed without additional special logic.
    auto cmdResponse = uassertStatusOK(primaryShardPtr->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        DatabaseName::kAdmin,
        BSON("_flushDatabaseCacheUpdates"
             << DatabaseNameUtil::serialize(dbName, serializationContext)),
        Shard::RetryPolicy::kIdempotent));
    uassertStatusOK(cmdResponse.commandStatus);

    return database;
}

void ShardingCatalogManager::commitMovePrimary(OperationContext* opCtx,
                                               const DatabaseName& dbName,
                                               const DatabaseVersion& expectedDbVersion,
                                               const ShardId& toShardId,
                                               const SerializationContext& serializationContext,
                                               bool cloneOnlyUntrackedColls) {
    // Hold the shard lock until the entire commit finishes to serialize with removeShard.
    Lock::SharedLock shardLock(opCtx, _kShardMembershipLock);

    const auto toShardDoc = [&] {
        DBDirectClient dbClient(opCtx);
        return dbClient.findOne(NamespaceString::kConfigsvrShardsNamespace,
                                BSON(ShardType::name << toShardId));
    }();
    uassert(ErrorCodes::ShardNotFound,
            "Requested primary shard {} does not exist"_format(toShardId.toString()),
            !toShardDoc.isEmpty());

    const auto toShardEntry = uassertStatusOK(ShardType::fromBSON(toShardDoc));
    uassert(ErrorCodes::ShardNotFound,
            "Requested primary shard {} is draining"_format(toShardId.toString()),
            !toShardEntry.getDraining());

    const auto currentTime = VectorClock::get(opCtx)->getTime();
    const auto validAfter = currentTime.clusterTime().asTimestamp();

    const auto transactionChain = [dbName,
                                   expectedDbVersion,
                                   toShardId,
                                   validAfter,
                                   cloneOnlyUntrackedColls,
                                   serializationContext](
                                      const txn_api::TransactionClient& txnClient,
                                      ExecutorPtr txnExec) {
        int currStmtId = 0;
        // Find database entry to get current dbPrimary
        FindCommandRequest findDb(NamespaceString::kConfigDatabasesNamespace);
        const auto query = [&] {
            BSONObjBuilder bsonBuilder;
            bsonBuilder.append(DatabaseType::kDbNameFieldName,
                               DatabaseNameUtil::serialize(dbName, serializationContext));
            // Include the version in the update filter to be resilient to potential
            // network retries and delayed messages.
            for (const auto [fieldName, fieldValue] : expectedDbVersion.toBSON()) {
                const auto dottedFieldName = DatabaseType::kVersionFieldName + "." + fieldName;
                bsonBuilder.appendAs(fieldValue, dottedFieldName);
            }
            return bsonBuilder.obj();
        }();
        findDb.setFilter(query);
        findDb.setSingleBatch(true);
        auto dbs = txnClient.exhaustiveFindSync(findDb);

        // If we didn't find a database entry, this must be a retry of the transaction
        if (dbs.size() == 0) {
            return SemiFuture<void>::makeReady();
        }

        auto dbEntry = DatabaseType::parse(IDLParserContext("DatabaseType"), dbs.front());

        // Only change the unsharded collection metadata on the config server if we are moving some
        // tracked collections. Otherwise, there will be no collections or chunks to find here for
        // the collections being moved, so we can skip the whole section.
        if (!cloneOnlyUntrackedColls) {
            // Find all collections in the database that are unsplittable
            FindCommandRequest findColls(CollectionType::ConfigNS);
            BSONObjBuilder b;
            const auto db = DatabaseNameUtil::serialize(dbName, serializationContext);
            b.appendRegex(CollectionType::kNssFieldName, "^{}\\."_format(pcre_util::quoteMeta(db)));
            findColls.setFilter(b.obj());
            auto colls = txnClient.exhaustiveFindSync(findColls);

            // For each collection, we need to insert a placement entry for the collection and
            // update the chunk entry.
            std::vector<BSONObj> insertCollPlacementEntries;
            insertCollPlacementEntries.reserve(colls.size());
            std::vector<write_ops::UpdateOpEntry> updateChunksEntries;
            updateChunksEntries.reserve(colls.size());
            std::vector<int> chunkUpdateIds;
            std::vector<int> collPlacementInsertIds;
            for (const auto& collObj : colls) {
                auto coll =
                    CollectionTypeBase::parse(IDLParserContext("CommitMovePrimary"), collObj);
                if (!coll.getUnsplittable()) {
                    continue;
                }
                FindCommandRequest findChunk(ChunkType::ConfigNS);
                findChunk.setFilter(BSON(ChunkType::collectionUUID.name() << coll.getUuid()));
                findChunk.setSingleBatch(true);
                auto chunks = txnClient.exhaustiveFindSync(findChunk);
                invariant(chunks.size() == 1);
                auto chunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
                    chunks[0], *coll.getPre22CompatibleEpoch(), coll.getTimestamp()));
                if (chunk.getShard() != dbEntry.getPrimary()) {
                    continue;
                }
                chunk.setShard(toShardId);
                auto currentVersion = chunk.getVersion();
                chunk.setVersion(
                    ChunkVersion({*coll.getPre22CompatibleEpoch(), coll.getTimestamp()},
                                 {currentVersion.majorVersion() + 1, 0}));
                auto newHistory = chunk.getHistory();
                int entriesDeleted = 0;
                while (newHistory.size() > 1 &&
                       newHistory.back().getValidAfter().getSecs() + 10 < validAfter.getSecs()) {
                    newHistory.pop_back();
                    ++entriesDeleted;
                }
                logv2::DynamicAttributes attrs;
                attrs.add("entriesDeleted", entriesDeleted);
                if (!newHistory.empty()) {
                    attrs.add("oldestEntryValidAfter", newHistory.back().getValidAfter());
                }
                chunk.setOnCurrentShardSince(validAfter);
                newHistory.emplace(newHistory.begin(),
                                   ChunkHistory(*chunk.getOnCurrentShardSince(), toShardId));
                chunk.setHistory(std::move(newHistory));
                write_ops::UpdateOpEntry entry;
                entry.setQ(BSON(ChunkType::collectionUUID.name() << coll.getUuid()));
                entry.setU(
                    write_ops::UpdateModification::parseFromClassicUpdate(chunk.toConfigBSON()));
                updateChunksEntries.push_back(entry);

                NamespacePlacementType placementInfo(coll.getNss(), validAfter, {toShardId});
                placementInfo.setUuid(coll.getUuid());
                insertCollPlacementEntries.push_back(placementInfo.toBSON());

                collPlacementInsertIds.push_back(currStmtId);
                chunkUpdateIds.push_back(currStmtId + colls.size());
                currStmtId++;
            }

            // Execute chunk and collection placement updates if there were any generated.
            if (updateChunksEntries.size() != 0) {
                const auto updateChunksEntryOp = [&] {
                    write_ops::UpdateCommandRequest updateOp(
                        NamespaceString::kConfigsvrChunksNamespace);
                    updateOp.setUpdates(updateChunksEntries);
                    return updateOp;
                }();
                write_ops::InsertCommandRequest insertPlacementReq(
                    NamespaceString::kConfigsvrPlacementHistoryNamespace,
                    insertCollPlacementEntries);
                auto insertPlacementResponse =
                    txnClient.runCRUDOpSync(insertPlacementReq, collPlacementInsertIds);
                uassertStatusOK(insertPlacementResponse.toStatus());
                auto updateChunksResponse =
                    txnClient.runCRUDOpSync(updateChunksEntryOp, chunkUpdateIds);
                uassertStatusOK(updateChunksResponse.toStatus());
                currStmtId += colls.size();
            }
        }

        // Update the database entry and insert a placement history entry for the database.
        const auto updateDatabaseEntryOp = [&] {
            const auto query = [&] {
                BSONObjBuilder bsonBuilder;
                bsonBuilder.append(DatabaseType::kDbNameFieldName,
                                   DatabaseNameUtil::serialize(dbName, serializationContext));
                // Include the version in the update filter to be resilient to potential
                // network retries and delayed messages.
                for (const auto [fieldName, fieldValue] : expectedDbVersion.toBSON()) {
                    const auto dottedFieldName = DatabaseType::kVersionFieldName + "." + fieldName;
                    bsonBuilder.appendAs(fieldValue, dottedFieldName);
                }
                return bsonBuilder.obj();
            }();

            const auto update = [&] {
                const auto newDbVersion = expectedDbVersion.makeUpdated();

                BSONObjBuilder bsonBuilder;
                bsonBuilder.append(DatabaseType::kPrimaryFieldName, toShardId);
                bsonBuilder.append(DatabaseType::kVersionFieldName, newDbVersion.toBSON());
                return BSON("$set" << bsonBuilder.obj());
            }();

            write_ops::UpdateCommandRequest updateOp(NamespaceString::kConfigDatabasesNamespace);
            updateOp.setUpdates({[&] {
                write_ops::UpdateOpEntry entry;
                entry.setQ(query);
                entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
                return entry;
            }()});

            return updateOp;
        }();

        auto updateDatabaseEntryResponse =
            txnClient.runCRUDOpSync(updateDatabaseEntryOp, {currStmtId++});
        uassertStatusOK(updateDatabaseEntryResponse.toStatus());

        NamespacePlacementType placementInfo(
            NamespaceString(dbName), validAfter, std::vector<mongo::ShardId>{toShardId});

        write_ops::InsertCommandRequest insertPlacementHistoryOp(
            NamespaceString::kConfigsvrPlacementHistoryNamespace);
        insertPlacementHistoryOp.setDocuments({placementInfo.toBSON()});

        auto insertDatabasePlacementHistoryResponse =
            txnClient.runCRUDOpSync(insertPlacementHistoryOp, {currStmtId++});
        uassertStatusOK(insertDatabasePlacementHistoryResponse.toStatus());

        return SemiFuture<void>::makeReady();
    };

    auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();

    txn_api::SyncTransactionWithRetries txn(opCtx,
                                            executor,
                                            nullptr, /*resourceYielder*/
                                            inlineExecutor);
    txn.run(opCtx, transactionChain);
}

}  // namespace mongo
