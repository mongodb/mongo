// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/graph_lookup_mock_mongo_interface.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/process_interface/standalone_process_interface.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/db/pipeline/sharded_agg_helpers_targeting_policy.h"

#include <deque>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace test {

GraphLookUpMockMongoInterface::GraphLookUpMockMongoInterface(
    std::deque<DocumentSource::GetNextResult> results)
    : StandaloneProcessInterface(nullptr), _results(std::move(results)) {}


std::unique_ptr<Pipeline>
GraphLookUpMockMongoInterface::finalizeAndMaybePreparePipelineForExecution(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<Pipeline> pipeline,
    bool attachCursorAfterOptimizing,
    std::function<void(Pipeline* pipeline)> optimizePipeline,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern,
    bool shouldUseCollectionDefaultCollator) {
    if (_shardedViewThrowCount > 0) {
        --_shardedViewThrowCount;
        uassertStatusOK(
            Status(ResolvedNamespace(expCtx->getNamespaceString(), std::vector<BSONObj>{}),
                   "Mock $graphLookup sharded view kickback"));
    }

    if (optimizePipeline) {
        optimizePipeline(pipeline.get());
    }

    if (attachCursorAfterOptimizing) {
        return preparePipelineForExecution(std::move(pipeline), shardTargetingPolicy, readConcern);
    }
    return pipeline;
}

std::unique_ptr<Pipeline> GraphLookUpMockMongoInterface::preparePipelineForExecution(
    std::unique_ptr<Pipeline> pipeline,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern) {
    pipeline->addInitialSource(DocumentSourceMock::createForTest(_results, pipeline->getContext()));
    return pipeline;
}

std::unique_ptr<mongo::Pipeline> GraphLookUpMockMongoInterface::preparePipelineForExecution(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const AggregateCommandRequest& aggRequest,
    std::unique_ptr<Pipeline> pipeline,
    boost::optional<BSONObj> shardCursorsSortSpec,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern,
    bool shouldUseCollectionDefaultCollator) {
    return preparePipelineForExecution(std::move(pipeline), shardTargetingPolicy, readConcern);
}

}  // namespace test
}  // namespace mongo
