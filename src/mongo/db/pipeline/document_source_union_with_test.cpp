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

#include "mongo/db/pipeline/document_source_union_with.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_comparator.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/lite_parsed_union_with.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/stub_lookup_single_document_process_interface.h"
#include "mongo/db/pipeline/serverless_aggregation_context_fixture.h"
#include "mongo/db/pipeline/stage_params_to_document_source_registry.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/atomic.h"
#include "mongo/transport/mock_session.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <array>
#include <deque>
#include <list>
#include <vector>

#include <boost/none.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using MockMongoInterface = StubLookupSingleDocumentProcessInterface;

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceUnionWithTest = AggregationContextFixture;

auto makeUnion(const boost::intrusive_ptr<ExpressionContext>& expCtx,
               std::unique_ptr<Pipeline> pipeline) {
    return make_intrusive<DocumentSourceUnionWith>(expCtx, std::move(pipeline));
}

TEST_F(DocumentSourceUnionWithTest, BasicSerialUnions) {
    const auto doc = Document{{"a", 1}};
    const auto mock = exec::agg::MockStage::createForTest(doc, getExpCtx());
    const auto mockDeque = std::deque<DocumentSource::GetNextResult>{Document{doc}};
    getExpCtx()->setMongoProcessInterface(std::make_unique<MockMongoInterface>(mockDeque));
    auto unionWithOneStage = exec::agg::buildStage(makeUnion(
        getExpCtx(),
        Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx())));
    auto unionWithTwo = exec::agg::buildStage(makeUnion(
        getExpCtx(),
        Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx())));
    exec::agg::MockStage::setSource_forTest(unionWithOneStage, mock.get());
    exec::agg::MockStage::setSource_forTest(unionWithTwo, unionWithOneStage.get());

    auto comparator = DocumentComparator();
    const auto expectedResults = 3;
    for (auto i = 0; i < expectedResults; ++i) {
        auto next = unionWithTwo->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_EQ(comparator.compare(next.releaseDocument(), doc), 0);
    }

    ASSERT_TRUE(unionWithTwo->getNext().isEOF());
    ASSERT_TRUE(unionWithTwo->getNext().isEOF());
    ASSERT_TRUE(unionWithTwo->getNext().isEOF());

    unionWithTwo->dispose();
}

TEST_F(DocumentSourceUnionWithTest, BasicNestedUnions) {
    const auto doc = Document{{"a", 1}};
    const auto mock = exec::agg::MockStage::createForTest(doc, getExpCtx());
    const auto mockDeque = std::deque<DocumentSource::GetNextResult>{Document{doc}};
    getExpCtx()->setMongoProcessInterface(std::make_unique<MockMongoInterface>(mockDeque));
    auto unionWithOne = make_intrusive<DocumentSourceUnionWith>(
        getExpCtx(),
        Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx()));
    auto unionWithTwo = exec::agg::buildStage(
        makeUnion(getExpCtx(),
                  Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{unionWithOne},
                                   getExpCtx())));
    exec::agg::MockStage::setSource_forTest(unionWithTwo, mock.get());

    auto comparator = DocumentComparator();
    const auto expectedResults = 3;
    for (auto i = 0; i < expectedResults; ++i) {
        auto next = unionWithTwo->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_EQ(comparator.compare(next.releaseDocument(), doc), 0);
    }

    ASSERT_TRUE(unionWithTwo->getNext().isEOF());
    ASSERT_TRUE(unionWithTwo->getNext().isEOF());
    ASSERT_TRUE(unionWithTwo->getNext().isEOF());

    unionWithTwo->dispose();
}

TEST_F(DocumentSourceUnionWithTest, UnionsWithNonEmptySubPipelines) {
    const auto inputDoc = Document{{"a", 1}};
    const auto outputDocs = std::array{Document{{"a", 1}}, Document{{"a", 1}, {"d", 1}}};
    const auto mock = exec::agg::MockStage::createForTest(inputDoc, getExpCtx());
    const auto mockDeque = std::deque<DocumentSource::GetNextResult>{Document{inputDoc}};
    getExpCtx()->setMongoProcessInterface(std::make_unique<MockMongoInterface>(mockDeque));
    const auto filter = DocumentSourceMatch::create(BSON("d" << 1), getExpCtx());
    const auto proj = DocumentSourceAddFields::create(BSON("d" << 1), getExpCtx());
    auto unionWithOne = exec::agg::buildStage(makeUnion(
        getExpCtx(),
        Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{filter}, getExpCtx())));
    auto unionWithTwo = exec::agg::buildStage(makeUnion(
        getExpCtx(),
        Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{proj}, getExpCtx())));
    exec::agg::MockStage::setSource_forTest(unionWithOne, mock.get());
    exec::agg::MockStage::setSource_forTest(unionWithTwo, unionWithOne.get());

    auto comparator = DocumentComparator();
    auto results = comparator.makeUnorderedDocumentSet();
    for (auto& doc [[maybe_unused]] : outputDocs) {
        auto next = unionWithTwo->getNext();
        ASSERT_TRUE(next.isAdvanced());
        const auto [ignored, inserted] = results.insert(next.releaseDocument());
        ASSERT_TRUE(inserted);
    }
    for (const auto& doc : outputDocs)
        ASSERT_TRUE(results.find(doc) != results.end());

    ASSERT_TRUE(unionWithTwo->getNext().isEOF());
    ASSERT_TRUE(unionWithTwo->getNext().isEOF());
    ASSERT_TRUE(unionWithTwo->getNext().isEOF());

    unionWithTwo->dispose();
}

