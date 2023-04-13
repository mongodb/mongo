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
#include "mongo/db/pipeline/document_source_facet.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/stub_lookup_single_document_process_interface.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {
namespace {

using MockMongoInterface = StubLookupSingleDocumentProcessInterface;

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceUnionWithTest = AggregationContextFixture;

auto makeUnion(const boost::intrusive_ptr<ExpressionContext>& expCtx,
               std::unique_ptr<Pipeline, PipelineDeleter> pipeline) {
    return std::unique_ptr<DocumentSourceUnionWith, DocumentSourceDeleter>(
        new DocumentSourceUnionWith(expCtx, std::move(pipeline)), DocumentSourceDeleter());
}

auto makeUnionFromBson(BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto docSource = DocumentSourceUnionWith::createFromBson(elem, expCtx);
    auto unionWith = static_cast<DocumentSourceUnionWith*>(docSource.detach());
    return std::unique_ptr<DocumentSourceUnionWith, DocumentSourceDeleter>(unionWith,
                                                                           DocumentSourceDeleter());
}

TEST_F(DocumentSourceUnionWithTest, BasicSerialUnions) {
    const auto doc = Document{{"a", 1}};
    const auto mock = DocumentSourceMock::createForTest(doc, getExpCtx());
    const auto mockDeque = std::deque<DocumentSource::GetNextResult>{Document{doc}};
    getExpCtx()->mongoProcessInterface = std::make_unique<MockMongoInterface>(mockDeque);
    auto unionWithOne =
        makeUnion(getExpCtx(),
                  Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx()));
    auto unionWithTwo =
        makeUnion(getExpCtx(),
                  Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx()));
    unionWithOne->setSource(mock.get());
    unionWithTwo->setSource(unionWithOne.get());

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
}

TEST_F(DocumentSourceUnionWithTest, BasicNestedUnions) {
    const auto doc = Document{{"a", 1}};
    const auto mock = DocumentSourceMock::createForTest(doc, getExpCtx());
    const auto mockDeque = std::deque<DocumentSource::GetNextResult>{Document{doc}};
    getExpCtx()->mongoProcessInterface = std::make_unique<MockMongoInterface>(mockDeque);
    auto unionWithOne = make_intrusive<DocumentSourceUnionWith>(
        getExpCtx(),
        Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx()));
    auto unionWithTwo =
        makeUnion(getExpCtx(),
                  Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{unionWithOne},
                                   getExpCtx()));
    unionWithTwo->setSource(mock.get());

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
}

TEST_F(DocumentSourceUnionWithTest, UnionsWithNonEmptySubPipelines) {
    const auto inputDoc = Document{{"a", 1}};
    const auto outputDocs = std::array{Document{{"a", 1}}, Document{{"a", 1}, {"d", 1}}};
    const auto mock = DocumentSourceMock::createForTest(inputDoc, getExpCtx());
    const auto mockDeque = std::deque<DocumentSource::GetNextResult>{Document{inputDoc}};
    getExpCtx()->mongoProcessInterface = std::make_unique<MockMongoInterface>(mockDeque);
    const auto filter = DocumentSourceMatch::create(BSON("d" << 1), getExpCtx());
    const auto proj = DocumentSourceAddFields::create(BSON("d" << 1), getExpCtx());
    auto unionWithOne = makeUnion(
        getExpCtx(),
        Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{filter}, getExpCtx()));
    auto unionWithTwo = makeUnion(
        getExpCtx(),
        Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{proj}, getExpCtx()));
    unionWithOne->setSource(mock.get());
    unionWithTwo->setSource(unionWithOne.get());

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
}

