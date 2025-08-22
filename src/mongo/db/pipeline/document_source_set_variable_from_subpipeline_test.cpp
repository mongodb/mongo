/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <boost/smart_ptr/intrusive_ptr.hpp>

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
    ASSERT(setVariable->getSourceName() == DocumentSourceSetVariableFromSubPipeline::kStageName);
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
    auto stage = exec::agg::buildStage(setVariableFromSubPipeline);
    stage->setSource(mockStageForSetVarStage.get());

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
    auto stage = exec::agg::buildStage(setVariableFromSubPipeline);
    stage->setSource(mockStageForSetVarStage.get());
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

    auto stage = exec::agg::buildStage(setVariableFromSubPipeline);
    stage->setSource(mockSourceForSetVarStage.get());

    // Make sure that if we call dispose on the outer pipeline, which includes
    // $setVariableFromSubPipeline, the subpipeline will also be properly disposed.
    stage->dispose();
    ASSERT_TRUE(mockSourceForSetVarStage->isDisposed);
}
}  // namespace
}  // namespace mongo
