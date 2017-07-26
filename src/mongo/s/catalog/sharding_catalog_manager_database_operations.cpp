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

#include "mongo/s/catalog/sharding_catalog_manager.h"

#include <pcrecpp.h>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;
using std::vector;

namespace {

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});

}  // namespace

Status ShardingCatalogManager::createDatabase(OperationContext* opCtx, const std::string& dbName) {
    invariant(nsIsDbOnly(dbName));

    // The admin and config databases should never be explicitly created. They "just exist",
    // i.e. getDatabase will always return an entry for them.
    if (dbName == "admin" || dbName == "config") {
        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << "cannot manually create database '" << dbName << "'");
    }

    // Lock the database globally to prevent conflicts with simultaneous database creation.
    auto scopedDistLock = Grid::get(opCtx)->catalogClient()->getDistLockManager()->lock(
        opCtx, dbName, "createDatabase", DistLockManager::kDefaultLockTimeout);
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    // check for case sensitivity violations
    Status status = _checkDbDoesNotExist(opCtx, dbName, nullptr);
    if (!status.isOK()) {
        if (status.code() == ErrorCodes::NamespaceExists) {
            return Status::OK();
        }
        return status;
    }

    // Database does not exist, pick a shard and create a new entry
    auto newShardIdStatus = _selectShardForNewDatabase(opCtx, Grid::get(opCtx)->shardRegistry());
    if (!newShardIdStatus.isOK()) {
        return newShardIdStatus.getStatus();
    }

    const ShardId& newShardId = newShardIdStatus.getValue();

    log() << "Placing [" << dbName << "] on: " << newShardId;

    DatabaseType db;
    db.setName(dbName);
    db.setPrimary(newShardId);
    db.setSharded(false);

    status = Grid::get(opCtx)->catalogClient()->insertConfigDocument(
        opCtx, DatabaseType::ConfigNS, db.toBSON(), ShardingCatalogClient::kMajorityWriteConcern);
    if (status.code() == ErrorCodes::DuplicateKey) {
        return Status(ErrorCodes::NamespaceExists, "database " + dbName + " already exists");
    }

    return status;
}

Status ShardingCatalogManager::enableSharding(OperationContext* opCtx, const std::string& dbName) {
    invariant(nsIsDbOnly(dbName));

    if (dbName == NamespaceString::kConfigDb || dbName == NamespaceString::kAdminDb) {
        return {
            ErrorCodes::IllegalOperation,
            str::stream() << "Enabling sharding on system configuration databases is not allowed"};
    }

    // Lock the database globally to prevent conflicts with simultaneous database
    // creation/modification.
    auto scopedDistLock = Grid::get(opCtx)->catalogClient()->getDistLockManager()->lock(
        opCtx, dbName, "enableSharding", DistLockManager::kDefaultLockTimeout);
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus();
    }

    // Check for case sensitivity violations
    DatabaseType db;
    Status status = _checkDbDoesNotExist(opCtx, dbName, &db);
    if (status.isOK()) {
        // Database does not exist, create a new entry
        auto newShardIdStatus =
            _selectShardForNewDatabase(opCtx, Grid::get(opCtx)->shardRegistry());
        if (!newShardIdStatus.isOK()) {
            return newShardIdStatus.getStatus();
        }

        const ShardId& newShardId = newShardIdStatus.getValue();

        log() << "Placing [" << dbName << "] on: " << newShardId;

        db.setName(dbName);
        db.setPrimary(newShardId);
        db.setSharded(true);
    } else if (status.code() == ErrorCodes::NamespaceExists) {
        if (db.getSharded()) {
            return Status::OK();
        }

        // Database exists, so just update it
        db.setSharded(true);
    } else {
        return status;
    }

    log() << "Enabling sharding for database [" << dbName << "] in config db";

    return Grid::get(opCtx)->catalogClient()->updateDatabase(opCtx, dbName, db);
}

Status ShardingCatalogManager::_checkDbDoesNotExist(OperationContext* opCtx,
                                                    const std::string& dbName,
                                                    DatabaseType* db) {
    BSONObjBuilder queryBuilder;
    queryBuilder.appendRegex(
        DatabaseType::name(), (string) "^" + pcrecpp::RE::QuoteMeta(dbName) + "$", "i");

    auto findStatus = Grid::get(opCtx)->catalogClient()->_exhaustiveFindOnConfig(
        opCtx,
        kConfigReadSelector,
        repl::ReadConcernLevel::kMajorityReadConcern,
        NamespaceString(DatabaseType::ConfigNS),
        queryBuilder.obj(),
        BSONObj(),
        1);

    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue().value;
    if (docs.empty()) {
        return Status::OK();
    }

    BSONObj dbObj = docs.front();
    std::string actualDbName = dbObj[DatabaseType::name()].String();
    if (actualDbName == dbName) {
        if (db) {
            auto parseDBStatus = DatabaseType::fromBSON(dbObj);
            if (!parseDBStatus.isOK()) {
                return parseDBStatus.getStatus();
            }

            *db = parseDBStatus.getValue();
        }

        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "database " << dbName << " already exists");
    }

    return Status(ErrorCodes::DatabaseDifferCase,
                  str::stream() << "can't have 2 databases that just differ on case "
                                << " have: "
                                << actualDbName
                                << " want to add: "
                                << dbName);
}

Status ShardingCatalogManager::getDatabasesForShard(OperationContext* opCtx,
                                                    const ShardId& shardId,
                                                    std::vector<std::string>* dbs) {
    auto findStatus = Grid::get(opCtx)->catalogClient()->_exhaustiveFindOnConfig(
        opCtx,
        kConfigReadSelector,
        repl::ReadConcernLevel::kLocalReadConcern,
        NamespaceString(DatabaseType::ConfigNS),
        BSON(DatabaseType::primary(shardId.toString())),
        BSONObj(),
        boost::none);  // no limit

    for (const BSONObj& obj : findStatus.getValue().value) {
        std::string dbName;
        Status status = bsonExtractStringField(obj, DatabaseType::name(), &dbName);
        if (!status.isOK()) {
            dbs->clear();
            return status;
        }

        dbs->push_back(dbName);
    }

    return Status::OK();
}

}  // namespace mongo