TEST_F(DocumentSourceUnionWithTest, SerializeAndParseWithPipeline) {
    auto expCtx = getExpCtx();
    NamespaceString nsToUnionWith =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {nsToUnionWith.coll().toString(), {nsToUnionWith, std::vector<BSONObj>()}}});
    auto bson =
        BSON("$unionWith" << BSON(
                 "coll" << nsToUnionWith.coll() << "pipeline"
                        << BSON_ARRAY(BSON("$addFields" << BSON("a" << BSON("$const" << 3))))));
    auto unionWith = DocumentSourceUnionWith::createFromBson(bson.firstElement(), expCtx);
    ASSERT(unionWith->getSourceName() == DocumentSourceUnionWith::kStageName);
    std::vector<Value> serializedArray;
    unionWith->serializeToArray(serializedArray);
    auto serializedBson = serializedArray[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedBson, bson);
    unionWith = DocumentSourceUnionWith::createFromBson(serializedBson.firstElement(), expCtx);
    ASSERT(unionWith != nullptr);
    ASSERT(unionWith->getSourceName() == DocumentSourceUnionWith::kStageName);
}

TEST_F(DocumentSourceUnionWithTest, SerializeAndParseWithoutPipeline) {
    auto expCtx = getExpCtx();
    NamespaceString nsToUnionWith =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {nsToUnionWith.coll().toString(), {nsToUnionWith, std::vector<BSONObj>()}}});
    auto bson = BSON("$unionWith" << nsToUnionWith.coll());
    auto desugaredBson =
        BSON("$unionWith" << BSON("coll" << nsToUnionWith.coll() << "pipeline" << BSONArray()));
    auto unionWith = DocumentSourceUnionWith::createFromBson(bson.firstElement(), expCtx);
    ASSERT(unionWith->getSourceName() == DocumentSourceUnionWith::kStageName);
    std::vector<Value> serializedArray;
    unionWith->serializeToArray(serializedArray);
    auto serializedBson = serializedArray[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedBson, desugaredBson);
    unionWith = DocumentSourceUnionWith::createFromBson(serializedBson.firstElement(), expCtx);
    ASSERT(unionWith != nullptr);
    ASSERT(unionWith->getSourceName() == DocumentSourceUnionWith::kStageName);
}

TEST_F(DocumentSourceUnionWithTest, SerializeAndParseWithoutPipelineExtraSubobject) {
    auto expCtx = getExpCtx();
    NamespaceString nsToUnionWith =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {nsToUnionWith.coll().toString(), {nsToUnionWith, std::vector<BSONObj>()}}});
    auto bson = BSON("$unionWith" << BSON("coll" << nsToUnionWith.coll()));
    auto desugaredBson =
        BSON("$unionWith" << BSON("coll" << nsToUnionWith.coll() << "pipeline" << BSONArray()));
    auto unionWith = DocumentSourceUnionWith::createFromBson(bson.firstElement(), expCtx);
    ASSERT(unionWith->getSourceName() == DocumentSourceUnionWith::kStageName);
    std::vector<Value> serializedArray;
    unionWith->serializeToArray(serializedArray);
    auto serializedBson = serializedArray[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedBson, desugaredBson);
    unionWith = DocumentSourceUnionWith::createFromBson(serializedBson.firstElement(), getExpCtx());
    ASSERT(unionWith != nullptr);
    ASSERT(unionWith->getSourceName() == DocumentSourceUnionWith::kStageName);
}

