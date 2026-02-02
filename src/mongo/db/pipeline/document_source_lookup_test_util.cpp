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

#include "mongo/db/pipeline/document_source_lookup_test_util.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/lookup_stage.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/unittest/unittest.h"


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
    StringData json, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return makeLookUpFromBson(fromjson(json).firstElement(), expCtx);
}

BSONObj sequentialCacheStageObj(const StringData status, const long long maxSizeBytes) {
    return BSON("$sequentialCache" << BSON("maxSizeBytes" << maxSizeBytes << "status" << status));
}

boost::intrusive_ptr<mongo::exec::agg::LookUpStage> buildLookUpStage(
    const boost::intrusive_ptr<DocumentSource>& ds) {
    auto result = boost::dynamic_pointer_cast<exec::agg::LookUpStage>(exec::agg::buildStage(ds));
    ASSERT(result);
    return result;
}
}  // namespace test
}  // namespace mongo
