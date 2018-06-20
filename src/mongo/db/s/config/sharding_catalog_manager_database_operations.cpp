/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;
using std::vector;

namespace {

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});

}  // namespace

DatabaseType ShardingCatalogManager::createDatabase(OperationContext* opCtx,
                                                    const std::string& dbName) {
    invariant(nsIsDbOnly(dbName));

    // The admin and config databases should never be explicitly created. They "just exist",
    // i.e. getDatabase will always return an entry for them.
    if (dbName == "admin" || dbName == "config") {
        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << "cannot manually create database '" << dbName << "'");
    }

    // Check if a database already exists with the same name (case sensitive), and if so, return the
    // existing entry.

    BSONObjBuilder queryBuilder;
    queryBuilder.appendRegex(
        DatabaseType::name(), (string) "^" + pcrecpp::RE::QuoteMeta(dbName) + "$", "i");

    auto docs = uassertStatusOK(Grid::get(opCtx)->catalogClient()->_exhaustiveFindOnConfig(
                                    opCtx,
                                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                    repl::ReadConcernLevel::kLocalReadConcern,
                                    DatabaseType::ConfigNS,
                                    queryBuilder.obj(),
                                    BSONObj(),
                                    1))
                    .value;

    if (!docs.empty()) {
        BSONObj dbObj = docs.front();
        std::string actualDbName = dbObj[DatabaseType::name()].String();

        uassert(ErrorCodes::DatabaseDifferCase,
                str::stream() << "can't have 2 databases that just differ on case "
                              << " have: "
                              << actualDbName
                              << " want to add: "
                              << dbName,
                actualDbName == dbName);

        // We did a local read of the database entry above and found that the database already
        // exists. However, the data may not be majority committed (a previous createDatabase
        // attempt may have failed with a writeConcern error).
        // Since the current Client doesn't know the opTime of the last write to the database entry,
        // make it wait for the last opTime in the system when we wait for writeConcern.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return uassertStatusOK(DatabaseType::fromBSON(dbObj));
    }

    // The database does not exist. Insert an entry for the new database into the sharding catalog.

    // Pick a primary shard for the new database.
    const auto primaryShardId =
        uassertStatusOK(_selectShardForNewDatabase(opCtx, Grid::get(opCtx)->shardRegistry()));

    // Insert an entry for the new database into the sharding catalog.
    DatabaseType db(dbName, std::move(primaryShardId), false, databaseVersion::makeNew());

    log() << "Registering new database " << db << " in sharding catalog";

    uassertStatusOK(Grid::get(opCtx)->catalogClient()->insertConfigDocument(
        opCtx, DatabaseType::ConfigNS, db.toBSON(), ShardingCatalogClient::kLocalWriteConcern));

    return db;
}

void ShardingCatalogManager::enableSharding(OperationContext* opCtx, const std::string& dbName) {
    invariant(nsIsDbOnly(dbName));

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Enabling sharding on the admin database is not allowed",
            dbName != NamespaceString::kAdminDb);

    // Sharding is enabled automatically on the config db.
    if (dbName == NamespaceString::kConfigDb) {
        return;
    }

    // Creates the database if it doesn't exist and returns the new database entry, else returns the
    // existing database entry.
    auto dbType = createDatabase(opCtx, dbName);
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

    log() << "Enabling sharding for database [" << dbName << "] in config db";
    uassertStatusOK(Grid::get(opCtx)->catalogClient()->updateConfigDocument(
        opCtx,
        DatabaseType::ConfigNS,
        BSON(DatabaseType::name(dbName)),
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
        log() << "error committing movePrimary: " << dbname
              << causedBy(redact(updateStatus.getStatus()));
        return updateStatus.getStatus();
    }

    // If this assertion is tripped, it means that the request sent fine, but no documents were
    // updated. This is likely because the database version was changed in between the query and
    // the update, so no documents were found to change. This shouldn't happen however, because we
    // are holding the dist lock during the movePrimary operation.
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Tried to update primary shard for database '" << dbname
                          << " with version "
                          << currentDatabaseVersion.getLastMod(),
            updateStatus.getValue());

    // Ensure the next attempt to retrieve the database or any of its collections will do a full
    // reload
    Grid::get(opCtx)->catalogCache()->purgeDatabase(dbname);

    return Status::OK();
}

}  // namespace mongo
