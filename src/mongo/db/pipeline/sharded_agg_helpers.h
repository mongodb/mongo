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

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/query/cluster_aggregation_planner.h"

namespace mongo {
class CachedCollectionRoutingInfo;

namespace sharded_agg_helpers {
struct DispatchShardPipelineResults {
    // True if this pipeline was split, and the second half of the pipeline needs to be run on
    // the primary shard for the database.
    bool needsPrimaryShardMerge;

    // Populated if this *is not* an explain, this vector represents the cursors on the remote
    // shards.
    std::vector<OwnedRemoteCursor> remoteCursors;

    // Populated if this *is* an explain, this vector represents the results from each shard.
    std::vector<AsyncRequestsSender::Response> remoteExplainOutput;

    // The split version of the pipeline if more than one shard was targeted, otherwise
    // boost::none.
    boost::optional<cluster_aggregation_planner::SplitPipeline> splitPipeline;

    // If the pipeline targeted a single shard, this is the pipeline to run on that shard.
    std::unique_ptr<Pipeline, PipelineDeleter> pipelineForSingleShard;

    // The command object to send to the targeted shards.
    BSONObj commandForTargetedShards;

    // How many exchange producers are running the shard part of splitPipeline.
    size_t numProducers;

    // The exchange specification if the query can run with the exchange otherwise boost::none.
    boost::optional<cluster_aggregation_planner::ShardedExchangePolicy> exchangeSpec;
};

Shard::RetryPolicy getDesiredRetryPolicy(const AggregationRequest& req);

bool mustRunOnAllShards(const NamespaceString& nss, const LiteParsedPipeline& litePipe);

StatusWith<CachedCollectionRoutingInfo> getExecutionNsRoutingInfo(OperationContext* opCtx,
                                                                  const NamespaceString& execNss);

/**
 * Targets shards for the pipeline and returns a struct with the remote cursors or results, and the
 * pipeline that will need to be executed to merge the results from the remotes. If a stale shard
 * version is encountered, refreshes the routing table and tries again.
 */
DispatchShardPipelineResults dispatchShardPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& executionNss,
    const AggregationRequest& aggRequest,
    const LiteParsedPipeline& liteParsedPipeline,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    BSONObj collationObj);

std::set<ShardId> getTargetedShards(OperationContext* opCtx,
                                    bool mustRunOnAllShards,
                                    const boost::optional<CachedCollectionRoutingInfo>& routingInfo,
                                    const BSONObj shardQuery,
                                    const BSONObj collation);

std::vector<RemoteCursor> establishShardCursors(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const LiteParsedPipeline& litePipe,
    boost::optional<CachedCollectionRoutingInfo>& routingInfo,
    const BSONObj& cmdObj,
    const AggregationRequest& request,
    const ReadPreferenceSetting& readPref,
    const BSONObj& shardQuery);

BSONObj createCommandForTargetedShards(
    OperationContext* opCtx,
    const AggregationRequest& request,
    const LiteParsedPipeline& litePipe,
    const cluster_aggregation_planner::SplitPipeline& splitPipeline,
    const BSONObj collationObj,
    const boost::optional<cluster_aggregation_planner::ShardedExchangePolicy> exchangeSpec,
    bool needsMerge);

BSONObj createPassthroughCommandForShard(OperationContext* opCtx,
                                         const AggregationRequest& request,
                                         const boost::optional<ShardId>& shardId,
                                         Pipeline* pipeline,
                                         BSONObj collationObj);

BSONObj genericTransformForShards(MutableDocument&& cmdForShards,
                                  OperationContext* opCtx,
                                  const boost::optional<ShardId>& shardId,
                                  const AggregationRequest& request,
                                  BSONObj collationObj);

/**
 * For a sharded collection, establishes remote cursors on each shard that may have results, and
 * creates a DocumentSourceMergeCursors stage to merge the remove cursors. Returns a pipeline
 * beginning with that DocumentSourceMergeCursors stage. Note that one of the 'remote' cursors might
 * be this node itself.
 */
std::unique_ptr<Pipeline, PipelineDeleter> targetShardsAndAddMergeCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* ownedPipeline);

}  // namespace sharded_agg_helpers
}  // namespace mongo
