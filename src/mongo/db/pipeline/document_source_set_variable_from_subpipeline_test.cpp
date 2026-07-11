// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_comparator.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/stub_lookup_single_document_process_interface.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <vector>


namespace mongo {
namespace {

/**
 * A MongoProcessInterface use for testing that supports making pipelines with an initial
 * DocumentSourceMock source.
 */
using MockMongoInterface = StubLookupSingleDocumentProcessInterface;

using DocumentSourceSetVariableFromSubPipelineTest = AggregationContextFixture;

TEST_F(DocumentSourceSetVariableFromSubPipelineTest, testParseAndSerialize) {
    auto expCtx = getExpCtx();
    auto testBson =
        BSON("$setVariableFromSubPipeline"
             << BSON("setVariable"
                     << "$$SEARCH_META"
                     << "pipeline"
                     << BSON_ARRAY(BSON("$addFields" << BSON("a" << BSON("$const" << 3))))));
    auto setVariable =
        DocumentSourceSetVariableFromSubPipeline::createFromBson(testBson.firstElement(), expCtx);
    ASSERT(setVariable->isInstanceOf<DocumentSourceSetVariableFromSubPipeline>());
    std::vector<Value> serializedArray;
    setVariable->serializeToArray(serializedArray);
    auto serializedBson = serializedArray[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedBson, testBson);
}

TEST_F(DocumentSourceSetVariableFromSubPipelineTest, testParserErrors) {
    auto expCtx = getExpCtx();
    auto missingSetVar =
        BSON("$setVariableFromSubPipeline" << BSON(
                 "pipeline" << BSON_ARRAY(BSON("$addFields" << BSON("a" << BSON("$const" << 3))))));

    ASSERT_THROWS_CODE(DocumentSourceSetVariableFromSubPipeline::createFromBson(
                           missingSetVar.firstElement(), expCtx),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);

    auto missingPipeline =
        BSON("$setVariableFromSubPipeline" << BSON("setVariable" << "$$SEARCH_META"));
    ASSERT_THROWS_CODE(DocumentSourceSetVariableFromSubPipeline::createFromBson(
                           missingPipeline.firstElement(), expCtx),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);

    auto wrongType =
        BSON("$setVariableFromSubPipeline"
             << BSON("setVariable" << "$$SEARCH_META"
                                   << "pipeline"
                                   << BSON("$addFields" << BSON("a" << BSON("$const" << 3)))));
    ASSERT_THROWS_CODE(
        DocumentSourceSetVariableFromSubPipeline::createFromBson(wrongType.firstElement(), expCtx),
        AssertionException,
        ErrorCodes::TypeMismatch);
    wrongType = BSON("$setVariableFromSubPipeline" << BSON(
                         "setVariable"
                         << 3000 << "pipeline"
                         << BSON_ARRAY(BSON("$addFields" << BSON("a" << BSON("$const" << 3))))));
    ASSERT_THROWS_CODE(
        DocumentSourceSetVariableFromSubPipeline::createFromBson(wrongType.firstElement(), expCtx),
        AssertionException,
        ErrorCodes::TypeMismatch);

    auto kErrorCodeWrongSetVar = 625291;
    auto wrongSetVar =
        BSON("$setVariableFromSubPipeline"
             << BSON("setVariable"
                     << "$$CLUSTER_TIME"
                     << "pipeline"
                     << BSON_ARRAY(BSON("$addFields" << BSON("a" << BSON("$const" << 3))))));
    ASSERT_THROWS_CODE(DocumentSourceSetVariableFromSubPipeline::createFromBson(
                           wrongSetVar.firstElement(), expCtx),
                       AssertionException,
                       kErrorCodeWrongSetVar);
}

TEST_F(DocumentSourceSetVariableFromSubPipelineTest, testDoGetNext) {
    const auto inputDocs =
        std::vector{Document{{"a", 1}}, Document{{"b", 1}}, Document{{"c", 1}}, Document{{"d", 1}}};
    auto expCtx = getExpCtx();
    const auto mockStageForSetVarStage = exec::agg::MockStage::createForTest(inputDocs[1], expCtx);
    auto ctxForSubPipeline =
        makeCopyForSubPipelineFromExpressionContext(expCtx, expCtx->getNamespaceString());
    const auto mockSourceForSubPipeline =
        DocumentSourceMock::createForTest(inputDocs, ctxForSubPipeline);
    auto setVariableFromSubPipeline = DocumentSourceSetVariableFromSubPipeline::create(
        expCtx,
        Pipeline::create({DocumentSourceMatch::create(BSON("d" << 1), ctxForSubPipeline)},
                         ctxForSubPipeline),
        Variables::kSearchMetaId);

    setVariableFromSubPipeline->addSubPipelineInitialSource(mockSourceForSubPipeline);
    auto stage =
        exec::agg::buildStageAndStitch(setVariableFromSubPipeline, mockStageForSetVarStage);

    auto comparator = DocumentComparator();
    auto results = comparator.makeUnorderedDocumentSet();
    auto next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    ASSERT_TRUE(Value::compare(expCtx->variables.getValue(Variables::kSearchMetaId),
                               Value((BSON("d" << 1))),
                               nullptr) == 0);
}
TEST_F(DocumentSourceSetVariableFromSubPipelineTest, QueryShape) {
    const auto inputDocs =
        std::vector{Document{{"a", 1}}, Document{{"b", 1}}, Document{{"c", 1}}, Document{{"d", 1}}};
    auto expCtx = getExpCtx();
    const auto mockStageForSetVarStage = exec::agg::MockStage::createForTest(inputDocs[1], expCtx);
    auto ctxForSubPipeline =
        makeCopyForSubPipelineFromExpressionContext(expCtx, expCtx->getNamespaceString());
    const auto mockSourceForSubPipeline =
        DocumentSourceMock::createForTest(inputDocs, ctxForSubPipeline);
    auto setVariableFromSubPipeline = DocumentSourceSetVariableFromSubPipeline::create(
        expCtx,
        Pipeline::create({DocumentSourceMatch::create(BSON("d" << 1), ctxForSubPipeline)},
                         ctxForSubPipeline),
        Variables::kSearchMetaId);

    setVariableFromSubPipeline->addSubPipelineInitialSource(mockSourceForSubPipeline);
    auto stage =
        exec::agg::buildStageAndStitch(setVariableFromSubPipeline, mockStageForSetVarStage);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$setVariableFromSubPipeline": {
                "setVariable": "HASH<$$SEARCH_META>",
                "pipeline": [
                    {
                        "$mock": {}
                    },
                    {
                        "$match": {
                            "HASH<d>": {
                                "$eq": "?number"
                            }
                        }
                    }
                ]
            }
        })",
        redact(*setVariableFromSubPipeline));
}

TEST_F(DocumentSourceSetVariableFromSubPipelineTest, ShouldPropagateDisposeThroughToSubpipeline) {
    auto expCtx = getExpCtx();

    const auto mockSourceForSetVarStage = exec::agg::MockStage::createForTest({}, expCtx);

    auto ctxForSubPipeline =
        makeCopyForSubPipelineFromExpressionContext(expCtx, expCtx->getNamespaceString());
    const auto mockSourceForSubPipeline = DocumentSourceMock::createForTest({}, ctxForSubPipeline);

    auto setVariableFromSubPipeline = DocumentSourceSetVariableFromSubPipeline::create(
        expCtx,
        Pipeline::create({mockSourceForSubPipeline}, ctxForSubPipeline),
        Variables::kSearchMetaId);

    auto stage =
        exec::agg::buildStageAndStitch(setVariableFromSubPipeline, mockSourceForSetVarStage);

    // Make sure that if we call dispose on the outer pipeline, which includes
    // $setVariableFromSubPipeline, the subpipeline will also be properly disposed.
    stage->dispose();
    ASSERT_TRUE(mockSourceForSetVarStage->isDisposed);
}
}  // namespace
}  // namespace mongo
