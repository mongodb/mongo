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


#include "mongo/db/s/sessions_collection_config_server.h"

#include "mongo/logv2/log.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/stale_shard_version_helpers.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

void SessionsCollectionConfigServer::_shardCollectionIfNeeded(OperationContext* opCtx) {
    // First, check if the collection is already sharded.
    try {
        checkSessionsCollectionExists(opCtx);
        return;
    } catch (const DBException&) {
        // If the sessions collection doesn't exist, create it
    }

    // If we don't have any shards, we can't set up this collection yet.
    uassert(ErrorCodes::ShardNotFound,
            str::stream() << "Failed to create " << NamespaceString::kLogicalSessionsNamespace
                          << ": cannot create the collection until there are shards",
            Grid::get(opCtx)->shardRegistry()->getNumShards(opCtx) != 0);

    ShardsvrCreateCollection shardsvrCollRequest(NamespaceString::kLogicalSessionsNamespace);
    CreateCollectionRequest requestParamsObj;
    requestParamsObj.setShardKey(BSON("_id" << 1));
    shardsvrCollRequest.setCreateCollectionRequest(std::move(requestParamsObj));
    shardsvrCollRequest.setDbName(NamespaceString::kLogicalSessionsNamespace.db());

    cluster::createCollection(opCtx, shardsvrCollRequest);
}

void SessionsCollectionConfigServer::_generateIndexesIfNeeded(OperationContext* opCtx) {
    const auto nss = NamespaceString::kLogicalSessionsNamespace;
    auto shardResults = shardVersionRetry(
        opCtx,
        Grid::get(opCtx)->catalogCache(),
        nss,
        "SessionsCollectionConfigServer::_generateIndexesIfNeeded",
        [&] {
            const ChunkManager cm = [&]() {
                // (SERVER-61214) wait for the catalog cache to acknowledge that the sessions
                // collection is sharded in order to be sure to get a valid routing table
                while (true) {
                    auto cm = uassertStatusOK(
                        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx,
                                                                                              nss));

                    if (cm.isSharded()) {
                        return cm;
                    }
                }
            }();

            return scatterGatherVersionedTargetByRoutingTable(
                opCtx,
                nss.db(),
                nss,
                cm,
                SessionsCollection::generateCreateIndexesCmd(),
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kNoRetry,
                BSONObj() /* query */,
                BSONObj() /* collation */);
        });

    for (auto& shardResult : shardResults) {
        const auto shardResponse = uassertStatusOK(std::move(shardResult.swResponse));
        const auto& res = shardResponse.data;
        uassertStatusOK(getStatusFromCommandResult(res));
    }
}

void SessionsCollectionConfigServer::setupSessionsCollection(OperationContext* opCtx) {
    // If the sharding state is not yet initialized, fail.
    uassert(ErrorCodes::ShardingStateNotInitialized,
            "sharding state is not yet initialized",
            Grid::get(opCtx)->isShardingInitialized());

    stdx::lock_guard<Latch> lk(_mutex);

    _shardCollectionIfNeeded(opCtx);
    _generateIndexesIfNeeded(opCtx);
    static constexpr int64_t kAverageSessionDocSizeBytes = 200;
    static constexpr int64_t kDesiredDocsInChunks = 1000;
    static constexpr int64_t kMaxChunkSizeBytes =
        kAverageSessionDocSizeBytes * kDesiredDocsInChunks;
    auto filterQuery =
        BSON("_id" << NamespaceString::kLogicalSessionsNamespace.ns()
                   << CollectionType::kMaxChunkSizeBytesFieldName << BSON("$exists" << false));
    auto updateQuery = BSON("$set" << BSON(CollectionType::kMaxChunkSizeBytesFieldName
                                           << kMaxChunkSizeBytes
                                           << CollectionType::kNoAutoSplitFieldName << true));

    uassertStatusOK(Grid::get(opCtx)->catalogClient()->updateConfigDocument(
        opCtx,
        CollectionType::ConfigNS,
        filterQuery,
        updateQuery,
        false,
        ShardingCatalogClient::kMajorityWriteConcern));
}

}  // namespace mongo
