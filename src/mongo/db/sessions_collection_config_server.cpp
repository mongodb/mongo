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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/db/sessions_collection_config_server.h"

#include "mongo/client/query.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/shard_collection_gen.h"
#include "mongo/util/log.h"

namespace mongo {

// Returns an error if the collection didn't exist and we couldn't
// shard it into existence, either.
Status SessionsCollectionConfigServer::_shardCollectionIfNeeded(OperationContext* opCtx) {
    // First, check if the collection is already sharded.
    auto res = _checkCacheForSessionsCollection(opCtx);
    if (res.isOK()) {
        return res;
    }

    // If we don't have any shards, we can't set up this collection yet.
    if (Grid::get(opCtx)->shardRegistry()->getNumShards() == 0) {
        return {ErrorCodes::ShardNotFound,
                "Cannot create config.system.sessions until there are shards"};
    }

    // First, shard the sessions collection to create it.
    ConfigsvrShardCollectionRequest shardCollection;
    shardCollection.set_configsvrShardCollection(NamespaceString::kLogicalSessionsNamespace);
    shardCollection.setKey(BSON("_id" << 1));

    DBDirectClient client(opCtx);
    BSONObj info;
    if (!client.runCommand(
            "admin", CommandHelpers::appendMajorityWriteConcern(shardCollection.toBSON()), info)) {
        return getStatusFromCommandResult(info);
    }

    return Status::OK();
}

Status SessionsCollectionConfigServer::_generateIndexesIfNeeded(OperationContext* opCtx) {
    try {
        scatterGatherOnlyVersionIfUnsharded(opCtx,
                                            NamespaceString::kLogicalSessionsNamespace,
                                            SessionsCollection::generateCreateIndexesCmd(),
                                            ReadPreferenceSetting::get(opCtx),
                                            Shard::RetryPolicy::kNoRetry);
        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

Status SessionsCollectionConfigServer::setupSessionsCollection(OperationContext* opCtx) {
    // If the sharding state is not yet initialized, fail.
    if (!Grid::get(opCtx)->isShardingInitialized()) {
        return {ErrorCodes::ShardingStateNotInitialized, "sharding state is not yet initialized"};
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    {
        auto res = _shardCollectionIfNeeded(opCtx);
        if (!res.isOK()) {
            log() << "Failed to create config.system.sessions: " << res.reason()
                  << ", will try again at the next refresh interval";
            return res;
        }

        res = _generateIndexesIfNeeded(opCtx);
        if (!res.isOK()) {
            log() << "Failed to generate TTL index for config.system.sessions on all shards, "
                  << "will try again on the next refresh interval";
        }

        return res;
    }
}

Status SessionsCollectionConfigServer::checkSessionsCollectionExists(OperationContext* opCtx) {
    return _checkCacheForSessionsCollection(opCtx);
}

}  // namespace mongo