TEST_F(DocumentSourceUnionWithTest, SerializeAndParseWithPipeline) {
    auto expCtx = getExpCtx();
    NamespaceString nsToUnionWith = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "coll");
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{nsToUnionWith, {nsToUnionWith, std::vector<BSONObj>()}}});
    auto bson =
        BSON("$unionWith" << BSON(
                 "coll" << nsToUnionWith.coll() << "pipeline"
                        << BSON_ARRAY(BSON("$addFields" << BSON("a" << BSON("$const" << 3))))));
    auto unionWith = DocumentSourceUnionWith::createFromBson(bson.firstElement(), expCtx);
    ASSERT(unionWith->isInstanceOf<DocumentSourceUnionWith>());
    std::vector<Value> serializedArray;
    unionWith->serializeToArray(serializedArray);
    auto serializedBson = serializedArray[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedBson, bson);
    unionWith = DocumentSourceUnionWith::createFromBson(serializedBson.firstElement(), expCtx);
    ASSERT(unionWith != nullptr);
    ASSERT(unionWith->isInstanceOf<DocumentSourceUnionWith>());
}

TEST_F(DocumentSourceUnionWithTest, QueryStatsSerializeWithSameDBOmitsDbField) {
    auto expCtx = getExpCtx();
    NamespaceString nsToUnionWith = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "coll");
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{nsToUnionWith, {nsToUnionWith, std::vector<BSONObj>()}}});
    auto bson =
        BSON("$unionWith" << BSON("coll" << nsToUnionWith.coll() << "pipeline"
                                         << BSON_ARRAY(BSON("$addFields" << BSON("a" << 3)))));
    auto unionWith = DocumentSourceUnionWith::createFromBson(bson.firstElement(), expCtx);

    // Serialize with query stats options that transform identifiers.
    auto opts = query_shape::SerializationOptions::kMarkIdentifiers_FOR_TEST;
    std::vector<Value> serializedArray;
    unionWith->serializeToArray(serializedArray, opts);
    auto serializedBson = serializedArray[0].getDocument().toBson();

    // Same-database $unionWith should NOT include the "db" field.
    auto unionWithSpec = serializedBson["$unionWith"].Obj();
    ASSERT_FALSE(unionWithSpec.hasField("db")) << "Unexpected 'db' field in query stats "
                                                  "serialization for same-database $unionWith: "
                                               << serializedBson;
    ASSERT_EQ(unionWithSpec["coll"].String(), "HASH<coll>");
}

TEST_F(DocumentSourceUnionWithTest, SerializeAndParseWithoutPipeline) {
    auto expCtx = getExpCtx();
    NamespaceString nsToUnionWith = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "coll");
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{nsToUnionWith, {nsToUnionWith, std::vector<BSONObj>()}}});
    auto bson = BSON("$unionWith" << nsToUnionWith.coll());
    auto desugaredBson =
        BSON("$unionWith" << BSON("coll" << nsToUnionWith.coll() << "pipeline" << BSONArray()));
    auto unionWith = DocumentSourceUnionWith::createFromBson(bson.firstElement(), expCtx);
    ASSERT(unionWith->isInstanceOf<DocumentSourceUnionWith>());
    std::vector<Value> serializedArray;
    unionWith->serializeToArray(serializedArray);
    auto serializedBson = serializedArray[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedBson, desugaredBson);
    unionWith = DocumentSourceUnionWith::createFromBson(serializedBson.firstElement(), expCtx);
    ASSERT(unionWith != nullptr);
    ASSERT(unionWith->isInstanceOf<DocumentSourceUnionWith>());
}

TEST_F(DocumentSourceUnionWithTest, SerializeAndParseWithoutPipelineExtraSubobject) {
    auto expCtx = getExpCtx();
    NamespaceString nsToUnionWith = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "coll");
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{nsToUnionWith, {nsToUnionWith, std::vector<BSONObj>()}}});
    auto bson = BSON("$unionWith" << BSON("coll" << nsToUnionWith.coll()));
    auto desugaredBson =
        BSON("$unionWith" << BSON("coll" << nsToUnionWith.coll() << "pipeline" << BSONArray()));
    auto unionWith = DocumentSourceUnionWith::createFromBson(bson.firstElement(), expCtx);
    ASSERT(unionWith->isInstanceOf<DocumentSourceUnionWith>());
    std::vector<Value> serializedArray;
    unionWith->serializeToArray(serializedArray);
    auto serializedBson = serializedArray[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedBson, desugaredBson);
    unionWith = DocumentSourceUnionWith::createFromBson(serializedBson.firstElement(), getExpCtx());
    ASSERT(unionWith != nullptr);
    ASSERT(unionWith->isInstanceOf<DocumentSourceUnionWith>());
}

