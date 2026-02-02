/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/lookup_stage.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/util/modules.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace test {
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
    StringData json, const boost::intrusive_ptr<ExpressionContext>& expCtx);

BSONObj sequentialCacheStageObj(
    StringData status = "kBuilding"_sd,
    long long maxSizeBytes = loadMemoryLimit(StageMemoryLimit::DocumentSourceLookupCacheSizeBytes));

boost::intrusive_ptr<mongo::exec::agg::LookUpStage> buildLookUpStage(
    const boost::intrusive_ptr<DocumentSource>& ds);

}  // namespace test
}  // namespace mongo
