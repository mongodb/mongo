// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/create_database_util.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/util/pcre_util.h"

#include <string_view>

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
        uassert(ErrorCodes::BadValue,
                str::stream() << "invalid shard name: " << *optPrimaryShard,
                optPrimaryShard->isValid());
        return uassertStatusOK(Grid::get(opCtx)->shardRegistry()->resolveShardId(
            opCtx, *optPrimaryShard, true /* allowNonShardIdIdentifiers */));
    }
    return boost::none;
}

BSONObj constructDbMatchFilterExact(std::string_view dbNameStr,
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
    std::string_view dbNameStr,
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

BSONObj constructDbMatchFilterCaseInsensitive(std::string_view dbNameStr) {
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