TEST_F(DocumentSourceUnionWithTest, ParseErrors) {
    auto expCtx = getExpCtx();
    NamespaceString nsToUnionWith = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "coll");
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{nsToUnionWith, {nsToUnionWith, std::vector<BSONObj>()}}});
    ASSERT_THROWS_CODE(
        DocumentSourceUnionWith::createFromBson(BSON("$unionWith" << false).firstElement(), expCtx),
        AssertionException,
        ErrorCodes::FailedToParse);
    ASSERT_THROWS_CODE(DocumentSourceUnionWith::createFromBson(
                           BSON("$unionWith" << BSON("coll" << nsToUnionWith.coll() << "coll"
                                                            << "bar"))
                               .firstElement(),
                           expCtx),
                       AssertionException,
                       ErrorCodes::IDLDuplicateField);
    ASSERT_THROWS_CODE(
        DocumentSourceUnionWith::createFromBson(
            BSON("$unionWith" << BSON("coll" << nsToUnionWith.coll() << "pipeline"
                                             << BSON_ARRAY(BSON("$addFields" << BSON("a" << 3)))
                                             << "coll"
                                             << "bar"))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::IDLDuplicateField);
    ASSERT_THROWS_CODE(
        DocumentSourceUnionWith::createFromBson(
            BSON("$unionWith" << BSON("coll" << nsToUnionWith.coll() << "pipeline"
                                             << BSON_ARRAY(BSON("$addFields" << BSON("a" << 3)))
                                             << "myDog"
                                             << "bar"))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::IDLUnknownField);
    ASSERT_THROWS_CODE(DocumentSourceUnionWith::createFromBson(
                           BSON("$unionWith" << BSON(
                                    "coll" << nsToUnionWith.coll() << "pipeline"
                                           << BSON_ARRAY(BSON("$petMyDog" << BSON("myDog" << 3)))))
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       40324);
    ASSERT_THROWS_CODE(
        DocumentSourceUnionWith::createFromBson(
            BSON("$unionWith" << BSON("coll" << BSON("not" << "string") << "pipeline"
                                             << BSON_ARRAY(BSON("$addFields" << BSON("a" << 3)))))
                .firstElement(),
            getExpCtx()),
        AssertionException,
        ErrorCodes::TypeMismatch);
    ASSERT_THROWS_CODE(DocumentSourceUnionWith::createFromBson(
                           BSON("$unionWith" << BSON("coll" << nsToUnionWith.coll() << "pipeline"
                                                            << "string"))
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
    ASSERT_THROWS_CODE(DocumentSourceUnionWith::createFromBson(
                           BSON("$unionWith" << BSON("coll" << nsToUnionWith.coll() << "pipeline"
                                                            << BSON("not" << "string")))
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceUnionWithTest, RejectsUserSuppliedIsHybridSearchWhenExtensionsFlagOn) {
    // When featureFlagExtensionsInsideHybridSearch is on, the stage-params dispatch path is taken
    // instead of createFromBson, and must equally reject a user-supplied $_internalIsHybridSearch.
    auto ifrCtx = std::make_shared<IncrementalFeatureRolloutContext>(std::vector<BSONObj>{
        BSON("name" << "featureFlagExtensionsInsideHybridSearch" << "value" << true)});

    // A client with a transport session and no internal tag is an external (user) client.
    auto client = getServiceContext()->getService()->makeClient(
        "external", transport::MockSession::create(/*transportLayer=*/nullptr));
    auto opCtx = client->makeOperationContext();
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx.get())
                      .ns(getExpCtx()->getNamespaceString())
                      .ifrContext(ifrCtx)
                      .build();

    const std::vector<BSONObj> rawPipeline = {
        BSON("$unionWith" << BSON("coll" << "coll" << "pipeline" << BSONArray()
                                         << "$_internalIsHybridSearch" << true))};
    LiteParsedPipeline liteParsedPipeline(
        expCtx->getNamespaceString(), rawPipeline, false, LiteParserOptions{.ifrContext = ifrCtx});
    ASSERT_THROWS_CODE(
        Pipeline::parseFromLiteParsed(liteParsedPipeline, expCtx), AssertionException, 5491300);
}

TEST_F(DocumentSourceUnionWithTest, PropagatePauses) {
    const auto mock =
        exec::agg::MockStage::createForTest({Document(),
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document(),
                                             DocumentSource::GetNextResult::makePauseExecution()},
                                            getExpCtx());
    const auto mockDeque = std::deque<DocumentSource::GetNextResult>{};
    getExpCtx()->setMongoProcessInterface(std::make_unique<MockMongoInterface>(mockDeque));
    auto unionWithOne = exec::agg::buildStage(makeUnion(
        getExpCtx(),
        Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx())));
    auto unionWithTwo = exec::agg::buildStage(makeUnion(
        getExpCtx(),
        Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx())));
    exec::agg::MockStage::setSource_forTest(unionWithOne, mock.get());
    exec::agg::MockStage::setSource_forTest(unionWithTwo, unionWithOne.get());

    ASSERT_TRUE(unionWithTwo->getNext().isAdvanced());
    ASSERT_TRUE(unionWithTwo->getNext().isPaused());
    ASSERT_TRUE(unionWithTwo->getNext().isAdvanced());
    ASSERT_TRUE(unionWithTwo->getNext().isPaused());

    ASSERT_TRUE(unionWithTwo->getNext().isEOF());
    ASSERT_TRUE(unionWithTwo->getNext().isEOF());
    ASSERT_TRUE(unionWithTwo->getNext().isEOF());

    unionWithTwo->dispose();
}

TEST_F(DocumentSourceUnionWithTest, ReturnEOFAfterBeingDisposed) {
    const auto mockInput =
        exec::agg::MockStage::createForTest({Document(), Document()}, getExpCtx());
    const auto mockUnionInput = std::deque<DocumentSource::GetNextResult>{};
    const auto mockCtx = makeCopyFromExpressionContext(getExpCtx(), {});
    mockCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>(mockUnionInput));
    auto unionWith = exec::agg::buildStage(make_intrusive<DocumentSourceUnionWith>(
        mockCtx, Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx())));
    exec::agg::MockStage::setSource_forTest(unionWith, mockInput.get());

    ASSERT_TRUE(unionWith->getNext().isAdvanced());

    unionWith->dispose();
    ASSERT_TRUE(unionWith->getNext().isEOF());
    ASSERT_TRUE(unionWith->getNext().isEOF());
    ASSERT_TRUE(unionWith->getNext().isEOF());

    unionWith->dispose();
}

TEST_F(DocumentSourceUnionWithTest, DependencyAnalysisReportsFullDoc) {
    auto expCtx = getExpCtx();
    const auto replaceRoot = DocumentSourceReplaceRoot::createFromBson(
        BSON("$replaceRoot" << BSON("newRoot" << "$b")).firstElement(), expCtx);
    const auto unionWith = make_intrusive<DocumentSourceUnionWith>(
        expCtx, Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, expCtx));

    // With the $unionWith *before* the $replaceRoot, the dependency analysis will report that some
    // fields are needed
    auto pipeline = Pipeline::create({unionWith, replaceRoot}, expCtx);

    auto deps = pipeline->getDependencies(DepsTracker::kNoMetadata);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSON("b" << 1 << "_id" << 0));
    ASSERT_TRUE(!deps.needWholeDocument);
}

