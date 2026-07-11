// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/lookup_stage.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace test {
using namespace std::literals::string_view_literals;
/**
 * A mock 'MongoProcessInterface' which allows mocking a foreign pipeline. If
 * 'removeLeadingQueryStages' is true then any $match, $sort or $project fields at the start of
 * the pipeline will be removed, simulating the pipeline changes which occur when
 * PipelineD::prepareCursorSource absorbs stages into the PlanExecutor.
 */
class DocumentSourceLookupMockMongoInterface final : public StubMongoProcessInterface {
public:
    DocumentSourceLookupMockMongoInterface(std::deque<DocumentSource::GetNextResult> mockResults,
                                           bool removeLeadingQueryStages = false);

    bool isSharded(OperationContext* opCtx, const NamespaceString& ns) final;

    std::unique_ptr<Pipeline> finalizeAndMaybePreparePipelineForExecution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::unique_ptr<Pipeline> pipeline,
        bool attachCursorAfterOptimizing,
        std::function<void(Pipeline* pipeline)> optimizePipeline = nullptr,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none,
        bool shouldUseCollectionDefaultCollator = false) override;

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        std::unique_ptr<Pipeline> pipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) final;

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const AggregateCommandRequest& aggRequest,
        std::unique_ptr<Pipeline> pipeline,
        boost::optional<BSONObj> shardCursorsSortSpec = boost::none,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none,
        bool shouldUseCollectionDefaultCollator = false) final;

private:
    std::deque<DocumentSource::GetNextResult> _mockResults;
    bool _removeLeadingQueryStages = false;
};

boost::intrusive_ptr<DocumentSourceLookUp> makeLookUpFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

boost::intrusive_ptr<DocumentSourceLookUp> makeLookUpFromJson(
    std::string_view json, const boost::intrusive_ptr<ExpressionContext>& expCtx);

BSONObj sequentialCacheStageObj(
    std::string_view status = "kBuilding"sv,
    long long maxSizeBytes =
        loadMemoryLimit(StageMemoryLimit::DocumentSourceLookupCacheSizeBytes).get());

boost::intrusive_ptr<mongo::exec::agg::LookUpStage> buildLookUpStage(
    const boost::intrusive_ptr<DocumentSource>& ds);

}  // namespace test
}  // namespace mongo
