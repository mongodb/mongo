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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/db/s/config/sharding_catalog_manager.h"

#include <pcrecpp.h>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/server_options.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/shard_util.h"

namespace mongo {
namespace {

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});

/**
 * Selects an optimal shard on which to place a newly created database from the set of available
 * shards. Will return ShardNotFound if shard could not be found.
 */
ShardId selectShardForNewDatabase(OperationContext* opCtx, ShardRegistry* shardRegistry) {
    std::vector<ShardId> allShardIds;

    // Ensure the shard registry contains the most up-to-date list of available shards
    shardRegistry->reload(opCtx);
    shardRegistry->getAllShardIds(opCtx, &allShardIds);
    uassert(ErrorCodes::ShardNotFound, "No shards found", !allShardIds.empty());

    ShardId candidateShardId = allShardIds[0];

    auto candidateSize =
        uassertStatusOK(shardutil::retrieveTotalShardSize(opCtx, candidateShardId));

    for (size_t i = 1; i < allShardIds.size(); i++) {
        const ShardId shardId = allShardIds[i];

        const auto currentSize = uassertStatusOK(shardutil::retrieveTotalShardSize(opCtx, shardId));

        if (currentSize < candidateSize) {
            candidateSize = currentSize;
            candidateShardId = shardId;
        }
    }

    return candidateShardId;
}

}  // namespace

DatabaseType ShardingCatalogManager::createDatabase(OperationContext* opCtx,
                                                    StringData dbName,
                                                    const ShardId& primaryShard) {
    invariant(nsIsDbOnly(dbName));

    // The admin and config databases should never be explicitly created. They "just exist",
    // i.e. getDatabase will always return an entry for them.
    if (dbName == NamespaceString::kAdminDb || dbName == NamespaceString::kConfigDb) {
        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << "cannot manually create database '" << dbName << "'");
    }

    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    // Check if a database already exists with the same name (case sensitive), and if so, return the
    // existing entry.

    BSONObjBuilder queryBuilder;
    queryBuilder.appendRegex(DatabaseType::name(),
                             (std::string) "^" + pcrecpp::RE::QuoteMeta(dbName.toString()) + "$",
                             "i");

    auto docs = uassertStatusOK(catalogClient->_exhaustiveFindOnConfig(
                                    opCtx,
                                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                    repl::ReadConcernLevel::kLocalReadConcern,
                                    DatabaseType::ConfigNS,
                                    queryBuilder.obj(),
                                    BSONObj(),
                                    1))
                    .value;

    auto const [primaryShardPtr, database] = [&] {
        if (!docs.empty()) {
            auto actualDb = uassertStatusOK(DatabaseType::fromBSON(docs.front()));

            uassert(ErrorCodes::DatabaseDifferCase,
                    str::stream() << "can't have 2 databases that just differ on case "
                                  << " have: " << actualDb.getName()
                                  << " want to add: " << dbName.toString(),
                    actualDb.getName() == dbName.toString());

            uassert(
                ErrorCodes::NamespaceExists,
                str::stream() << "database already created on a primary which is different from: "
                              << primaryShard,
                !primaryShard.isValid() || actualDb.getPrimary() == primaryShard);

            // We did a local read of the database entry above and found that the database already
            // exists. However, the data may not be majority committed (a previous createDatabase
            // attempt may have failed with a writeConcern error).
            // Since the current Client doesn't know the opTime of the last write to the database
            // entry, make it wait for the last opTime in the system when we wait for writeConcern.
            auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
            replClient.setLastOpToSystemLastOpTime(opCtx);

            WriteConcernResult unusedResult;
            uassertStatusOK(waitForWriteConcern(opCtx,
                                                replClient.getLastOp(),
                                                ShardingCatalogClient::kMajorityWriteConcern,
                                                &unusedResult));
            return std::make_pair(
                uassertStatusOK(shardRegistry->getShard(opCtx, actualDb.getPrimary())), actualDb);
        } else {
            // The database does not exist. Insert an entry for the new database into the sharding
            // catalog.
            auto const shardPtr = uassertStatusOK(shardRegistry->getShard(
                opCtx,
                primaryShard.isValid() ? primaryShard
                                       : selectShardForNewDatabase(opCtx, shardRegistry)));

            // Pick a primary shard for the new database.
            DatabaseType db(
                dbName.toString(), shardPtr->getId(), false, databaseVersion::makeNew());

            LOGV2(21938, "Registering new database {db} in sharding catalog", "db"_attr = db);

            // Do this write with majority writeConcern to guarantee that the shard sees the write
            // when it receives the _flushDatabaseCacheUpdates.
            uassertStatusOK(
                catalogClient->insertConfigDocument(opCtx,
                                                    DatabaseType::ConfigNS,
                                                    db.toBSON(),
                                                    ShardingCatalogClient::kMajorityWriteConcern));

            return std::make_pair(shardPtr, db);
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
        "admin",
        BSON("_flushDatabaseCacheUpdates" << dbName),
        Shard::RetryPolicy::kIdempotent));

    // If the shard had binary version v4.2 when it received the _flushDatabaseCacheUpdates, it will
    // have responded with NamespaceNotFound, because the shard does not have the database (see
    // SERVER-34431). Ignore this error, since the _flushDatabaseCacheUpdates is only a nicety for
    // users testing transactions, and the transaction passthrough suites do not change shard binary
    // versions.
    if (cmdResponse.commandStatus != ErrorCodes::NamespaceNotFound) {
        uassertStatusOK(cmdResponse.commandStatus);
    }

    return database;
}