TEST_F(DocumentSourceUnionWithTest, DependencyAnalysisReportsReferencedFieldsBeforeUnion) {
    auto expCtx = getExpCtx();

    const auto replaceRoot = DocumentSourceReplaceRoot::createFromBson(
        BSON("$replaceRoot" << BSON("newRoot" << "$b")).firstElement(), expCtx);
    const auto unionWith = make_intrusive<DocumentSourceUnionWith>(
        expCtx, Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, expCtx));

    // With the $unionWith *after* the $replaceRoot, the dependency analysis will now report only
    // the referenced fields.
    auto pipeline = Pipeline::create({replaceRoot, unionWith}, expCtx);

    auto deps = pipeline->getDependencies(DepsTracker::kNoMetadata);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSON("b" << 1 << "_id" << 0));
    ASSERT_FALSE(deps.needWholeDocument);
}

class MockMongoInterfaceWithQueryExecution : public StubMongoProcessInterface {
public:
    explicit MockMongoInterfaceWithQueryExecution(
        std::deque<DocumentSource::GetNextResult> mockResults)
        : _mockResults(std::move(mockResults)) {}

    bool isExpectedToExecuteQueries() override {
        return true;
    }

    std::unique_ptr<Pipeline> attachCursorSourceToPipelineForLocalRead(
        std::unique_ptr<Pipeline> pipeline,
        boost::optional<const AggregateCommandRequest&> aggRequest = boost::none,
        bool shouldUseCollectionDefaultCollator = false) override {
        pipeline->addInitialSource(
            DocumentSourceMock::createForTest(_mockResults, pipeline->getContext()));
        return pipeline;
    }

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        std::unique_ptr<Pipeline> pipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) override {
        return attachCursorSourceToPipelineForLocalRead(std::move(pipeline));
    }

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const AggregateCommandRequest& aggRequest,
        std::unique_ptr<Pipeline> pipeline,
        boost::optional<BSONObj> shardCursorsSortSpec = boost::none,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none,
        bool shouldUseCollectionDefaultCollator = false) override {
        return attachCursorSourceToPipelineForLocalRead(std::move(pipeline));
    }

    std::unique_ptr<Pipeline> finalizeAndMaybePreparePipelineForExecution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::unique_ptr<Pipeline> pipeline,
        bool attachCursorAfterOptimizing,
        std::function<void(Pipeline* pipeline)> optimizePipeline = nullptr,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none,
        bool shouldUseCollectionDefaultCollator = false) override {
        if (optimizePipeline) {
            optimizePipeline(pipeline.get());
        }
        if (attachCursorAfterOptimizing) {
            return attachCursorSourceToPipelineForLocalRead(std::move(pipeline));
        }
        return pipeline;
    }

private:
    std::deque<DocumentSource::GetNextResult> _mockResults;
};

TEST_F(DocumentSourceUnionWithTest, RespectsViewDefinition) {
    auto expCtx = getExpCtx();
    NamespaceString nsToUnionWith = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {nsToUnionWith,
         {nsToUnionWith, std::vector<BSONObj>{fromjson("{$match: {_id: {$mod: [2, 0]}}}")}}}});

    // Mock out the foreign collection.
    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 1}},
                                                                  Document{{"_id", 2}}};
    expCtx->setMongoProcessInterface(
        std::make_shared<MockMongoInterfaceWithQueryExecution>(std::move(mockForeignContents)));

    const auto localMock =
        exec::agg::MockStage::createForTest({Document{{"_id"sv, "local"sv}}}, getExpCtx());
    auto bson = BSON("$unionWith" << nsToUnionWith.coll());
    auto unionWith =
        exec::agg::buildStage(DocumentSourceUnionWith::createFromBson(bson.firstElement(), expCtx));
    exec::agg::MockStage::setSource_forTest(unionWith, localMock.get());

    auto result = unionWith->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_DOCUMENT_EQ(result.getDocument(), (Document{{"_id"sv, "local"sv}}));

    result = unionWith->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_DOCUMENT_EQ(result.getDocument(), (Document{{"_id"sv, 2}}));

    ASSERT_TRUE(unionWith->getNext().isEOF());

    unionWith->dispose();
}

