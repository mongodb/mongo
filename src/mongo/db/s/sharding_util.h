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

#pragma once

#include <memory>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace sharding_util {

/**
 * Sends _flushRoutingTableCacheUpdatesWithWriteConcern to a list of shards. Throws if one of the
 * shards fails to refresh.
 */
void tellShardsToRefreshCollection(OperationContext* opCtx,
                                   const std::vector<ShardId>& shardIds,
                                   const NamespaceString& nss,
                                   const std::shared_ptr<executor::TaskExecutor>& executor);

/**
 * Sends _flushRoutingTableCacheUpdatesWithWriteConcern to a list of shards. Does not wait for or
 * check the responses from the shards.
 */
void triggerFireAndForgetShardRefreshes(OperationContext* opCtx,
                                        const std::vector<ShardId>& shardIds,
                                        const NamespaceString& nss);

/**
 * Process the responses received from a set of requests sent to the shards. If `throwOnError=true`,
 * throws in case one of the commands fails.
 */
std::vector<AsyncRequestsSender::Response> processShardResponses(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& command,
    const std::vector<AsyncRequestsSender::Request>& requests,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    bool throwOnError);

/**
 * Generic utility to send a command to a list of shards. If `throwOnError=true`, throws in case one
 * of the commands fails.
 */
std::vector<AsyncRequestsSender::Response> sendCommandToShards(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& command,
    const std::vector<ShardId>& shardIds,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    bool throwOnError = true);

/**
 * Generic utility to send a command to a list of shards attaching the shard version to the request.
 * If `throwOnError=true`, throws in case one of the commands fails.
 */
std::vector<AsyncRequestsSender::Response> sendCommandToShardsWithVersion(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& command,
    const std::vector<ShardId>& shardIds,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    const CollectionRoutingInfo& cri,
    bool throwOnError = true);

/**
 * Creates the necessary indexes for the sharding index catalog collections.
 */
Status createShardingIndexCatalogIndexes(OperationContext* opCtx,
                                         const NamespaceString& indexCatalogNamespace);

/**
 * Creates the necessary indexes for the collections collection.
 */
Status createShardCollectionCatalogIndexes(OperationContext* opCtx);

/**
 * Helper function to create an index on a collection locally.
 */
Status createIndexOnCollection(OperationContext* opCtx,
                               const NamespaceString& ns,
                               const BSONObj& keys,
                               bool unique);
/**
 * Helper function to send a command to one shard
 */
void invokeCommandOnShardWithIdempotentRetryPolicy(OperationContext* opCtx,
                                                   const ShardId& recipientId,
                                                   const DatabaseName& dbName,
                                                   const BSONObj& cmd);

/**
 * Runs doWork until it doesn't throw an error, the node is shutting down, the node has stepped
 * down, or the node has stepped down and up.
 *
 * Note that it is not guaranteed that 'doWork' will not be executed while the node is secondary
 * or after the node has stepped down and up, only that 'doWork' will eventually stop being retried
 * if one of those events has happened.
 *
 * Requirements:
 * - doWork must be idempotent.
 */
void retryIdempotentWorkAsPrimaryUntilSuccessOrStepdown(
    OperationContext* opCtx,
    StringData taskDescription,
    std::function<void(OperationContext*)> doWork,
    boost::optional<Backoff> backoff = boost::none);
}  // namespace sharding_util
}  // namespace mongo
