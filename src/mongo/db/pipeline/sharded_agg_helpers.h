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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/exchange_spec_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/sharded_agg_helpers_targeting_policy.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/shard_id.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/query/owned_remote_cursor.h"
#include "mongo/s/stale_shard_version_helpers.h"

namespace mongo {
namespace sharded_agg_helpers {

/**
 * Represents the two halves of a pipeline that will execute in a sharded cluster. 'shardsPipeline'
 * will execute in parallel on each shard, and 'mergePipeline' will execute on the merge host -
 * either one of the shards or a mongos.
 */
struct SplitPipeline {
    SplitPipeline(std::unique_ptr<Pipeline, PipelineDeleter> shardsPipeline,
                  std::unique_ptr<Pipeline, PipelineDeleter> mergePipeline,
                  boost::optional<BSONObj> shardCursorsSortSpec)
        : shardsPipeline(std::move(shardsPipeline)),
          mergePipeline(std::move(mergePipeline)),
          shardCursorsSortSpec(std::move(shardCursorsSortSpec)) {}

    std::unique_ptr<Pipeline, PipelineDeleter> shardsPipeline;
    std::unique_ptr<Pipeline, PipelineDeleter> mergePipeline;

    // If set, the cursors from the shards are expected to be sorted according to this spec, and to
    // have populated a "$sortKey" metadata field which can be used to compare the results.
    boost::optional<BSONObj> shardCursorsSortSpec;
};

struct ShardedExchangePolicy {
    // The exchange specification that will be sent to shards as part of the aggregate command.
    // It will be used by producers to determine how to distribute documents to consumers.
    ExchangeSpec exchangeSpec;

    // Shards that will run the consumer part of the exchange.
    std::vector<ShardId> consumerShards;
};

struct DispatchShardPipelineResults {
    // Contains a value when the second half of the pipeline was requested to run on a specific
    // shard.
    boost::optional<ShardId> mergeShardId;

    // Populated if this *is not* an explain, this vector represents the cursors on the remote
    // shards.
    std::vector<OwnedRemoteCursor> remoteCursors;

    // Populated if this *is* an explain, this vector represents the results from each shard.
    std::vector<AsyncRequestsSender::Response> remoteExplainOutput;

    // The split version of the pipeline if more than one shard was targeted, otherwise
    // boost::none.
    boost::optional<SplitPipeline> splitPipeline;

    // If the pipeline targeted a single shard, this is the pipeline to run on that shard.
    std::unique_ptr<Pipeline, PipelineDeleter> pipelineForSingleShard;

    // The command object to send to the targeted shards.
    BSONObj commandForTargetedShards;

    // How many exchange producers are running the shard part of splitPipeline.
    size_t numProducers;

