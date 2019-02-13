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

#include "mongo/db/pipeline/exchange_spec_gen.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/owned_remote_cursor.h"
#include "mongo/s/shard_id.h"

namespace mongo {
namespace cluster_aggregation_planner {

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
 * Creates a new DocumentSourceMergeCursors from the provided 'remoteCursors' and adds it to the
 * front of 'mergePipeline'.
 */
void addMergeCursorsSource(Pipeline* mergePipeline,
                           const LiteParsedPipeline&,
                           BSONObj cmdSentToShards,
                           std::vector<OwnedRemoteCursor> remoteCursors,
                           const std::vector<ShardId>& targetedShards,
                           boost::optional<BSONObj> shardCursorsSortSpec,
                           executor::TaskExecutor*);

/**
 * Builds a ClusterClientCursor which will execute 'pipeline'. If 'pipeline' consists entirely of
 * $skip and $limit stages, the pipeline is eliminated entirely and replaced with a RouterExecStage
 * tree that does same thing but will avoid using a RouterStagePipeline. Avoiding a
 * RouterStagePipeline will remove an expensive conversion from BSONObj -> Document for each result.
 */
ClusterClientCursorGuard buildClusterCursor(OperationContext* opCtx,
                                            std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                                            ClusterClientCursorParams&&);

struct ShardedExchangePolicy {
    // The exchange specification that will be sent to shards as part of the aggregate command.
    // It will be used by producers to determine how to distribute documents to consumers.
    ExchangeSpec exchangeSpec;

    // Shards that will run the consumer part of the exchange.
    std::vector<ShardId> consumerShards;
};

/**
 * If the merging pipeline is eligible for an $exchange merge optimization, returns the information
 * required to set that up.
 */
boost::optional<ShardedExchangePolicy> checkIfEligibleForExchange(OperationContext* opCtx,
                                                                  const Pipeline* mergePipeline);
}  // namespace cluster_aggregation_planner
}  // namespace mongo
