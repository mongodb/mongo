/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
