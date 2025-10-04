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


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/routing_information_cache.h"
#include "mongo/db/global_catalog/ddl/create_database_util.h"
#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"
#include "mongo/db/global_catalog/ddl/notify_sharding_event_gen.h"
#include "mongo/db/global_catalog/ddl/shard_util.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

DatabaseType ShardingCatalogManager::createDatabase(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const boost::optional<ShardId>& optResolvedPrimaryShard,
    const SerializationContext& serializationContext) {
    // Make sure to force update of any stale metadata
    ON_BLOCK_EXIT([&] { RoutingInformationCache::get(opCtx)->purgeDatabase(dbName); });

    const auto dbNameStr = DatabaseNameUtil::serialize(dbName, serializationContext);
    const auto dbMatchFilterExact =
        create_database_util::constructDbMatchFilterExact(dbNameStr, optResolvedPrimaryShard);

    auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
    DBDirectClient client(opCtx);
    boost::optional<DDLLockManager::ScopedBaseDDLLock> dbLock;

    // First perform an optimistic attempt without taking the lock to check if database exists.
    // If the database is not found take the lock and try again.
    while (true) {
        auto dbObj = client.findOne(NamespaceString::kConfigDatabasesNamespace, dbMatchFilterExact);
        if (!dbObj.isEmpty()) {
            replClient.setLastOpToSystemLastOpTime(opCtx);
            return DatabaseType::parse(dbObj, IDLParserContext("DatabaseType"));
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
    const auto dbMatchFilterCaseInsensitive =
        create_database_util::constructDbMatchFilterCaseInsensitive(dbNameStr);
    auto dbDoc =
        client.findOne(NamespaceString::kConfigDatabasesNamespace, dbMatchFilterCaseInsensitive);
    auto returnDatabaseValue = [&] {
        if (!dbDoc.isEmpty()) {
            auto actualDb = DatabaseType::parse(dbDoc, IDLParserContext("DatabaseType"));
            create_database_util::checkAgainstExistingDbDoc(
                actualDb, dbName, optResolvedPrimaryShard);

            // We did a local read of the database entry above and found that the database already
            // exists. However, the data may not be majority committed (a previous createDatabase
            // attempt may have failed with a writeConcern error).
            // Since the current Client doesn't know the opTime of the last write to the database
            // entry, make it wait for the last opTime in the system when we wait for writeConcern.
            replClient.setLastOpToSystemLastOpTime(opCtx);

            return actualDb;
        } else {
            while (true) {
                const auto candidatePrimaryShardId =
                    create_database_util::getCandidatePrimaryShard(opCtx, optResolvedPrimaryShard);

                auto retries = 10;
                try {
                    return commitCreateDatabase(opCtx,
                                                dbName,
                                                candidatePrimaryShardId,
                                                optResolvedPrimaryShard.is_initialized());
                } catch (const ExceptionFor<ErrorCodes::ShardNotFound>& ex) {
                    create_database_util::logCommitCreateDatabaseFailed(dbName, redact(ex));
                    // The proposed primaryShard was found to not exist or be draining when
                    // attempting to commit.
                    if (optResolvedPrimaryShard) {
                        // If a primary shard was explicitly selected by the caller, then throw the
                        // error.
                        throw;
                    } else {
                        // If no primary shard was explicitly selected by the caller, then choose a
                        // new one and retry.
                        retries--;
                        if (retries > 0) {
                            continue;
                        } else {
                            LOGV2_WARNING(8917901,
                                          "Exhausted retries trying to commit create database",
                                          "dbName"_attr = dbName);
                            throw;
                        }
                    }
                }
            }
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
    create_database_util::refreshDbVersionOnPrimaryShard(
        opCtx, dbNameStr, returnDatabaseValue.getPrimary());

    return returnDatabaseValue;
}

void ShardingCatalogManager::commitMovePrimary(OperationContext* opCtx,
                                               const DatabaseName& dbName,
                                               const DatabaseVersion& expectedDbVersion,
                                               const ShardId& toShardId,
                                               const SerializationContext& serializationContext) {
    // Hold the shard lock until the entire commit finishes to serialize with removeShard.
    Lock::SharedLock shardLock(opCtx, _kShardMembershipLock);

    const auto toShardDoc = [&] {
        DBDirectClient dbClient(opCtx);
        return dbClient.findOne(NamespaceString::kConfigsvrShardsNamespace,
                                BSON(ShardType::name << toShardId));
    }();
    uassert(ErrorCodes::ShardNotFound,
            fmt::format("Requested primary shard {} does not exist", toShardId.toString()),
            !toShardDoc.isEmpty());

    const auto toShardEntry = uassertStatusOK(ShardType::fromBSON(toShardDoc));
    uassert(ErrorCodes::ShardNotFound,
            fmt::format("Requested primary shard {} is draining", toShardId.toString()),
            !toShardEntry.getDraining());

    const auto currentTime = VectorClock::get(opCtx)->getTime();
    const auto validAfter = currentTime.clusterTime().asTimestamp();

    const auto transactionChain =
        [dbName, expectedDbVersion, toShardId, validAfter, serializationContext](
            const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
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

            auto dbEntry = DatabaseType::parse(dbs.front(), IDLParserContext("DatabaseType"));

            // Update the database entry and insert a placement history entry for the database.
            const auto updateDatabaseEntryOp = [&] {
                const auto query = [&] {
                    BSONObjBuilder bsonBuilder;
                    bsonBuilder.append(DatabaseType::kDbNameFieldName,
                                       DatabaseNameUtil::serialize(dbName, serializationContext));
                    // Include the version in the update filter to be resilient to potential
                    // network retries and delayed messages.
                    for (const auto [fieldName, fieldValue] : expectedDbVersion.toBSON()) {
                        const auto dottedFieldName =
                            DatabaseType::kVersionFieldName + "." + fieldName;
                        bsonBuilder.appendAs(fieldValue, dottedFieldName);
                    }
                    return bsonBuilder.obj();
                }();

                const auto update = [&] {
                    auto newDbVersion = expectedDbVersion.makeUpdated();
                    newDbVersion.setTimestamp(validAfter);

                    tassert(8235300,
                            "New database timestamp must be newer than previous one",
                            newDbVersion.getTimestamp() > expectedDbVersion.getTimestamp());

                    BSONObjBuilder bsonBuilder;
                    bsonBuilder.append(DatabaseType::kPrimaryFieldName, toShardId);
                    bsonBuilder.append(DatabaseType::kVersionFieldName, newDbVersion.toBSON());
                    return BSON("$set" << bsonBuilder.obj());
                }();

                write_ops::UpdateCommandRequest updateOp(
                    NamespaceString::kConfigDatabasesNamespace);
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

DatabaseType ShardingCatalogManager::commitCreateDatabase(OperationContext* opCtx,
                                                          const DatabaseName& dbName,
                                                          const ShardId& primaryShard,
                                                          bool userSelectedPrimary) {
    // The database does not exist. Insert an entry for the new database into the sharding
    // catalog. Assign also a primary shard if the caller hasn't specified one.
    ShardingLogging::get(opCtx)->logChange(
        opCtx,
        "createDatabase.start",
        NamespaceString(dbName),
        BSON("primaryShard" << primaryShard << "userSelectedPrimary" << userSelectedPrimary),
        defaultMajorityWriteConcernDoNotUse(),
        _localConfigShard,
        _localCatalogClient.get());

    DatabaseType db = [&]() {
        // Hold _kShardMembershipLock until the entire commit finishes to serialize with removeShard
        // in order to guarantee that the proposed dbPrimary shard continues to exist (and the
        // user-selected primary shard is not draining) throughout the commit.
        Lock::SharedLock shardLock(opCtx, _kShardMembershipLock);

        const auto shardDocs = uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            NamespaceString::kConfigsvrShardsNamespace,
            BSON(ShardType::name << primaryShard),
            {},
            1));
        uassert(ErrorCodes::ShardNotFound,
                "Selected primary shard for new database does not exist",
                !shardDocs.docs.empty());
        const auto shardDoc = uassertStatusOK(ShardType::fromBSON(shardDocs.docs.front()));
        // Fails only if a user selects a draining shard as its primary shard explicitly.
        uassert(ErrorCodes::ShardNotFound,
                "Cannot select draining shard as primary for new database",
                !shardDoc.getDraining() || !userSelectedPrimary);

        // Pick a clusterTime that will be used as the 'timestamp' of the new database.
        const auto now = VectorClock::get(opCtx)->getTime();
        const auto clusterTime = now.clusterTime().asTimestamp();

        DatabaseType db(dbName, primaryShard, DatabaseVersion(UUID::gen(), clusterTime));

        LOGV2(21938, "Registering new database in sharding catalog", "db"_attr = db);
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

        return db;
    }();

    ShardingLogging::get(opCtx)->logChange(
        opCtx,
        "createDatabase",
        NamespaceString(dbName),
        BSON("primaryShard" << db.getPrimary() << "userSelectedPrimary" << userSelectedPrimary
                            << "version" << db.getVersion().toBSON()),
        defaultMajorityWriteConcernDoNotUse(),
        _localConfigShard,
        _localCatalogClient.get());

    return db;
}

}  // namespace mongo
