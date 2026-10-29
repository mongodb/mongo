// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/oid.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/keys_collection_client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <functional>
#include <memory>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class CatalogCache;
class ConnectionString;
class ShardFactory;
class Status;
class ShardingCatalogClient;

namespace executor {
class NetworkInterface;
class TaskExecutor;
}  // namespace executor

namespace rpc {
using ShardingEgressMetadataHookBuilder = std::function<std::unique_ptr<EgressMetadataHook>()>;
}  // namespace rpc

/**
 * Constructs a TaskExecutor which contains the required configuration for the sharding subsystem.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] std::shared_ptr<executor::TaskExecutor> makeShardingTaskExecutor(
    std::unique_ptr<executor::NetworkInterface> net);

[[MONGO_MOD_NEEDS_REPLACEMENT]] std::unique_ptr<rpc::EgressMetadataHookList>
makeShardingEgressHooksList(ServiceContext* service);

/**
 * Initializes the global ShardingCatalogClient, ShardingCatalogManager, and Grid objects.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status initializeGlobalShardingState(
    OperationContext* opCtx,
    std::unique_ptr<CatalogCache> catalogCache,
    std::unique_ptr<ShardRegistry> shardRegistry,
    rpc::ShardingEgressMetadataHookBuilder hookBuilder,
    boost::optional<size_t> taskExecutorPoolSize,
    std::function<std::unique_ptr<KeysCollectionClient>(ShardingCatalogClient*)> initKeysClient);

/**
 * Loads global settings from config server such as cluster ID and default write concern.
 */

[[MONGO_MOD_NEEDS_REPLACEMENT]] Status loadGlobalSettingsFromConfigServer(
    OperationContext* opCtx, ShardingCatalogClient* catalogClient);

/**
 * Pre-caches mongod routing info for the calling process.
 */

[[MONGO_MOD_NEEDS_REPLACEMENT]] void preCacheMongosRoutingInfo(OperationContext* opCtx);

/**
 * Warms up connections to shards with best effort strategy.
 */

[[MONGO_MOD_NEEDS_REPLACEMENT]] Status preWarmConnectionPool(OperationContext* opCtx);

}  // namespace mongo
