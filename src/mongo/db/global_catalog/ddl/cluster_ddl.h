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

#include "mongo/base/string_data.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/shard_id.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace cluster {

/**
 * Creates (or ensures that it is created) a database `dbName`, with `suggestedPrimaryId` as the
 * primary node.
 */
CachedDatabaseInfo createDatabase(OperationContext* opCtx,
                                  const DatabaseName& dbName,
                                  const boost::optional<ShardId>& suggestedPrimaryId = boost::none);

/**
 * Creates the specified collection.
 *
 * TODO (SERVER-100309): remove `againstFirstShard` once 9.0 becomes last LTS.
 */
CreateCollectionResponse createCollection(OperationContext* opCtx,
                                          ShardsvrCreateCollection request,
                                          bool againstFirstShard = false);

/**
 * Creates a collection with the options specified in `request`. Calls the above createCollection
 * function within a router loop.
 */
void createCollectionWithRouterLoop(OperationContext* opCtx,
                                    const ShardsvrCreateCollection& request);


/**
 * Creates the specified nss as an unsharded collection. Calls the above
 * createCollectionWithRouterLoop function.
 */
void createCollectionWithRouterLoop(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Returns the only allowed `shardCollection` request for `config.system.sessions`
 */
ShardsvrCreateCollection shardLogicalSessionsCollectionRequest();

}  // namespace cluster
}  // namespace mongo
