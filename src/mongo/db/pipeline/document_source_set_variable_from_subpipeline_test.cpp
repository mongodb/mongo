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

#include "mongo/platform/basic.h"

#include <array>
#include <deque>
#include <list>
#include <set>
#include <vector>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_comparator.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_documents.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/stub_lookup_single_document_process_interface.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

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
                       40414);

    auto missingPipeline = BSON("$setVariableFromSubPipeline" << BSON("setVariable"
                                                                      << "$$SEARCH_META"));
    ASSERT_THROWS_CODE(DocumentSourceSetVariableFromSubPipeline::createFromBson(
                           missingPipeline.firstElement(), expCtx),
                       AssertionException,
                       40414);

    auto wrongType =
        BSON("$setVariableFromSubPipeline"
             << BSON("setVariable"
                     << "$$SEARCH_META"
                     << "pipeline" << BSON("$addFields" << BSON("a" << BSON("$const" << 3)))));
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
        std::array{Document{{"a", 1}}, Document{{"b", 1}}, Document{{"c", 1}}, Document{{"d", 1}}};
    const auto mockSourceForSetVarStage =
        DocumentSourceMock::createForTest(inputDocs[0], getExpCtx());
    const auto mockSourceForSubPipeline =
        DocumentSourceMock::createForTest(inputDocs[1], getExpCtx());
    const auto mockSourceForSubPipelineDocSource =
        DocumentSourceMock::createForTest(inputDocs[3], getExpCtx());
    const auto mockDequeOne = std::deque<DocumentSource::GetNextResult>{Document{inputDocs[2]}};
    const auto mockDequeTwo = std::deque<DocumentSource::GetNextResult>{Document{inputDocs[3]}};
    const auto mockCtxOne = getExpCtx()->copyWith({});
    mockCtxOne->mongoProcessInterface = std::make_unique<MockMongoInterface>(mockDequeOne);
    const auto filter = DocumentSourceMatch::create(BSON("d" << 1), mockCtxOne);
    auto setVariableFromSubPipeline = DocumentSourceSetVariableFromSubPipeline::create(
        mockCtxOne,
        Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{filter}, getExpCtx()),
        Variables::kSearchMetaId);

    filter->setSource(mockSourceForSubPipelineDocSource.get());
    setVariableFromSubPipeline->setSource(mockSourceForSetVarStage.get());

    auto comparator = DocumentComparator();
    auto results = comparator.makeUnorderedDocumentSet();
    auto next = setVariableFromSubPipeline->getNext();
    ASSERT_TRUE(next.isAdvanced());

    // Test that $$SEARCH_META is now set and is equal to the value we expect.
    ASSERT_TRUE(Value::compare(mockCtxOne->variables.getValue(Variables::kSearchMetaId),
                               Value((BSON("d" << 1))),
                               nullptr) == 0);
}


TEST_F(DocumentSourceSetVariableFromSubPipelineTest, testDoOptimizeAt) {
    Pipeline::SourceContainer pipelineContainer;
    auto testBson =
        BSON("$setVariableFromSubPipeline"
             << BSON("setVariable"
                     << "$$SEARCH_META"
                     << "pipeline"
                     << BSON_ARRAY(BSON("$addFields" << BSON("a" << BSON("$const" << 3))))));
    auto setVariable = DocumentSourceSetVariableFromSubPipeline::createFromBson(
        testBson.firstElement(), getExpCtx());
    pipelineContainer.push_back(setVariable);

    // $geoNear is a shard-only stage, DocumentSourceSetVariableFromSubPipeline should optimize.
    auto geoNearStageObj =
        fromjson("{$geoNear: {distanceField: 'dist', near: [0, 0], key: 'a.b'}}");
    auto geoNear =
        DocumentSourceGeoNear::createFromBson(geoNearStageObj.firstElement(), getExpCtx());
    pipelineContainer.push_back(geoNear);
    pipelineContainer.front()->optimizeAt(pipelineContainer.begin(), &pipelineContainer);
    ASSERT(typeid(**pipelineContainer.begin()) == typeid(DocumentSourceGeoNear));

    // $lookUp is a merging stage, so DocumentSourceSetVariableFromSubPipeline shouldn't optimize.
    pipelineContainer.pop_front();
    ASSERT(typeid(**pipelineContainer.begin()) == typeid(DocumentSourceSetVariableFromSubPipeline));
    auto expCtx = getExpCtx();
    NamespaceString fromNs("test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});
    auto lookupStage = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from"
                               << "coll"
                               << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1))) << "as"
                               << "as"))
            .firstElement(),
        expCtx);
    pipelineContainer.push_back(lookupStage);
    pipelineContainer.front()->optimizeAt(pipelineContainer.begin(), &pipelineContainer);
    ASSERT(typeid(**pipelineContainer.begin()) == typeid(DocumentSourceSetVariableFromSubPipeline));
}

}  // namespace
}  // namespace mongo
