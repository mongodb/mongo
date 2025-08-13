/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/future.h"

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
class ShardingReady {

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
    mutable stdx::mutex _mutex;
    SharedPromise<void> _isReady;
};
}  // namespace mongo
