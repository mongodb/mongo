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
#include "mongo/s/query/cluster_aggregate.h"
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

/**
 * This structure contains information for targeting an aggregation pipeline in a sharded cluster.
 */
struct AggregationTargeter {
    /**
     * Populates and returns targeting info for an aggregation pipeline on the given namespace
     * 'executionNss'.
     */
    static StatusWith<AggregationTargeter> make(
        OperationContext* opCtx,
        const NamespaceString& executionNss,
        const std::function<std::unique_ptr<Pipeline, PipelineDeleter>(
            boost::optional<CachedCollectionRoutingInfo>)> buildPipelineFn,
        stdx::unordered_set<NamespaceString> involvedNamespaces,
        bool hasChangeStream,
        bool allowedToPassthrough);

    enum TargetingPolicy {
        kPassthrough,
        kMongosRequired,
        kAnyShard,
    } policy;

    std::unique_ptr<Pipeline, PipelineDeleter> pipeline;
    boost::optional<CachedCollectionRoutingInfo> routingInfo;
};

Status runPipelineOnPrimaryShard(OperationContext* opCtx,
                                 const ClusterAggregate::Namespaces& namespaces,
                                 const CachedDatabaseInfo& dbInfo,
                                 boost::optional<ExplainOptions::Verbosity> explain,
                                 Document serializedCommand,
                                 const PrivilegeVector& privileges,
                                 BSONObjBuilder* out);

/**
 * Runs a pipeline on mongoS, having first validated that it is eligible to do so. This can be a
 * pipeline which is split for merging, or an intact pipeline which must run entirely on mongoS.
 */
Status runPipelineOnMongoS(const ClusterAggregate::Namespaces& namespaces,
                           long long batchSize,
                           std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                           BSONObjBuilder* result,
                           const PrivilegeVector& privileges);

/**
 * Dispatches the pipeline in 'targeter' to the shards that are involved, and merges the results if
 * necessary on either mongos or a randomly designated shard.
 */
Status dispatchPipelineAndMerge(OperationContext* opCtx,
                                sharded_agg_helpers::AggregationTargeter targeter,
                                Document serializedCommand,
                                long long batchSize,
                                const ClusterAggregate::Namespaces& namespaces,
                                const PrivilegeVector& privileges,
                                BSONObjBuilder* result,
                                bool hasChangeStream);

/**
 *  Returns the "collation" and "uuid" for the collection given by "nss" with the following
 *  semantics:
 *  - The "collation" parameter will be set to the default collation for the collection or the
 *    simple collation if there is no default. If the collection does not exist or if the aggregate
 *    is on the collectionless namespace, this will be set to an empty object.
 *  - The "uuid" is retrieved from the chunk manager for sharded collections or the listCollections
 *    output for unsharded collections. The UUID will remain unset if the aggregate is on the
 *    collectionless namespace.
 */
std::pair<BSONObj, boost::optional<UUID>> getCollationAndUUID(
    const boost::optional<CachedCollectionRoutingInfo>& routingInfo,
    const NamespaceString& nss,
    const BSONObj& collation);

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
