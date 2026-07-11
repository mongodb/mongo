// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Returns the shards that would be targeted for the given query according to the given routing
 * info.
 */
std::set<ShardId> getTargetedShardsForQuery(boost::intrusive_ptr<ExpressionContext> expCtx,
                                            const CollectionRoutingInfo& cri,
                                            const BSONObj& query,
                                            const BSONObj& collation);
/**
 * Returns the shards that would be targeted for the given query according to the given routing
 * info.
 */
std::set<ShardId> getTargetedShardsForCanonicalQuery(const CanonicalQuery& query,
                                                     const CollectionRoutingInfo& cri);

}  // namespace mongo
