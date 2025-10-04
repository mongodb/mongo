/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/create_database_util.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/util/pcre_util.h"

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace create_database_util {

void logCommitCreateDatabaseFailed(const DatabaseName& dbName, const std::string& reason) {
    LOGV2_DEBUG(8917900,
                1,
                "Commit create database failed",
                "dbName"_attr = dbName.toStringForErrorMsg(),
                "reason"_attr = reason);
}

boost::optional<DatabaseType> checkDbNameConstraints(const DatabaseName& dbName) {
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
            DatabaseName::isValid(dbName, DatabaseName::DollarInDbNameBehavior::Disallow));

    return boost::none;
}

boost::optional<ShardId> resolvePrimaryShard(OperationContext* opCtx,
                                             const boost::optional<ShardId>& optPrimaryShard) {
    if (optPrimaryShard) {
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        uassert(ErrorCodes::BadValue,
                str::stream() << "invalid shard name: " << *optPrimaryShard,
                optPrimaryShard->isValid());
        return uassertStatusOK(shardRegistry->getShard(opCtx, *optPrimaryShard))->getId();
    }
    return boost::none;
}

BSONObj constructDbMatchFilterExact(StringData dbNameStr,
                                    const boost::optional<ShardId>& optResolvedPrimaryShard) {
    BSONObjBuilder filterBuilder;
    filterBuilder.append(DatabaseType::kDbNameFieldName, dbNameStr);
    if (optResolvedPrimaryShard) {
        filterBuilder.append(DatabaseType::kPrimaryFieldName, *optResolvedPrimaryShard);
    }
    return filterBuilder.obj();
}

boost::optional<DatabaseType> findDatabaseExactMatch(
    OperationContext* opCtx,
    StringData dbNameStr,
    const boost::optional<ShardId>& optResolvedPrimaryShard) {
    const auto dbMatchFilterExact = constructDbMatchFilterExact(dbNameStr, optResolvedPrimaryShard);

    DBDirectClient client(opCtx);
    auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
    auto dbObj = client.findOne(NamespaceString::kConfigDatabasesNamespace, dbMatchFilterExact);
    if (!dbObj.isEmpty()) {
        replClient.setLastOpToSystemLastOpTime(opCtx);
        return DatabaseType::parse(dbObj, IDLParserContext("DatabaseType"));
    }
    return boost::none;
}

BSONObj constructDbMatchFilterCaseInsensitive(StringData dbNameStr) {
    BSONObjBuilder filterBuilder;
    filterBuilder.appendRegex(
        DatabaseType::kDbNameFieldName, fmt::format("^{}$", pcre_util::quoteMeta(dbNameStr)), "i");
    return filterBuilder.obj();
}

void checkAgainstExistingDbDoc(const DatabaseType& existingDb,
                               const DatabaseName& dbName,
                               const boost::optional<ShardId>& optResolvedPrimaryShard) {
    uassert(ErrorCodes::DatabaseDifferCase,
            str::stream() << "can't have 2 databases that just differ on case "
                          << " have: " << existingDb.getDbName().toStringForErrorMsg()
                          << " want to add: " << dbName.toStringForErrorMsg(),
            existingDb.getDbName() == dbName);

    uassert(ErrorCodes::NamespaceExists,
            str::stream() << "database already created on a primary which is different from "
                          << optResolvedPrimaryShard,
            !optResolvedPrimaryShard || *optResolvedPrimaryShard == existingDb.getPrimary());
}

boost::optional<DatabaseType> checkForExistingDatabaseWithDifferentOptions(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const boost::optional<ShardId>& optResolvedPrimaryShard) {
    DBDirectClient client(opCtx);
    auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
    const auto dbNameStr =
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());
    // Check if a database already exists with the same name (case insensitive), and if so, return
    // the existing entry.
    const auto dbMatchFilterCaseInsensitive = constructDbMatchFilterCaseInsensitive(dbNameStr);

    if (auto dbDoc = client.findOne(NamespaceString::kConfigDatabasesNamespace,
                                    dbMatchFilterCaseInsensitive);
        !dbDoc.isEmpty()) {
        auto existingDb = DatabaseType::parse(dbDoc, IDLParserContext("DatabaseType"));
        checkAgainstExistingDbDoc(existingDb, dbName, optResolvedPrimaryShard);

        // We did a local read of the database entry above and found that the database already
        // exists. However, the data may not be majority committed (a previous createDatabase
        // attempt may have failed with a writeConcern error).
        // Since the current Client doesn't know the opTime of the last write to the database
        // entry, make it wait for the last opTime in the system when we wait for writeConcern.
        replClient.setLastOpToSystemLastOpTime(opCtx);

        return existingDb;
    }

    return boost::none;
}

bool checkIfDropPendingDB(OperationContext* opCtx, const DatabaseName& dbName) {
    const auto dbMatchFilterCaseInsensitive = constructDbMatchFilterCaseInsensitive(
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault()));

    DBDirectClient client(opCtx);
    return client
        .findOne(NamespaceString::kConfigDropPendingDBsNamespace, dbMatchFilterCaseInsensitive)
        .isEmpty();
}

ShardId getCandidatePrimaryShard(OperationContext* opCtx,
                                 const boost::optional<ShardId>& optResolvedPrimaryShard) {
    return optResolvedPrimaryShard ? *optResolvedPrimaryShard
                                   : sharding_util::selectLeastLoadedNonDrainingShard(opCtx);
}

void refreshDbVersionOnPrimaryShard(OperationContext* opCtx,
                                    const std::string& dbNameStr,
                                    const ShardId& primaryShard) {
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto primaryShardPtr = uassertStatusOK(shardRegistry->getShard(opCtx, primaryShard));
    auto cmdResponse = uassertStatusOK(
        primaryShardPtr->runCommand(opCtx,
                                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                    DatabaseName::kAdmin,
                                    BSON("_flushDatabaseCacheUpdates" << dbNameStr),
                                    Shard::RetryPolicy::kIdempotent));
    uassertStatusOK(cmdResponse.commandStatus);
}

}  // namespace create_database_util
}  // namespace mongo