TEST_F(DocumentSourceUnionWithTest, ParseErrors) {
    auto expCtx = getExpCtx();
    NamespaceString nsToUnionWith =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {nsToUnionWith.coll().toString(), {nsToUnionWith, std::vector<BSONObj>()}}});
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
                       40413);
    ASSERT_THROWS_CODE(
        DocumentSourceUnionWith::createFromBson(
            BSON("$unionWith" << BSON("coll" << nsToUnionWith.coll() << "pipeline"
                                             << BSON_ARRAY(BSON("$addFields" << BSON("a" << 3)))
                                             << "coll"
                                             << "bar"))
                .firstElement(),
            expCtx),
        AssertionException,
        40413);
    ASSERT_THROWS_CODE(
        DocumentSourceUnionWith::createFromBson(
            BSON("$unionWith" << BSON("coll" << nsToUnionWith.coll() << "pipeline"
                                             << BSON_ARRAY(BSON("$addFields" << BSON("a" << 3)))
                                             << "myDog"
                                             << "bar"))
                .firstElement(),
            expCtx),
        AssertionException,
        40415);
    ASSERT_THROWS_CODE(DocumentSourceUnionWith::createFromBson(
                           BSON("$unionWith" << BSON(
                                    "coll" << nsToUnionWith.coll() << "pipeline"
                                           << BSON_ARRAY(BSON("$petMyDog" << BSON("myDog" << 3)))))
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       16436);
    ASSERT_THROWS_CODE(
        DocumentSourceUnionWith::createFromBson(
            BSON("$unionWith" << BSON("coll" << BSON("not"
                                                     << "string")
                                             << "pipeline"
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
                                                            << BSON("not"
                                                                    << "string")))
                               .firstElement(),
                           getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceUnionWithTest, PropagatePauses) {
    const auto mock =
        DocumentSourceMock::createForTest({Document(),
                                           DocumentSource::GetNextResult::makePauseExecution(),
                                           Document(),
                                           DocumentSource::GetNextResult::makePauseExecution()},
                                          getExpCtx());
    const auto mockDeque = std::deque<DocumentSource::GetNextResult>{};
    getExpCtx()->mongoProcessInterface = std::make_unique<MockMongoInterface>(mockDeque);
    auto unionWithOne =
        makeUnion(getExpCtx(),
                  Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx()));
    auto unionWithTwo =
        makeUnion(getExpCtx(),
                  Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx()));
    unionWithOne->setSource(mock.get());
    unionWithTwo->setSource(unionWithOne.get());

    ASSERT_TRUE(unionWithTwo->getNext().isAdvanced());
    ASSERT_TRUE(unionWithTwo->getNext().isPaused());
    ASSERT_TRUE(unionWithTwo->getNext().isAdvanced());
    ASSERT_TRUE(unionWithTwo->getNext().isPaused());

    ASSERT_TRUE(unionWithTwo->getNext().isEOF());
    ASSERT_TRUE(unionWithTwo->getNext().isEOF());
    ASSERT_TRUE(unionWithTwo->getNext().isEOF());
}

TEST_F(DocumentSourceUnionWithTest, ReturnEOFAfterBeingDisposed) {
    const auto mockInput = DocumentSourceMock::createForTest({Document(), Document()}, getExpCtx());
    const auto mockUnionInput = std::deque<DocumentSource::GetNextResult>{};
    const auto mockCtx = getExpCtx()->copyWith({});
    mockCtx->mongoProcessInterface = std::make_unique<MockMongoInterface>(mockUnionInput);
    auto unionWith = DocumentSourceUnionWith(
        mockCtx, Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, getExpCtx()));
    unionWith.setSource(mockInput.get());

    ASSERT_TRUE(unionWith.getNext().isAdvanced());

    unionWith.dispose();
    ASSERT_TRUE(unionWith.getNext().isEOF());
    ASSERT_TRUE(unionWith.getNext().isEOF());
    ASSERT_TRUE(unionWith.getNext().isEOF());
}

TEST_F(DocumentSourceUnionWithTest, DependencyAnalysisReportsFullDoc) {
    auto expCtx = getExpCtx();
    const auto replaceRoot =
        DocumentSourceReplaceRoot::createFromBson(BSON("$replaceRoot" << BSON("newRoot"
                                                                              << "$b"))
                                                      .firstElement(),
                                                  expCtx);
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

    const auto replaceRoot =
        DocumentSourceReplaceRoot::createFromBson(BSON("$replaceRoot" << BSON("newRoot"
                                                                              << "$b"))
                                                      .firstElement(),
                                                  expCtx);
    const auto unionWith = make_intrusive<DocumentSourceUnionWith>(
        expCtx, Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{}, expCtx));

    // With the $unionWith *after* the $replaceRoot, the dependency analysis will now report only
    // the referenced fields.
    auto pipeline = Pipeline::create({replaceRoot, unionWith}, expCtx);

    auto deps = pipeline->getDependencies(DepsTracker::kNoMetadata);
    ASSERT_BSONOBJ_EQ(deps.toProjectionWithoutMetadata(), BSON("b" << 1 << "_id" << 0));
    ASSERT_FALSE(deps.needWholeDocument);
}

