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

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <functional>
#include <memory>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/shard_id.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/database_version.h"
#include "mongo/s/query/exec/cluster_client_cursor_guard.h"
#include "mongo/s/query/exec/cluster_client_cursor_impl.h"
#include "mongo/s/query/exec/cluster_client_cursor_params.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace cluster_aggregation_planner {

/**
 * Builds a ClusterClientCursor which will execute 'pipeline'. If 'pipeline' consists entirely of
 * $skip and $limit stages, the pipeline is eliminated entirely and replaced with a RouterExecStage
 * tree that does same thing but will avoid using a RouterStagePipeline. Avoiding a
 * RouterStagePipeline will remove an expensive conversion from BSONObj -> Document for each result.
 */
ClusterClientCursorGuard buildClusterCursor(OperationContext* opCtx,
                                            std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                                            ClusterClientCursorParams&&);

/**
 *  Returns the collation for aggregation targeting 'nss' with the following semantics:
 *  - Return 'collation' if the aggregation is collectionless.
 *  - If 'nss' is tracked, we return 'collation' if it is non-empty. If it is empty, we return the
 * collection default collation if there is one and the simple collation otherwise.
 *  - If 'nss' is untracked, we return an empty BSONObj as we will infer the correct collation when
 * the command reaches the primary shard. The exception is when
 * 'requiresCollationForParsingUnshardedAggregate' is true: in this case, we must contact the
 * primary shard to infer the collation as it is required during parsing.
 *
 *  TODO SERVER-81991: Delete 'requiresCollationForParsingUnshardedAggregate' parameter once all
 * unsharded collections are tracked in the sharding catalog as unsplittable along with their
 * collation.
 */
BSONObj getCollation(OperationContext* opCtx,
                     const boost::optional<ChunkManager>& cm,
                     const NamespaceString& nss,
                     const BSONObj& collation,
                     bool requiresCollationForParsingUnshardedAggregate);

/**
 * This structure contains information for targeting an aggregation pipeline in a sharded cluster.
 */
struct AggregationTargeter {
    /**
     * Populates and returns targeting info for an aggregation pipeline on the given namespace
     * 'executionNss'.
     */
    static AggregationTargeter make(
        OperationContext* opCtx,
        std::function<std::unique_ptr<Pipeline, PipelineDeleter>()> buildPipelineFn,
        boost::optional<CollectionRoutingInfo> cri,
        sharded_agg_helpers::PipelineDataSource pipelineDataSource,
        bool perShardCursor);

    enum TargetingPolicy {
        kMongosRequired,
        kAnyShard,
        kSpecificShardOnly,
    } policy;

    std::unique_ptr<Pipeline, PipelineDeleter> pipeline;
    boost::optional<CollectionRoutingInfo> cri;
};

/**
 * Runs a pipeline on mongoS, having first validated that it is eligible to do so. This can be a
 * pipeline which is split for merging, or an intact pipeline which must run entirely on mongoS.
 */
Status runPipelineOnMongoS(const ClusterAggregate::Namespaces& namespaces,
                           long long batchSize,
                           std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                           BSONObjBuilder* result,
                           const PrivilegeVector& privileges,
                           bool requestQueryStatsFromRemotes);

/**
 * Dispatches the pipeline in 'targeter' to the shards that are involved, and merges the results if
 * necessary on either mongos or a randomly designated shard. If 'eligibleForSampling' is true,
 * attaches a unique sample id to the request for one of the targeted shards if the collection has
 * query sampling enabled and the rate-limited sampler successfully generates a sample id for it.
 */
Status dispatchPipelineAndMerge(OperationContext* opCtx,
                                AggregationTargeter targeter,
                                Document serializedCommand,
                                long long batchSize,
                                const ClusterAggregate::Namespaces& namespaces,
                                const PrivilegeVector& privileges,
                                BSONObjBuilder* result,
                                sharded_agg_helpers::PipelineDataSource pipelineDataSource,
                                bool eligibleForSampling,
                                bool requestQueryStatsFromRemotes);

/**
 * Runs a pipeline on a specific shard. Used for running a pipeline on a specifc shard (i.e. by per
 * shard $changeStream cursors). This function will not add a shard version to the request sent to
 * mongod. If 'eligibleForSampling' is true, attaches a unique sample id to the request for that
 * shard if the collection has query sampling enabled and the rate-limited sampler successfully
 * generates a sample id for it.
 */
Status runPipelineOnSpecificShardOnly(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const ClusterAggregate::Namespaces& namespaces,
                                      boost::optional<ExplainOptions::Verbosity> explain,
                                      Document serializedCommand,
                                      const PrivilegeVector& privileges,
                                      ShardId shardId,
                                      bool eligibleForSampling,
                                      BSONObjBuilder* out,
                                      bool requestQueryStatsFromRemotes);

}  // namespace cluster_aggregation_planner
}  // namespace mongo