TEST_F(DocumentSourceUnionWithTest, ConcatenatesViewDefinitionToPipeline) {
    auto expCtx = getExpCtx();
    NamespaceString viewNsToUnionWith = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "view");
    NamespaceString nsToUnionWith = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {viewNsToUnionWith,
         {nsToUnionWith, std::vector<BSONObj>{fromjson("{$match: {_id: {$mod: [2, 0]}}}")}}}});

    // Mock out the foreign collection.
    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 1}},
                                                                  Document{{"_id", 2}}};
    expCtx->setMongoProcessInterface(
        std::make_shared<MockMongoInterfaceWithQueryExecution>(std::move(mockForeignContents)));

    const auto localMock =
        exec::agg::MockStage::createForTest({Document{{"_id"sv, "local"sv}}}, getExpCtx());
    auto bson = BSON("$unionWith" << BSON(
                         "coll" << viewNsToUnionWith.coll() << "pipeline"
                                << BSON_ARRAY(fromjson(
                                       "{$set: {originalId: '$_id', _id: {$add: [1, '$_id']}}}"))));
    auto unionWith =
        exec::agg::buildStage(DocumentSourceUnionWith::createFromBson(bson.firstElement(), expCtx));
    exec::agg::MockStage::setSource_forTest(unionWith, localMock.get());

    auto result = unionWith->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_DOCUMENT_EQ(result.getDocument(), (Document{{"_id"sv, "local"sv}}));

    result = unionWith->getNext();
    ASSERT_TRUE(result.isAdvanced());
    // Assert we get the document that originally had an even _id. Note this proves that the view
    // definition was _prepended_ on the pipeline, which is important.
    ASSERT_DOCUMENT_EQ(result.getDocument(), (Document{{"_id"sv, 3}, {"originalId"sv, 2}}));

    ASSERT_TRUE(unionWith->getNext().isEOF());

    unionWith->dispose();
}

TEST_F(DocumentSourceUnionWithTest, RejectUnionWhenDepthLimitIsExceeded) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    expCtx->setSubPipelineDepth(internalMaxSubPipelineViewDepth.load());

    ASSERT_THROWS_CODE(
        DocumentSourceUnionWith::createFromBson(
            BSON("$unionWith" << BSON("coll" << fromNs.coll() << "pipeline"
                                             << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::MaxSubPipelineDepthExceeded);
}

TEST_F(DocumentSourceUnionWithTest, ConstraintsWithoutPipelineAreCorrect) {
    auto emptyUnion =
        makeUnion(getExpCtx(),
                  Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx()));
    StageConstraints defaultConstraints(StageConstraints::StreamType::kStreaming,
                                        StageConstraints::PositionRequirement::kNone,
                                        StageConstraints::HostTypeRequirement::kTargetedShards,
                                        StageConstraints::DiskUseRequirement::kNoDiskUse,
                                        StageConstraints::FacetRequirement::kAllowed,
                                        StageConstraints::TransactionRequirement::kNotAllowed,
                                        StageConstraints::LookupRequirement::kAllowed,
                                        StageConstraints::UnionRequirement::kAllowed);
    ASSERT_TRUE(emptyUnion->constraints(PipelineSplitState::kUnsplit) == defaultConstraints);
}

TEST_F(DocumentSourceUnionWithTest, ConstraintsWithMixedSubPipelineAreCorrect) {
    const auto mock = DocumentSourceMock::createForTest({}, getExpCtx());
    StageConstraints stricterConstraint(StageConstraints::StreamType::kStreaming,
                                        StageConstraints::PositionRequirement::kNone,
                                        StageConstraints::HostTypeRequirement::kTargetedShards,
                                        StageConstraints::DiskUseRequirement::kNoDiskUse,
                                        StageConstraints::FacetRequirement::kNotAllowed,
                                        StageConstraints::TransactionRequirement::kNotAllowed,
                                        StageConstraints::LookupRequirement::kNotAllowed,
                                        StageConstraints::UnionRequirement::kAllowed);
    mock->mockConstraints = stricterConstraint;
    auto unionWithOne = DocumentSourceUnionWith(
        getExpCtx(),
        Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{mock}, getExpCtx()));
    ASSERT_TRUE(unionWithOne.constraints(PipelineSplitState::kUnsplit) == stricterConstraint);
}

