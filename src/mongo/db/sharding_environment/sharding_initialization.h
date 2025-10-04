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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/keys_collection_client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/rpc/metadata/metadata_hook.h"

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
std::shared_ptr<executor::TaskExecutor> makeShardingTaskExecutor(
    std::unique_ptr<executor::NetworkInterface> net);

std::unique_ptr<rpc::EgressMetadataHookList> makeShardingEgressHooksList(ServiceContext* service);

/**
 * Initializes the global ShardingCatalogClient, ShardingCatalogManager, and Grid objects.
 */
Status initializeGlobalShardingState(
    OperationContext* opCtx,
    std::unique_ptr<CatalogCache> catalogCache,
    std::unique_ptr<ShardRegistry> shardRegistry,
    rpc::ShardingEgressMetadataHookBuilder hookBuilder,
    boost::optional<size_t> taskExecutorPoolSize,
    std::function<std::unique_ptr<KeysCollectionClient>(ShardingCatalogClient*)> initKeysClient);

/**
 * Loads global settings from config server such as cluster ID and default write concern.
 */

Status loadGlobalSettingsFromConfigServer(OperationContext* opCtx,
                                          ShardingCatalogClient* catalogClient);

/**
 * Pre-caches mongod routing info for the calling process.
 */

void preCacheMongosRoutingInfo(OperationContext* opCtx);

/**
 * Warms up connections to shards with best effort strategy.
 */

Status preWarmConnectionPool(OperationContext* opCtx);

}  // namespace mongo