TEST_F(DocumentSourceUnionWithTest, RespectsViewDefinition) {
    auto expCtx = getExpCtx();
    NamespaceString nsToUnionWith =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {nsToUnionWith.coll().toString(),
         {nsToUnionWith, std::vector<BSONObj>{fromjson("{$match: {_id: {$mod: [2, 0]}}}")}}}});

    // Mock out the foreign collection.
    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 1}},
                                                                  Document{{"_id", 2}}};
    expCtx->mongoProcessInterface =
        std::make_shared<MockMongoInterface>(std::move(mockForeignContents));

    const auto localMock =
        DocumentSourceMock::createForTest({Document{{"_id"_sd, "local"_sd}}}, getExpCtx());
    auto bson = BSON("$unionWith" << nsToUnionWith.coll());
    auto unionWith = makeUnionFromBson(bson.firstElement(), expCtx);
    unionWith->setSource(localMock.get());

    auto result = unionWith->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_DOCUMENT_EQ(result.getDocument(), (Document{{"_id"_sd, "local"_sd}}));

    result = unionWith->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_DOCUMENT_EQ(result.getDocument(), (Document{{"_id"_sd, 2}}));

    ASSERT_TRUE(unionWith->getNext().isEOF());
}

TEST_F(DocumentSourceUnionWithTest, ConcatenatesViewDefinitionToPipeline) {
    auto expCtx = getExpCtx();
    NamespaceString viewNsToUnionWith =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "view");
    NamespaceString nsToUnionWith =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {viewNsToUnionWith.coll().toString(),
         {nsToUnionWith, std::vector<BSONObj>{fromjson("{$match: {_id: {$mod: [2, 0]}}}")}}}});

    // Mock out the foreign collection.
    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 1}},
                                                                  Document{{"_id", 2}}};
    expCtx->mongoProcessInterface =
        std::make_shared<MockMongoInterface>(std::move(mockForeignContents));

    const auto localMock =
        DocumentSourceMock::createForTest({Document{{"_id"_sd, "local"_sd}}}, getExpCtx());
    auto bson = BSON("$unionWith" << BSON(
                         "coll" << viewNsToUnionWith.coll() << "pipeline"
                                << BSON_ARRAY(fromjson(
                                       "{$set: {originalId: '$_id', _id: {$add: [1, '$_id']}}}"))));
    auto unionWith = makeUnionFromBson(bson.firstElement(), expCtx);
    unionWith->setSource(localMock.get());

    auto result = unionWith->getNext();
    ASSERT_TRUE(result.isAdvanced());
    ASSERT_DOCUMENT_EQ(result.getDocument(), (Document{{"_id"_sd, "local"_sd}}));

    result = unionWith->getNext();
    ASSERT_TRUE(result.isAdvanced());
    // Assert we get the document that originally had an even _id. Note this proves that the view
    // definition was _prepended_ on the pipeline, which is important.
    ASSERT_DOCUMENT_EQ(result.getDocument(), (Document{{"_id"_sd, 3}, {"originalId"_sd, 2}}));

    ASSERT_TRUE(unionWith->getNext().isEOF());
}

TEST_F(DocumentSourceUnionWithTest, RejectUnionWhenDepthLimitIsExceeded) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    expCtx->subPipelineDepth = internalMaxSubPipelineViewDepth.load();

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
                                        StageConstraints::HostTypeRequirement::kAnyShard,
                                        StageConstraints::DiskUseRequirement::kNoDiskUse,
                                        StageConstraints::FacetRequirement::kAllowed,
                                        StageConstraints::TransactionRequirement::kNotAllowed,
                                        StageConstraints::LookupRequirement::kAllowed,
                                        StageConstraints::UnionRequirement::kAllowed);
    ASSERT_TRUE(emptyUnion->constraints(Pipeline::SplitState::kUnsplit) == defaultConstraints);
}