TEST_F(DocumentSourceUnionWithTest, ConstraintsWithStrictSubPipelineAreCorrect) {
    const auto mockOne = DocumentSourceMock::createForTest({}, getExpCtx());
    StageConstraints constraintTmpDataFacetLookupNotAllowedNoFieldMod(
        StageConstraints::StreamType::kStreaming,
        StageConstraints::PositionRequirement::kNone,
        StageConstraints::HostTypeRequirement::kTargetedShards,
        StageConstraints::DiskUseRequirement::kWritesTmpData,
        StageConstraints::FacetRequirement::kNotAllowed,
        StageConstraints::TransactionRequirement::kAllowed,
        StageConstraints::LookupRequirement::kNotAllowed,
        StageConstraints::UnionRequirement::kAllowed);
    mockOne->mockConstraints = constraintTmpDataFacetLookupNotAllowedNoFieldMod;
    const auto mockTwo = DocumentSourceMock::createForTest({}, getExpCtx());
    StageConstraints constraintPermissive(StageConstraints::StreamType::kStreaming,
                                          StageConstraints::PositionRequirement::kNone,
                                          StageConstraints::HostTypeRequirement::kNone,
                                          StageConstraints::DiskUseRequirement::kNoDiskUse,
                                          StageConstraints::FacetRequirement::kAllowed,
                                          StageConstraints::TransactionRequirement::kAllowed,
                                          StageConstraints::LookupRequirement::kAllowed,
                                          StageConstraints::UnionRequirement::kAllowed);
    mockTwo->mockConstraints = constraintPermissive;
    const auto mockThree = DocumentSourceMock::createForTest({}, getExpCtx());
    StageConstraints constraintPersistentDataTransactionLookupNotAllowed(
        StageConstraints::StreamType::kStreaming,
        StageConstraints::PositionRequirement::kNone,
        StageConstraints::HostTypeRequirement::kNone,
        StageConstraints::DiskUseRequirement::kWritesPersistentData,
        StageConstraints::FacetRequirement::kAllowed,
        StageConstraints::TransactionRequirement::kNotAllowed,
        StageConstraints::LookupRequirement::kNotAllowed,
        StageConstraints::UnionRequirement::kAllowed);
    mockThree->mockConstraints = constraintPersistentDataTransactionLookupNotAllowed;
    auto unionStage = DocumentSourceUnionWith(
        getExpCtx(),
        Pipeline::create(
            std::list<boost::intrusive_ptr<DocumentSource>>{mockOne, mockTwo, mockThree},
            getExpCtx()));
    StageConstraints strict(StageConstraints::StreamType::kStreaming,
                            StageConstraints::PositionRequirement::kNone,
                            StageConstraints::HostTypeRequirement::kTargetedShards,
                            StageConstraints::DiskUseRequirement::kWritesPersistentData,
                            StageConstraints::FacetRequirement::kNotAllowed,
                            StageConstraints::TransactionRequirement::kNotAllowed,
                            StageConstraints::LookupRequirement::kNotAllowed,
                            StageConstraints::UnionRequirement::kAllowed);
    ASSERT_TRUE(unionStage.constraints(PipelineSplitState::kUnsplit) == strict);
}

TEST_F(DocumentSourceUnionWithTest, StricterConstraintsFromSubSubPipelineAreInherited) {
    const auto mock = DocumentSourceMock::createForTest({}, getExpCtx());
    StageConstraints strictConstraint(StageConstraints::StreamType::kStreaming,
                                      StageConstraints::PositionRequirement::kNone,
                                      StageConstraints::HostTypeRequirement::kTargetedShards,
                                      StageConstraints::DiskUseRequirement::kNoDiskUse,
                                      StageConstraints::FacetRequirement::kAllowed,
                                      StageConstraints::TransactionRequirement::kNotAllowed,
                                      StageConstraints::LookupRequirement::kNotAllowed,
                                      StageConstraints::UnionRequirement::kAllowed);
    mock->mockConstraints = strictConstraint;
    auto facetPipeline = Pipeline::create({mock}, getExpCtx());
    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("pipeline", std::move(facetPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), getExpCtx());
    auto unionStage = DocumentSourceUnionWith(
        getExpCtx(),
        Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{facetStage}, getExpCtx()));
    StageConstraints expectedConstraints(StageConstraints::StreamType::kStreaming,
                                         StageConstraints::PositionRequirement::kNone,
                                         StageConstraints::HostTypeRequirement::kTargetedShards,
                                         StageConstraints::DiskUseRequirement::kNoDiskUse,
                                         StageConstraints::FacetRequirement::kNotAllowed,
                                         StageConstraints::TransactionRequirement::kNotAllowed,
                                         StageConstraints::LookupRequirement::kNotAllowed,
                                         StageConstraints::UnionRequirement::kAllowed);
    ASSERT_TRUE(unionStage.constraints(PipelineSplitState::kUnsplit) == expectedConstraints);
}

TEST_F(DocumentSourceUnionWithTest, IncrementNestedAggregateOpCounterOnCreateButNotOnCopy) {
    auto testOpCounter = [&](const NamespaceString& nss, const int expectedIncrease) {
        auto resolvedNss = ResolvedNamespaceMap{{nss, {nss, std::vector<BSONObj>()}}};
        auto countBeforeCreate = globalOpCounters().nestedAggregates->value();

        // Create a DocumentSourceUnionWith and verify that the counter increases by the expected
        // amount.
        auto originalExpCtx = make_intrusive<ExpressionContextForTest>(getOpCtx(), nss);
        originalExpCtx->setResolvedNamespaces(resolvedNss);
        auto docSource = DocumentSourceUnionWith::createFromBson(
            BSON("$unionWith" << BSON("coll" << nss.coll() << "pipeline"
                                             << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))))
                .firstElement(),
            originalExpCtx);
        auto originalUnionWith = static_cast<DocumentSourceUnionWith*>(docSource.get());
        auto countAfterCreate = globalOpCounters().nestedAggregates->value();
        ASSERT_EQ(countAfterCreate - countBeforeCreate, expectedIncrease);

        // Copy the DocumentSourceUnionWith and verify that the counter doesn't increase.
        auto newExpCtx = make_intrusive<ExpressionContextForTest>(getOpCtx(), nss);
        newExpCtx->setResolvedNamespaces(resolvedNss);
        DocumentSourceUnionWith newUnionWith{*originalUnionWith, newExpCtx};
        auto countAfterCopy = globalOpCounters().nestedAggregates->value();
        ASSERT_EQ(countAfterCopy - countAfterCreate, 0);
    };

    testOpCounter(NamespaceString::createNamespaceString_forTest("testDb", "testColl"), 1);
    // $unionWith against internal databases should not cause the counter to get incremented.
    testOpCounter(NamespaceString::createNamespaceString_forTest("config", "testColl"), 0);
    testOpCounter(NamespaceString::createNamespaceString_forTest("admin", "testColl"), 0);
    testOpCounter(NamespaceString::createNamespaceString_forTest("local", "testColl"), 0);
}

