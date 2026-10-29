// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/shard_targeting_helpers.h"

#include "mongo/db/query/query_planner_common.h"
#include "mongo/s/query/shard_key_pattern_query_util.h"

namespace mongo {

std::set<ShardId> getTargetedShardsForQuery(boost::intrusive_ptr<ExpressionContext> expCtx,
                                            const CollectionRoutingInfo& cri,
                                            const BSONObj& query,
                                            const BSONObj& collation) {
    if (cri.hasRoutingTable()) {
        // The collection has a routing table. Use it to decide which shards to target based on the
        // query and collation.
        std::set<ShardId> shardIds;
        getShardIdsForQuery(expCtx, query, collation, cri.getChunkManager(), &shardIds);
        return shardIds;
    }

    // The collection does not have a routing table. Target only the primary shard for the database.
    return {cri.getDbPrimaryShardId()};
}

std::set<ShardId> getTargetedShardsForCanonicalQuery(const CanonicalQuery& query,
                                                     const CollectionRoutingInfo& cri) {
    if (cri.hasRoutingTable()) {
        const auto& cm = cri.getChunkManager();

        // The collection has a routing table. Use it to decide which shards to target based on the
        // query and collation.

        // If the query has a hint or geo expression, fall back to re-creating a find command from
        // scratch. Hint can interfere with query planning, which we rely on for targeting. Shard
        // targeting modifies geo queries and this helper shouldn't have a side effect on 'query'.
        const auto& findCommand = query.getFindCommandRequest();
        if (!findCommand.getHint().isEmpty() ||
            QueryPlannerCommon::hasNode(query.getPrimaryMatchExpression(),
                                        MatchExpression::GEO_NEAR)) {
            return getTargetedShardsForQuery(
                query.getExpCtx(), cri, findCommand.getFilter(), findCommand.getCollation());
        }

        query.getExpCtx()->setUUID(cm.getUUID());

        // 'getShardIdsForCanonicalQuery' assumes that the ExpressionContext has the appropriate
        // collation set. Here, if the query collation is empty, we use the collection default
        // collation for targeting.
        const auto& collation = query.getFindCommandRequest().getCollation();
        if (collation.isEmpty() && cm.getDefaultCollator()) {
            auto defaultCollator = cm.getDefaultCollator();
            query.getExpCtx()->setCollator(defaultCollator->clone());
        }

        std::set<ShardId> shardIds;
        getShardIdsForCanonicalQuery(query, cm, &shardIds);
        return shardIds;
    }

    // In the event of an untracked collection, we will discover the collection default collation on
    // the primary shard. As such, we don't forward the simple collation.
    if (query.getFindCommandRequest().getCollation().isEmpty()) {
        query.getExpCtx()->setIgnoreCollator();
    }

    // The collection does not have a routing table. Target only the primary shard for the database.
    return {cri.getDbPrimaryShardId()};
}

}  // namespace mongo