    // The exchange specification if the query can run with the exchange otherwise boost::none.
    boost::optional<ShardedExchangePolicy> exchangeSpec;
};

/**
 * If the merging pipeline is eligible for an $exchange merge optimization, returns the information
 * required to set that up.
 */
boost::optional<ShardedExchangePolicy> checkIfEligibleForExchange(OperationContext* opCtx,
                                                                  const Pipeline* mergePipeline);

/**
 * Split the current Pipeline into a Pipeline for each shard, and a Pipeline that combines the
 * results within a merging process. This call also performs optimizations with the aim of reducing
 * computing time and network traffic when a pipeline has been split into two pieces.
 *
 * The 'mergePipeline' returned as part of the SplitPipeline here is not ready to execute until the
 * 'shardsPipeline' has been sent to the shards and cursors have been established. Once cursors have
 * been established, the merge pipeline can be made executable by calling 'addMergeCursorsSource()'
 */
SplitPipeline splitPipeline(std::unique_ptr<Pipeline, PipelineDeleter> pipeline);

/**
 * Used to indicate if a pipeline contains any data source requiring extra handling for targeting
 * shards.
 */
enum class PipelineDataSource {
    kNormal,
    kChangeStream,  // Indicates a pipeline has a $changeStream stage.
    kQueue,         // Indicates the desugared pipeline starts with a $queue stage.
};

/**
 * Targets shards for the pipeline and returns a struct with the remote cursors or results, and the
 * pipeline that will need to be executed to merge the results from the remotes. If a stale shard
 * version is encountered, refreshes the routing table and tries again. If the command is eligible
 * for sampling, attaches a unique sample id to the request for one of the targeted shards if the
 * collection has query sampling enabled and the rate-limited sampler successfully generates a
 * sample id for it.
 *
 * Although the 'pipeline' has an 'ExpressionContext' which indicates whether this operation is an
 * explain (and if it is an explain what the verbosity is), the caller must explicitly indicate
 * whether it wishes to dispatch a regular aggregate command or an explain command using the
 * explicit 'explain' parameter. The reason for this is that in some contexts, the caller wishes to
 * dispatch a regular agg command rather than an explain command even if the top-level operation is
 * an explain. Consider the example of an explain that contains a stage like this:
 *
 *     {$unionWith: {coll: "innerShardedColl", pipeline: <sub-pipeline>}}
 *
 * The explain works by first executing the inner and outer subpipelines in order to gather runtime
 * statistics. While dispatching the inner pipeline, we must dispatch it not as an explain but as a
 * regular agg command so that the runtime stats are accurate.
 */
DispatchShardPipelineResults dispatchShardPipeline(
    Document serializedCommand,
    PipelineDataSource pipelineDataSource,
    bool eligibleForSampling,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    boost::optional<ExplainOptions::Verbosity> explain,
    boost::optional<CollectionRoutingInfo> cri = boost::none,
    ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
    boost::optional<BSONObj> readConcern = boost::none,
    AsyncRequestsSender::ShardHostMap designatedHostsMap = {},
    stdx::unordered_map<ShardId, BSONObj> resumeTokenMap = {},
    std::set<ShardId> shardsToSkip = {});

BSONObj createPassthroughCommandForShard(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Document serializedCommand,
    boost::optional<ExplainOptions::Verbosity> explainVerbosity,
    Pipeline* pipeline,
    boost::optional<BSONObj> readConcern,
    boost::optional<int> overrideBatchSize);

BSONObj createCommandForTargetedShards(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       Document serializedCommand,
                                       const SplitPipeline& splitPipeline,
                                       boost::optional<ShardedExchangePolicy> exchangeSpec,
                                       bool needsMerge,
                                       boost::optional<ExplainOptions::Verbosity> explain,
                                       boost::optional<BSONObj> readConcern = boost::none);

/**
 * Convenience method for callers that want to do 'partitionCursors', 'injectMetaCursors', and
 * 'addMergeCursorsSource' in order.
 */
void partitionAndAddMergeCursorsSource(Pipeline* pipeline,
                                       std::vector<OwnedRemoteCursor> cursors,
                                       boost::optional<BSONObj> shardCursorsSortSpec);

/**
 * Targets the shards with an aggregation command built from `ownedPipeline` and explain set to
 * true. Returns a BSONObj of the form {"pipeline": {<pipelineExplainOutput>}}.
 */
BSONObj targetShardsForExplain(Pipeline* ownedPipeline);

/**
 * Appends the explain output of `dispatchResults` to `result`.
 */
Status appendExplainResults(DispatchShardPipelineResults&& dispatchResults,
                            const boost::intrusive_ptr<ExpressionContext>& mergeCtx,
                            BSONObjBuilder* result);

/**
 * Returns the proper routing table to use for targeting shards: either a historical routing table
 * based on the global read timestamp if there is an active transaction with snapshot level read
 * concern or the latest routing table otherwise.
 *
 * Returns 'ShardNotFound' or 'NamespaceNotFound' if there are no shards in the cluster or if
 * collection 'execNss' does not exist, respectively.
 */
StatusWith<CollectionRoutingInfo> getExecutionNsRoutingInfo(OperationContext* opCtx,
                                                            const NamespaceString& execNss);

/**
 * Returns true if an aggregation over 'nss' must run on all shards.
 */
bool checkIfMustRunOnAllShards(const NamespaceString& nss, PipelineDataSource pipelineDataSource);

/**
 * Retrieves the desired retry policy based on whether the default writeConcern is set on 'opCtx'.
 */
Shard::RetryPolicy getDesiredRetryPolicy(OperationContext* opCtx);

/**
 * Prepares the given pipeline for execution. This involves:
 * (1) Determining if the pipeline needs to have a cursor source attached.
 * (2) If a cursor source is needed, attaching one. This may involve a local or remote cursor,
 * depending on whether or not the pipeline's expression context permits local reads and a local
 * read could be used to serve the pipeline. (3) Splitting the pipeline if required, and dispatching
 * half to the shards, leaving the merging half executing in this process after attaching a
 * $mergeCursors.
 *
 * Will retry on network errors and also on StaleConfig errors to avoid restarting the entire
 * operation. Returns `ownedPipeline`, but made-ready for execution.
 */
std::unique_ptr<Pipeline, PipelineDeleter> preparePipelineForExecution(
    Pipeline* ownedPipeline,
    ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
    boost::optional<BSONObj> readConcern = boost::none);

/**
 * For a sharded collection, establishes remote cursors on each shard that may have results, and
 * creates a DocumentSourceMergeCursors stage to merge the remote cursors. Returns a pipeline
 * beginning with that DocumentSourceMergeCursors stage. Note that one of the 'remote' cursors might
 * be this node itself.
 *
 * Even if the ExpressionContext indicates that this operation is explain, this function still
 * dispatches the pipeline as a non-explain, since it must open cursors on the remote nodes and
 * merge them with a $mergeCursors. If the caller's intent is to dispatch an explain command, it
 * must use a different helper.
 *
 * Use the AggregateCommandRequest alternative for 'targetRequest' to explicitly specify command
 * options (e.g. read concern) to the shards when establishing remote cursors. Note that doing so
 * incurs the cost of parsing the pipeline.
 *
 * Use the std::pair<AggregateCommandRequest, std::unique_ptr<Pipeline, PipelineDeleter>>
 * alternative for 'targetRequest' to explicitly specify command options (e.g. read concern) to the
 * shards when establishing remote cursors, and to pass a pipeline that has already been parsed.
 * This is useful when the pipeline has already been parsed as it avoids the cost
 * of parsing it again.
 */
std::unique_ptr<Pipeline, PipelineDeleter> targetShardsAndAddMergeCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::variant<std::unique_ptr<Pipeline, PipelineDeleter>,
                 AggregateCommandRequest,
                 std::pair<AggregateCommandRequest, std::unique_ptr<Pipeline, PipelineDeleter>>>
        targetRequest,
    boost::optional<BSONObj> shardCursorsSortSpec = boost::none,
    ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
    boost::optional<BSONObj> readConcern = boost::none);

/**
 * For a sharded or unsharded collection, establishes a remote cursor on only the specified shard,
 * and creates a DocumentSourceMergeCursors stage to consume the remote cursor. Returns a pipeline
 * beginning with that DocumentSourceMergeCursors stage.
 *
 * This function bypasses normal shard targeting for sharded and unsharded collections. It is
 * especially useful for reading from unsharded collections such as config.transactions and
 * local.oplog.rs that cannot be targeted by targetShardsAndAddMergeCursors().
 *
 * Note that the specified AggregateCommandRequest must not be for an explain command.
 */
std::unique_ptr<Pipeline, PipelineDeleter> runPipelineDirectlyOnSingleShard(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    AggregateCommandRequest request,
    ShardId shardId);

}  // namespace sharded_agg_helpers
}  // namespace mongo
