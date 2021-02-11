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

#include "mongo/s/cluster_ddl.h"

#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {
namespace cluster {

CachedDatabaseInfo createDatabase(OperationContext* opCtx,
                                  StringData dbName,
                                  boost::optional<ShardId> suggestedPrimaryId) {
    auto catalogCache = Grid::get(opCtx)->catalogCache();

    auto dbStatus = catalogCache->getDatabase(opCtx, dbName);

    if (dbStatus == ErrorCodes::NamespaceNotFound) {
        ConfigsvrCreateDatabase request(dbName.toString());
        request.setDbName(NamespaceString::kAdminDb);
        if (suggestedPrimaryId)
            request.setPrimaryShardId(StringData(suggestedPrimaryId->toString()));

        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
        auto response = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            "admin",
            CommandHelpers::appendMajorityWriteConcern(request.toBSON({})),
            Shard::RetryPolicy::kIdempotent));
        uassertStatusOK(response.writeConcernStatus);
        uassertStatusOKWithContext(response.commandStatus,
                                   str::stream()
                                       << "Database " << dbName << " could not be created");

        auto createDbResponse = ConfigsvrCreateDatabaseResponse::parse(
            IDLParserErrorContext("configsvrCreateDatabaseResponse"), response.response);
        catalogCache->onStaleDatabaseVersion(
            dbName, DatabaseVersion(createDbResponse.getDatabaseVersion()));

        dbStatus = catalogCache->getDatabase(opCtx, dbName);
    }

    return uassertStatusOK(std::move(dbStatus));
}

}  // namespace cluster
}  // namespace mongo
