// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace cluster {

/**
 * Creates (or ensures that it is created) a database `dbName`, with `suggestedPrimaryId` as the
 * primary node.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] CachedDatabaseInfo createDatabase(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const boost::optional<ShardId>& suggestedPrimaryId = boost::none);

/**
 * Creates the specified collection.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] CreateCollectionResponse createCollection(
    OperationContext* opCtx, ShardsvrCreateCollection request);

/**
 * Creates a collection with the options specified in `request`. Calls the above createCollection
 * function within a router loop.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void createCollectionWithRouterLoop(
    OperationContext* opCtx, const ShardsvrCreateCollection& request);


/**
 * Creates the specified nss as an unsharded collection. Calls the above
 * createCollectionWithRouterLoop function.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void createCollectionWithRouterLoop(OperationContext* opCtx,
                                                                    const NamespaceString& nss);

/**
 * Returns the only allowed `shardCollection` request for `config.system.sessions`
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] ShardsvrCreateCollection shardLogicalSessionsCollectionRequest();

}  // namespace cluster
}  // namespace mongo
