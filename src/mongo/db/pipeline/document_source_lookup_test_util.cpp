// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_lookup_test_util.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/lookup_stage.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/unittest/unittest.h"

#include <string_view>


namespace mongo {
namespace test {

DocumentSourceLookupMockMongoInterface::DocumentSourceLookupMockMongoInterface(
    std::deque<DocumentSource::GetNextResult> mockResults, bool removeLeadingQueryStages)
    : _mockResults(std::move(mockResults)), _removeLeadingQueryStages(removeLeadingQueryStages) {}


bool DocumentSourceLookupMockMongoInterface::isSharded(OperationContext* opCtx,
                                                       const NamespaceString& ns) {
    return false;
}

std::unique_ptr<Pipeline>
DocumentSourceLookupMockMongoInterface::finalizeAndMaybePreparePipelineForExecution(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<Pipeline> pipeline,
    bool attachCursorAfterOptimizing,
    std::function<void(Pipeline* pipeline)> optimizePipeline,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern,
    bool shouldUseCollectionDefaultCollator) {
    if (optimizePipeline) {
        optimizePipeline(pipeline.get());
    }

    if (attachCursorAfterOptimizing) {
        return preparePipelineForExecution(std::move(pipeline), shardTargetingPolicy, readConcern);
    }
    return pipeline;
}

std::unique_ptr<Pipeline> DocumentSourceLookupMockMongoInterface::preparePipelineForExecution(
    std::unique_ptr<Pipeline> pipeline,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern) {

    while (_removeLeadingQueryStages && !pipeline->empty()) {
        if (pipeline->popFrontWithName("$match") || pipeline->popFrontWithName("$sort") ||
            pipeline->popFrontWithName("$project")) {
            continue;
        }
        break;
    }

    pipeline->addInitialSource(
        DocumentSourceMock::createForTest(_mockResults, pipeline->getContext()));
    return pipeline;
}

std::unique_ptr<Pipeline> DocumentSourceLookupMockMongoInterface::preparePipelineForExecution(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const AggregateCommandRequest& aggRequest,
    std::unique_ptr<Pipeline> pipeline,
    boost::optional<BSONObj> shardCursorsSortSpec,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern,
    bool shouldUseCollectionDefaultCollator) {
    return preparePipelineForExecution(std::move(pipeline), shardTargetingPolicy, readConcern);
}

boost::intrusive_ptr<DocumentSourceLookUp> makeLookUpFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto result = boost::dynamic_pointer_cast<DocumentSourceLookUp>(
        DocumentSourceLookUp::createFromBson(elem, expCtx));
    ASSERT(result);
    return result;
}

boost::intrusive_ptr<DocumentSourceLookUp> makeLookUpFromJson(
    std::string_view json, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return makeLookUpFromBson(fromjson(json).firstElement(), expCtx);
}

BSONObj sequentialCacheStageObj(OperationContext* opCtx,
                                const std::string_view status,
                                boost::optional<long long> maxSizeBytes) {
    long long resolvedMaxSizeBytes = maxSizeBytes.value_or(
        loadMemoryLimit(StageMemoryLimit::DocumentSourceLookupCacheSizeBytes).get(opCtx));
    return BSON("$sequentialCache"
                << BSON("maxSizeBytes" << resolvedMaxSizeBytes << "status" << status));
}

boost::intrusive_ptr<mongo::exec::agg::LookUpStage> buildLookUpStage(
    const boost::intrusive_ptr<DocumentSource>& ds) {
    auto result = boost::dynamic_pointer_cast<exec::agg::LookUpStage>(exec::agg::buildStage(ds));
    ASSERT(result);
    return result;
}
}  // namespace test
}  // namespace mongo