void ShardingCatalogManager::enableSharding(OperationContext* opCtx,
                                            StringData dbName,
                                            const ShardId& primaryShard) {
    // Sharding is enabled automatically on the config db.
    if (dbName == NamespaceString::kConfigDb) {
        return;
    }

    // Creates the database if it doesn't exist and returns the new database entry, else returns the
    // existing database entry.
    auto dbType = createDatabase(opCtx, dbName, primaryShard);
    dbType.setSharded(true);

    // We must wait for the database entry to be majority committed, because it's possible that
    // reading from the majority snapshot has been set on the RecoveryUnit due to an earlier read,
    // such as overtaking a distlock or loading the ShardRegistry.
    WriteConcernResult unusedResult;
    uassertStatusOK(
        waitForWriteConcern(opCtx,
                            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp(),
                            WriteConcernOptions(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                Milliseconds{30000}),
                            &unusedResult));

    LOGV2(21939, "Enabling sharding for database [{dbName}] in config db", "dbName"_attr = dbName);

    uassertStatusOK(Grid::get(opCtx)->catalogClient()->updateConfigDocument(
        opCtx,
        DatabaseType::ConfigNS,
        BSON(DatabaseType::name(dbName.toString())),
        BSON("$set" << BSON(DatabaseType::sharded(true))),
        false,
        ShardingCatalogClient::kLocalWriteConcern));
}

StatusWith<std::vector<std::string>> ShardingCatalogManager::getDatabasesForShard(
    OperationContext* opCtx, const ShardId& shardId) {
    auto findStatus = Grid::get(opCtx)->catalogClient()->_exhaustiveFindOnConfig(
        opCtx,
        kConfigReadSelector,
        repl::ReadConcernLevel::kLocalReadConcern,
        DatabaseType::ConfigNS,
        BSON(DatabaseType::primary(shardId.toString())),
        BSONObj(),
        boost::none);  // no limit

    if (!findStatus.isOK())
        return findStatus.getStatus();

    std::vector<std::string> dbs;
    for (const BSONObj& obj : findStatus.getValue().value) {
        std::string dbName;
        Status status = bsonExtractStringField(obj, DatabaseType::name(), &dbName);
        if (!status.isOK()) {
            return status;
        }

        dbs.push_back(dbName);
    }

    return dbs;
}

Status ShardingCatalogManager::commitMovePrimary(OperationContext* opCtx,
                                                 const StringData dbname,
                                                 const ShardId& toShard) {

    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    // Must use local read concern because we will perform subsequent writes.
    auto findResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            DatabaseType::ConfigNS,
                                            BSON(DatabaseType::name << dbname),
                                            BSON(DatabaseType::name << -1),
                                            1));

    const auto databasesVector = std::move(findResponse.docs);
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Tried to find max database version for database '" << dbname
                          << "', but found no databases",
            !databasesVector.empty());

    const auto dbType = uassertStatusOK(DatabaseType::fromBSON(databasesVector.front()));

    if (dbType.getPrimary() == toShard) {
        // The primary has already been set to the destination shard. It's likely that there was a
        // network error and the shard resent the command.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return Status::OK();
    }

    auto newDbType = dbType;
    newDbType.setPrimary(toShard);

    auto const currentDatabaseVersion = dbType.getVersion();

    newDbType.setVersion(databaseVersion::makeIncremented(currentDatabaseVersion));

    auto updateQueryBuilder = BSONObjBuilder(BSON(DatabaseType::name << dbname));
    updateQueryBuilder.append(DatabaseType::version.name(), currentDatabaseVersion.toBSON());

    auto updateStatus = Grid::get(opCtx)->catalogClient()->updateConfigDocument(
        opCtx,
        DatabaseType::ConfigNS,
        updateQueryBuilder.obj(),
        newDbType.toBSON(),
        false,
        ShardingCatalogClient::kLocalWriteConcern);

    if (!updateStatus.isOK()) {
        LOGV2(21940,
              "error committing movePrimary: {dbname}{causedBy_updateStatus_getStatus}",
              "dbname"_attr = dbname,
              "causedBy_updateStatus_getStatus"_attr = causedBy(redact(updateStatus.getStatus())));
        return updateStatus.getStatus();
    }

    // If this assertion is tripped, it means that the request sent fine, but no documents were
    // updated. This is likely because the database version was changed in between the query and
    // the update, so no documents were found to change. This shouldn't happen however, because we
    // are holding the dist lock during the movePrimary operation.
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Tried to update primary shard for database '" << dbname
                          << " with version " << currentDatabaseVersion.getLastMod(),
            updateStatus.getValue());

    // Ensure the next attempt to retrieve the database or any of its collections will do a full
    // reload
    Grid::get(opCtx)->catalogCache()->purgeDatabase(dbname);

    return Status::OK();
}

}  // namespace mongo
