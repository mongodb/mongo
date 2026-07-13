// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <vector>

namespace mongo {
namespace sharding_util {

/**
 * Sends _flushRoutingTableCacheUpdatesWithWriteConcern to a list of shards. Throws if one of the
 * shards fails to refresh.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void tellShardsToRefreshCollection(
    OperationContext* opCtx,
    const std::vector<ShardId>& shardIds,
    const NamespaceString& nss,
    const std::shared_ptr<executor::TaskExecutor>& executor);

/**
 * Sends _flushRoutingTableCacheUpdatesWithWriteConcern to a list of shards. Does not wait for or
 * check the responses from the shards.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void triggerFireAndForgetShardRefreshes(
    OperationContext* opCtx, const std::vector<ShardId>& shardIds, const NamespaceString& nss);

/**
 * Process the responses received from a set of requests sent to the shards. If `throwOnError=true`,
 * throws in case one of the commands fails.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] std::vector<AsyncRequestsSender::Response> processShardResponses(
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
[[MONGO_MOD_NEEDS_REPLACEMENT]] std::vector<AsyncRequestsSender::Response> sendCommandToShards(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& command,
    const std::vector<ShardId>& shardIds,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    bool throwOnError = true);

/**
 * Helper function to create an index on a collection locally.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status createIndexOnCollection(OperationContext* opCtx,
                                                               const NamespaceString& ns,
                                                               const BSONObj& keys,
                                                               bool unique);
/**
 * Helper function to send a command to one shard
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void invokeCommandOnShardWithIdempotentRetryPolicy(
    OperationContext* opCtx,
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
[[MONGO_MOD_NEEDS_REPLACEMENT]] void retryIdempotentWorkAsPrimaryUntilSuccessOrStepdown(
    OperationContext* opCtx,
    std::string_view taskDescription,
    std::function<void(OperationContext*)> doWork,
    boost::optional<Backoff> backoff = boost::none);

/**
 * Selects the shard with the least amount of data by checking the total size of each shard in the
 * shard registry. Considers only shards that are not currently draining. Will return ShardNotFound
 * if no shard is found.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] ShardId selectLeastLoadedNonDrainingShard(OperationContext* opCtx);

/**
 * Returns true if 'bucketNss' is a tracked timeseries buckets collection, i.e. it has a sharding
 * catalog entry with timeseries fields. Returns false if the namespace is not tracked.
 */
[[MONGO_MOD_PUBLIC]] bool isTrackedTimeseries(OperationContext* opCtx,
                                              const NamespaceString& bucketNss);

/**
 * Returns true iff the MaxKey detection scans should run, i.e. if either the enableMaxKeyDetection
 * server parameter or featureFlagMaxKeyDetection is enabled.
 */
[[MONGO_MOD_PUBLIC]] bool isMaxKeyDetectionEnabled();
}  // namespace sharding_util
}  // namespace mongo