TEST_F(DocumentSourceUnionWithTest, ConstraintsWithMixedSubPipelineAreCorrect) {
    const auto mock = DocumentSourceMock::createForTest(getExpCtx());
    StageConstraints stricterConstraint(StageConstraints::StreamType::kStreaming,
                                        StageConstraints::PositionRequirement::kNone,
                                        StageConstraints::HostTypeRequirement::kAnyShard,
                                        StageConstraints::DiskUseRequirement::kNoDiskUse,
                                        StageConstraints::FacetRequirement::kNotAllowed,
                                        StageConstraints::TransactionRequirement::kNotAllowed,
                                        StageConstraints::LookupRequirement::kNotAllowed,
                                        StageConstraints::UnionRequirement::kAllowed);
    mock->mockConstraints = stricterConstraint;
    auto unionWithOne = DocumentSourceUnionWith(
        getExpCtx(),
        Pipeline::create(std::list<boost::intrusive_ptr<DocumentSource>>{mock}, getExpCtx()));
    ASSERT_TRUE(unionWithOne.constraints(Pipeline::SplitState::kUnsplit) == stricterConstraint);
}

TEST_F(DocumentSourceUnionWithTest, ConstraintsWithStrictSubPipelineAreCorrect) {
    const auto mockOne = DocumentSourceMock::createForTest(getExpCtx());
    StageConstraints constraintTmpDataFacetLookupNotAllowed(
        StageConstraints::StreamType::kStreaming,
        StageConstraints::PositionRequirement::kNone,
        StageConstraints::HostTypeRequirement::kAnyShard,
        StageConstraints::DiskUseRequirement::kWritesTmpData,
        StageConstraints::FacetRequirement::kNotAllowed,
        StageConstraints::TransactionRequirement::kAllowed,
        StageConstraints::LookupRequirement::kNotAllowed,
        StageConstraints::UnionRequirement::kAllowed);
    mockOne->mockConstraints = constraintTmpDataFacetLookupNotAllowed;
    const auto mockTwo = DocumentSourceMock::createForTest(getExpCtx());
    StageConstraints constraintPermissive(StageConstraints::StreamType::kStreaming,
                                          StageConstraints::PositionRequirement::kNone,
                                          StageConstraints::HostTypeRequirement::kNone,
                                          StageConstraints::DiskUseRequirement::kNoDiskUse,
                                          StageConstraints::FacetRequirement::kAllowed,
                                          StageConstraints::TransactionRequirement::kAllowed,
                                          StageConstraints::LookupRequirement::kAllowed,
                                          StageConstraints::UnionRequirement::kAllowed);
    mockTwo->mockConstraints = constraintPermissive;
    const auto mockThree = DocumentSourceMock::createForTest(getExpCtx());
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
                            StageConstraints::HostTypeRequirement::kAnyShard,
                            StageConstraints::DiskUseRequirement::kWritesPersistentData,
                            StageConstraints::FacetRequirement::kNotAllowed,
                            StageConstraints::TransactionRequirement::kNotAllowed,
                            StageConstraints::LookupRequirement::kNotAllowed,
                            StageConstraints::UnionRequirement::kAllowed);
    ASSERT_TRUE(unionStage.constraints(Pipeline::SplitState::kUnsplit) == strict);
}
TEST_F(DocumentSourceUnionWithTest, StricterConstraintsFromSubSubPipelineAreInherited) {
    const auto mock = DocumentSourceMock::createForTest(getExpCtx());
    StageConstraints strictConstraint(StageConstraints::StreamType::kStreaming,
                                      StageConstraints::PositionRequirement::kNone,
                                      StageConstraints::HostTypeRequirement::kAnyShard,
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
                                         StageConstraints::HostTypeRequirement::kAnyShard,
                                         StageConstraints::DiskUseRequirement::kNoDiskUse,
                                         StageConstraints::FacetRequirement::kNotAllowed,
                                         StageConstraints::TransactionRequirement::kNotAllowed,
                                         StageConstraints::LookupRequirement::kNotAllowed,
                                         StageConstraints::UnionRequirement::kAllowed);
    ASSERT_TRUE(unionStage.constraints(Pipeline::SplitState::kUnsplit) == expectedConstraints);
}

