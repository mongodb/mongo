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

#include "mongo/db/s/sharding_util.h"

#include <fmt/format.h>

#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/type_shard_collection.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/request_types/flush_routing_table_cache_updates_gen.h"

namespace mongo {
namespace sharding_util {

using namespace fmt::literals;

void tellShardsToRefreshCollection(OperationContext* opCtx,
                                   const std::vector<ShardId>& shardIds,
                                   const NamespaceString& nss,
                                   const std::shared_ptr<executor::TaskExecutor>& executor) {
    auto cmd = FlushRoutingTableCacheUpdatesWithWriteConcern(nss);
    cmd.setSyncFromConfig(true);
    cmd.setDbName(nss.db());
    auto cmdObj = CommandHelpers::appendMajorityWriteConcern(cmd.toBSON({}));
    sendCommandToShards(opCtx, NamespaceString::kAdminDb, cmdObj, shardIds, executor);
}

std::vector<AsyncRequestsSender::Response> sendCommandToShards(
    OperationContext* opCtx,
    StringData dbName,
    const BSONObj& command,
    const std::vector<ShardId>& shardIds,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    const bool throwOnError) {
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& shardId : shardIds) {
        requests.emplace_back(shardId, command);
    }

    std::vector<AsyncRequestsSender::Response> responses;
    if (!requests.empty()) {
        // The _flushRoutingTableCacheUpdatesWithWriteConcern command will fail with a
        // QueryPlanKilled error response if the config.cache.chunks collection is dropped
        // concurrently. The config.cache.chunks collection is dropped by the shard when it detects
        // the sharded collection's epoch having changed. We use kIdempotentOrCursorInvalidated so
        // the ARS automatically retries in that situation.
        AsyncRequestsSender ars(opCtx,
                                executor,
                                dbName,
                                requests,
                                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                Shard::RetryPolicy::kIdempotentOrCursorInvalidated,
                                nullptr /* resourceYielder */);

        while (!ars.done()) {
            // Retrieve the responses and throw at the first failure.
            auto response = ars.next();

            if (throwOnError) {
                const auto errorContext =
                    "Failed command {} for database '{}' on shard '{}'"_format(
                        command.toString(), dbName, StringData{response.shardId});

                uassertStatusOKWithContext(response.swResponse.getStatus(), errorContext);
                const auto& respBody = response.swResponse.getValue().data;

                const auto status = getStatusFromCommandResult(respBody);
                uassertStatusOKWithContext(status, errorContext);

                const auto wcStatus = getWriteConcernStatusFromCommandResult(respBody);
                uassertStatusOKWithContext(wcStatus, errorContext);
            }

            responses.push_back(std::move(response));
        }
    }
    return responses;
}

void downgradeCollectionBalancingFieldsToPre53(OperationContext* opCtx) {
    const NamespaceString collNss = [&]() {
        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
            return NamespaceString::kShardConfigCollectionsNamespace;
        } else if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            return CollectionType::ConfigNS;
        }
        MONGO_UNREACHABLE;
    }();

    write_ops::UpdateCommandRequest updateOp(collNss);
    updateOp.setUpdates({[&] {
        write_ops::UpdateOpEntry entry;
        BSONObjBuilder updateCmd;
        BSONObjBuilder unsetBuilder(updateCmd.subobjStart("$unset"));
        unsetBuilder.append(CollectionType::kMaxChunkSizeBytesFieldName, 0);
        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            unsetBuilder.append(CollectionType::kNoAutoSplitFieldName, 0);
        } else {
            unsetBuilder.append(ShardCollectionTypeBase::kAllowAutoSplitFieldName, 0);
        }
        unsetBuilder.doneFast();
        entry.setQ({});
        const BSONObj update = updateCmd.obj();
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
        entry.setUpsert(false);
        entry.setMulti(true);
        return entry;
    }()});

    DBDirectClient client(opCtx);
    client.update(updateOp);

    const WriteConcernOptions majorityWC{
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, Seconds(0)};
    WriteConcernResult ignoreResult;
    auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, majorityWC, &ignoreResult));
}

}  // namespace sharding_util
}  // namespace mongo