TEST_F(DocumentSourceUnionWithTest, RedactsCorrectlyBasic) {
    auto expCtx = getExpCtx();
    auto nsToUnionWith = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "coll");
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{nsToUnionWith, {nsToUnionWith, std::vector<BSONObj>()}}});

    auto docSource = DocumentSourceUnionWith::createFromBson(
        BSON("$unionWith" << nsToUnionWith.coll()).firstElement(), expCtx);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$unionWith": {
                "coll": "HASH<coll>",
                "pipeline": []
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceUnionWithTest, RedactsCorrectlyWithPipeline) {
    auto expCtx = getExpCtx();
    auto nsToUnionWith = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "coll");
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{nsToUnionWith, {nsToUnionWith, std::vector<BSONObj>()}}});

    BSONArrayBuilder pipeline;
    pipeline << BSON("$match" << BSON("a" << 15));
    pipeline << BSON("$project" << BSON("a" << 1 << "b" << 1));
    auto docSource = DocumentSourceUnionWith::createFromBson(
        BSON("$unionWith" << BSON("coll" << nsToUnionWith.coll() << "pipeline" << pipeline.arr()))
            .firstElement(),
        expCtx);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$unionWith": {
                "coll": "HASH<coll>",
                "pipeline": [
                    {
                        "$match": {
                            "HASH<a>": {
                                "$eq": "?number"
                            }
                        }
                    },
                    {
                        "$project": {
                            "HASH<_id>": true,
                            "HASH<a>": true,
                            "HASH<b>": true
                        }
                    }
                ]
            }
        })",
        redact(*docSource));
}

using DocumentSourceUnionWithServerlessTest = ServerlessAggregationContextFixture;

TEST_F(DocumentSourceUnionWithServerlessTest,
       LiteParsedDocumentSourceLookupContainsExpectedNamespacesInServerless) {
    unittest::ServerParameterGuard multitenancyController("multitenancySupport", true);

    auto tenantId = TenantId(OID::gen());
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(tenantId, "test", "testColl");
    std::vector<BSONObj> pipeline;

    auto stageSpec = BSON("$unionWith" << "some_coll");
    auto liteParsedUnionWith =
        LiteParsedUnionWith::parse(nss, stageSpec.firstElement(), LiteParserOptions{});
    auto namespaceSet = liteParsedUnionWith->getInvolvedNamespaces();
    ASSERT_EQ(1, namespaceSet.size());
    ASSERT_EQ(1ul,
              namespaceSet.count(
                  NamespaceString::createNamespaceString_forTest(tenantId, "test", "some_coll")));

    stageSpec = BSON("$unionWith" << BSON("coll" << "some_coll"
                                                 << "pipeline" << BSONArray()));
    liteParsedUnionWith =
        LiteParsedUnionWith::parse(nss, stageSpec.firstElement(), LiteParserOptions{});
    namespaceSet = liteParsedUnionWith->getInvolvedNamespaces();
    ASSERT_EQ(1, namespaceSet.size());
    ASSERT_EQ(1ul,
              namespaceSet.count(
                  NamespaceString::createNamespaceString_forTest(tenantId, "test", "some_coll")));
}

TEST_F(DocumentSourceUnionWithServerlessTest,
       CreateFromBSONContainsExpectedNamespacesInServerless) {
    unittest::ServerParameterGuard multitenancyController("multitenancySupport", true);

    auto expCtx = getExpCtx();
    ASSERT(expCtx->getNamespaceString().tenantId());

    NamespaceString unionWithNs = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().tenantId(), "test", "some_coll");
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{unionWithNs, {unionWithNs, std::vector<BSONObj>()}}});

    auto spec = BSON("$unionWith" << "some_coll");
    auto unionWithStage = DocumentSourceUnionWith::createFromBson(spec.firstElement(), expCtx);
    auto pipeline =
        Pipeline::create({DocumentSourceMock::createForTest({}, expCtx), unionWithStage}, expCtx);
    auto involvedNssSet = pipeline->getInvolvedCollections();
    ASSERT_EQ(involvedNssSet.size(), 1UL);
    ASSERT_EQ(1ul, involvedNssSet.count(unionWithNs));

    spec = BSON("$unionWith" << BSON("coll" << "some_coll"
                                            << "pipeline" << BSONArray()));
    unionWithStage = DocumentSourceUnionWith::createFromBson(spec.firstElement(), expCtx);
    pipeline =
        Pipeline::create({DocumentSourceMock::createForTest({}, expCtx), unionWithStage}, expCtx);
    involvedNssSet = pipeline->getInvolvedCollections();
    ASSERT_EQ(involvedNssSet.size(), 1UL);
    ASSERT_EQ(1ul, involvedNssSet.count(unionWithNs));
}

