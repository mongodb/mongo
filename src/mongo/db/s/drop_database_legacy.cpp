/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/drop_database_legacy.h"

#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/s/drop_collection_legacy.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

void dropDatabaseFromShard(OperationContext* opCtx, const ShardId& shardId, StringData dbName) {
    const auto dropDatabaseCommandBSON = [opCtx] {
        BSONObjBuilder builder;
        builder.append("dropDatabase", 1);
        builder.append(WriteConcernOptions::kWriteConcernField, opCtx->getWriteConcern().toBSON());
        return builder.obj();
    }();

    const auto shard = uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
    auto cmdDropDatabaseResult = uassertStatusOK(
        shard->runCommandWithFixedRetryAttempts(opCtx,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                dbName.toString(),
                                                dropDatabaseCommandBSON,
                                                Shard::RetryPolicy::kIdempotent));

    uassertStatusOK(cmdDropDatabaseResult.commandStatus);
    uassertStatusOK(cmdDropDatabaseResult.writeConcernStatus);
}

}  // namespace

void dropDatabaseLegacy(OperationContext* opCtx, StringData dbName) {
    auto dbDistLock = uassertStatusOK(DistLockManager::get(opCtx)->lock(
        opCtx, dbName, "dropDatabase", DistLockManager::kDefaultLockTimeout));

    ON_BLOCK_EXIT([&] { Grid::get(opCtx)->catalogCache()->purgeDatabase(dbName); });

    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer &&
        dbName != NamespaceString::kConfigDb) {
        DatabaseShardingState::checkIsPrimaryShardForDb(opCtx, dbName);
    }

    auto const catalogClient = Grid::get(opCtx)->catalogClient();

    DatabaseType dbType;
    try {
        dbType =
            catalogClient->getDatabase(opCtx, dbName, repl::ReadConcernLevel::kMajorityReadConcern);
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        return;
    }

    uassertStatusOK(ShardingLogging::get(opCtx)->logChangeChecked(
        opCtx,
        "dropDatabase.start",
        dbName,
        BSONObj(),
        ShardingCatalogClient::kMajorityWriteConcern));

    // Drop the database's collections.
    for (const auto& nss : catalogClient->getAllShardedCollectionsForDb(
             opCtx, dbName, repl::ReadConcernLevel::kMajorityReadConcern)) {
        auto collDistLock = uassertStatusOK(DistLockManager::get(opCtx)->lock(
            opCtx, nss.ns(), "dropCollection", DistLockManager::kDefaultLockTimeout));
        dropCollectionNoDistLock(opCtx, nss);
    }

    // Drop the database from the primary shard first.
    dropDatabaseFromShard(opCtx, dbType.getPrimary(), dbName);

    // Drop the database from each of the remaining shards.
    const auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIdsNoReload();
    for (const ShardId& shardId : allShardIds) {
        dropDatabaseFromShard(opCtx, shardId, dbName);
    }

    // Remove the database entry from the metadata.
    const Status status =
        catalogClient->removeConfigDocuments(opCtx,
                                             DatabaseType::ConfigNS,
                                             BSON(DatabaseType::name(dbName.toString())),
                                             ShardingCatalogClient::kMajorityWriteConcern);
    uassertStatusOKWithContext(
        status, str::stream() << "Could not remove database '" << dbName << "' from metadata");

    // Send _flushDatabaseCacheUpdates to all shards
    IgnoreAPIParametersBlock ignoreApiParametersBlock{opCtx};
    for (const ShardId& shardId : allShardIds) {
        const auto shard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
        auto cmdResponse = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            BSON("_flushDatabaseCacheUpdates" << dbName),
            Shard::RetryPolicy::kIdempotent));
        uassertStatusOK(cmdResponse.commandStatus);
    }

    ShardingLogging::get(opCtx)->logChange(
        opCtx, "dropDatabase", dbName, BSONObj(), ShardingCatalogClient::kMajorityWriteConcern);
}

}  // namespace mongo