TEST_F(DocumentSourceUnionWithTest, IncrementNestedAggregateOpCounterOnCreateButNotOnCopy) {
    auto testOpCounter = [&](const NamespaceString& nss, const int expectedIncrease) {
        auto resolvedNss = StringMap<ExpressionContext::ResolvedNamespace>{
            {nss.coll().toString(), {nss, std::vector<BSONObj>()}}};
        auto countBeforeCreate = globalOpCounters.getNestedAggregate()->load();

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
        auto countAfterCreate = globalOpCounters.getNestedAggregate()->load();
        ASSERT_EQ(countAfterCreate - countBeforeCreate, expectedIncrease);

        // Copy the DocumentSourceUnionWith and verify that the counter doesn't increase.
        auto newExpCtx = make_intrusive<ExpressionContextForTest>(getOpCtx(), nss);
        newExpCtx->setResolvedNamespaces(resolvedNss);
        DocumentSourceUnionWith newUnionWith{*originalUnionWith, newExpCtx};
        auto countAfterCopy = globalOpCounters.getNestedAggregate()->load();
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
    auto nsToUnionWith =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {nsToUnionWith.coll().toString(), {nsToUnionWith, std::vector<BSONObj>()}}});

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
    auto nsToUnionWith =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {nsToUnionWith.coll().toString(), {nsToUnionWith, std::vector<BSONObj>()}}});

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
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    auto tenantId = TenantId(OID::gen());
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(tenantId, "test", "testColl");
    std::vector<BSONObj> pipeline;

    auto stageSpec = BSON("$unionWith"
                          << "some_coll");
    auto liteParsedLookup =
        DocumentSourceUnionWith::LiteParsed::parse(nss, stageSpec.firstElement());
    auto namespaceSet = liteParsedLookup->getInvolvedNamespaces();
    ASSERT_EQ(1, namespaceSet.size());
    ASSERT_EQ(1ul,
              namespaceSet.count(
                  NamespaceString::createNamespaceString_forTest(tenantId, "test", "some_coll")));

    stageSpec = BSON("$unionWith" << BSON("coll"
                                          << "some_coll"
                                          << "pipeline" << BSONArray()));
    liteParsedLookup = DocumentSourceUnionWith::LiteParsed::parse(nss, stageSpec.firstElement());
    namespaceSet = liteParsedLookup->getInvolvedNamespaces();
    ASSERT_EQ(1, namespaceSet.size());
    ASSERT_EQ(1ul,
              namespaceSet.count(
                  NamespaceString::createNamespaceString_forTest(tenantId, "test", "some_coll")));
}

TEST_F(DocumentSourceUnionWithServerlessTest,
       CreateFromBSONContainsExpectedNamespacesInServerless) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    auto expCtx = getExpCtx();
    ASSERT(expCtx->ns.tenantId());

    NamespaceString unionWithNs =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.tenantId(), "test", "some_coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {unionWithNs.coll().toString(), {unionWithNs, std::vector<BSONObj>()}}});

    auto spec = BSON("$unionWith"
                     << "some_coll");
    auto unionWithStage = DocumentSourceUnionWith::createFromBson(spec.firstElement(), expCtx);
    auto pipeline =
        Pipeline::create({DocumentSourceMock::createForTest(expCtx), unionWithStage}, expCtx);
    auto involvedNssSet = pipeline->getInvolvedCollections();
    ASSERT_EQ(involvedNssSet.size(), 1UL);
    ASSERT_EQ(1ul, involvedNssSet.count(unionWithNs));

    spec = BSON("$unionWith" << BSON("coll"
                                     << "some_coll"
                                     << "pipeline" << BSONArray()));
    unionWithStage = DocumentSourceUnionWith::createFromBson(spec.firstElement(), expCtx);
    pipeline =
        Pipeline::create({DocumentSourceMock::createForTest(expCtx), unionWithStage}, expCtx);
    involvedNssSet = pipeline->getInvolvedCollections();
    ASSERT_EQ(involvedNssSet.size(), 1UL);
    ASSERT_EQ(1ul, involvedNssSet.count(unionWithNs));
}

}  // namespace
}  // namespace mongo