// ---- Tests for custom UnionWithStageParams and builder function ----

TEST_F(DocumentSourceUnionWithTest, StageParamsCarriesParsedData) {
    auto expCtx = getExpCtx();
    NamespaceString nss = expCtx->getNamespaceString();

    // Object spec with pipeline: carries namespace, pipeline BSON, and desugared LPP.
    {
        auto bson =
            BSON("$unionWith" << BSON("coll" << "target_coll"
                                             << "pipeline"
                                             << BSON_ARRAY(BSON("$addFields" << BSON("a" << 1)))));
        auto liteParsed = LiteParsedUnionWith::parse(nss, bson.firstElement(), LiteParserOptions{});
        auto stageParams = liteParsed->getStageParams();
        auto* params = dynamic_cast<UnionWithStageParams*>(stageParams.get());
        ASSERT(params);
        ASSERT_EQ(params->unionNss.coll(), "target_coll");
        ASSERT_EQ(params->unionNss.dbName(), nss.dbName());
        ASSERT_EQ(params->pipeline.size(), 1U);
        ASSERT_TRUE(params->subpipelineStageParams.has_value());
        ASSERT_EQ(params->subpipelineStageParams->size(), 1U);
    }

    // String spec (collection name only): no subpipelineStageParams since there's no subpipeline.
    {
        auto bson = BSON("$unionWith" << "target_coll");
        auto liteParsed = LiteParsedUnionWith::parse(nss, bson.firstElement(), LiteParserOptions{});
        auto stageParams = liteParsed->getStageParams();
        auto* params = dynamic_cast<UnionWithStageParams*>(stageParams.get());
        ASSERT(params);
        ASSERT_EQ(params->unionNss.coll(), "target_coll");
        ASSERT_TRUE(params->pipeline.empty());
        ASSERT_FALSE(params->subpipelineStageParams.has_value());
    }

    // Object spec with empty pipeline: LPP present but has zero stages.
    {
        auto bson = BSON("$unionWith" << BSON("coll" << "target_coll"
                                                     << "pipeline" << BSONArray()));
        auto liteParsed = LiteParsedUnionWith::parse(nss, bson.firstElement(), LiteParserOptions{});
        auto stageParams = liteParsed->getStageParams();
        auto* params = dynamic_cast<UnionWithStageParams*>(stageParams.get());
        ASSERT(params);
        ASSERT_TRUE(params->subpipelineStageParams.has_value());
        ASSERT_TRUE(params->subpipelineStageParams->empty());
    }

    // Multi-stage pipeline: subpipelineStageParams carries all stages.
    {
        auto bson = BSON("$unionWith"
                         << BSON("coll" << "target_coll"
                                        << "pipeline"
                                        << BSON_ARRAY(BSON("$match" << BSON("x" << 1))
                                                      << BSON("$addFields" << BSON("y" << 2)))));
        auto liteParsed = LiteParsedUnionWith::parse(nss, bson.firstElement(), LiteParserOptions{});
        auto stageParams = liteParsed->getStageParams();
        auto* params = dynamic_cast<UnionWithStageParams*>(stageParams.get());
        ASSERT(params);
        ASSERT_TRUE(params->subpipelineStageParams.has_value());
        ASSERT_EQ(params->subpipelineStageParams->size(), 2U);
    }
}

TEST_F(DocumentSourceUnionWithTest, BuilderRoundTripMatchesCreateFromBson) {
    auto expCtx = getExpCtx();

    auto verifyRoundTrip = [&](const BSONObj& bson, const NamespaceString& nsToUnionWith) {
        expCtx->setResolvedNamespaces(
            ResolvedNamespaceMap{{nsToUnionWith, {nsToUnionWith, std::vector<BSONObj>()}}});

        auto liteParsed = LiteParsedUnionWith::parse(
            expCtx->getNamespaceString(), bson.firstElement(), LiteParserOptions{});
        auto docSources = buildDocumentSource(*liteParsed, expCtx);
        ASSERT_EQ(docSources.size(), 1U);
        ASSERT(docSources.front()->isInstanceOf<DocumentSourceUnionWith>());

        std::vector<Value> builderSerialized;
        docSources.front()->serializeToArray(builderSerialized);

        auto fromBson = DocumentSourceUnionWith::createFromBson(bson.firstElement(), expCtx);
        std::vector<Value> legacySerialized;
        fromBson->serializeToArray(legacySerialized);

        ASSERT_EQ(builderSerialized.size(), legacySerialized.size());
        for (size_t i = 0; i < builderSerialized.size(); ++i) {
            ASSERT_VALUE_EQ(builderSerialized[i], legacySerialized[i]);
        }
    };

    // With pipeline.
    {
        auto nsToUnionWith = NamespaceString::createNamespaceString_forTest(
            expCtx->getNamespaceString().dbName(), "target_coll");
        verifyRoundTrip(
            BSON("$unionWith" << BSON(
                     "coll" << "target_coll"
                            << "pipeline"
                            << BSON_ARRAY(BSON("$addFields" << BSON("a" << BSON("$const" << 3)))))),
            nsToUnionWith);
    }

    // String spec.
    {
        auto nsToUnionWith = NamespaceString::createNamespaceString_forTest(
            expCtx->getNamespaceString().dbName(), "target_coll");
        verifyRoundTrip(BSON("$unionWith" << "target_coll"), nsToUnionWith);
    }
}

}  // namespace
}  // namespace mongo
