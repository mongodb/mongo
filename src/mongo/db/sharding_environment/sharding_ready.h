// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Used on config and shard servers to determine if the sharded cluster has at least one shard.
 *
 * On a shard server primary or secondary, ShardingReady is always set on startup/restart because
 * there is guaranteed to always be at least one shard (either the config shard or a dedicated shard
 * server).
 *
 * On a config server primary or secondary, ShardingReady is set on startup/restart if there is at
 * least one shard in config.shards. If there are no shards at startup, that means that the
 * config server is not part of a sharded cluster and is still auto-bootstrapping into a config
 * shard. In that case, ShardingReady is set when the config server becomes a config shard.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardingReady {

    ShardingReady(const ShardingReady&) = delete;
    ShardingReady& operator=(const ShardingReady&) = delete;

public:
    ShardingReady() = default;
    ~ShardingReady() = default;

    static ShardingReady* get(ServiceContext* serviceContext);
    static ShardingReady* get(OperationContext* opCtx);

    void scheduleTransitionToConfigShard(OperationContext* opCtx);
    void waitUntilReady(OperationContext* opCtx);
    bool isReady();
    SharedSemiFuture<void> isReadyFuture() const;

    /**
     * Sets a value for the _isReady promise indicating that the sharding system is ready to start
     * performing operations. This function is idempotent and will not set a value for the _isReady
     * promise if one has already been placed.
     */
    void setIsReady();

    /**
     * Calls setIsReady() only if a shard exists in the sharded cluster. This can only be called
     * from a config server because the function reads from the local config server database. This
     * function is only called once at startup.
     */
    void setIsReadyIfShardExists(OperationContext* opCtx);

private:
    void _transitionToConfigShard(ServiceContext* serviceContext);

    // Protects _isReady.
    mutable std::mutex _mutex;
    SharedPromise<void> _isReady;
};
}  // namespace mongo
