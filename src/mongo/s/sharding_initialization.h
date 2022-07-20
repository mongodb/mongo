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

#pragma once

#include <boost/optional.hpp>
#include <functional>
#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/s/client/shard_registry.h"

namespace mongo {

class CatalogCache;
class ConnectionString;
class OperationContext;
class ShardFactory;
class Status;
class ShardingCatalogClient;

namespace executor {
class NetworkInterface;
class TaskExecutor;
}  // namespace executor

namespace rpc {
class EgressMetadataHook;
using ShardingEgressMetadataHookBuilder = std::function<std::unique_ptr<EgressMetadataHook>()>;
}  // namespace rpc

/**
 * Constructs a TaskExecutor which contains the required configuration for the sharding subsystem.
 */
std::unique_ptr<executor::TaskExecutor> makeShardingTaskExecutor(
    std::unique_ptr<executor::NetworkInterface> net);

/**
 * Initializes the global ShardingCatalogClient, ShardingCatalogManager, and Grid objects.
 */
Status initializeGlobalShardingState(OperationContext* opCtx,
                                     std::unique_ptr<CatalogCache> catalogCache,
                                     std::unique_ptr<ShardRegistry> shardRegistry,
                                     rpc::ShardingEgressMetadataHookBuilder hookBuilder,
                                     boost::optional<size_t> taskExecutorPoolSize);

/**
 * Loads global settings from config server such as cluster ID and default write concern.
 */

Status loadGlobalSettingsFromConfigServer(OperationContext* opCtx);

/**
 * Pre-caches mongod routing info for the calling process.
 */

void preCacheMongosRoutingInfo(OperationContext* opCtx);

/**
 * Warms up connections to shards with best effort strategy.
 */

Status preWarmConnectionPool(OperationContext* opCtx);

}  // namespace mongo
