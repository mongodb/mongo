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

#include "mongo/db/pipeline/document_source_facet.h"

#include <deque>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
using std::deque;
using std::vector;

namespace {

using std::deque;
using std::vector;

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceFacetTest = AggregationContextFixture;

//
// Parsing and serialization.
//

TEST_F(DocumentSourceFacetTest, ShouldRejectNonObjectSpec) {
    auto ctx = getExpCtx();
    auto spec = BSON("$facet"
                     << "string");
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);

    spec = BSON("$facet" << 1);
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);

    spec = BSON("$facet" << BSON_ARRAY(1 << 2));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);
}

TEST_F(DocumentSourceFacetTest, ShouldRejectEmptyObject) {
    auto ctx = getExpCtx();
    auto spec = BSON("$facet" << BSONObj());
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);
}

TEST_F(DocumentSourceFacetTest, ShouldRejectFacetsWithInvalidNames) {
    auto ctx = getExpCtx();
    auto spec = BSON("$facet" << BSON("" << BSON_ARRAY(BSON("$skip" << 4))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);

    spec = BSON("$facet" << BSON("a.b" << BSON_ARRAY(BSON("$skip" << 4))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);

    spec = BSON("$facet" << BSON("$a" << BSON_ARRAY(BSON("$skip" << 4))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);
}

TEST_F(DocumentSourceFacetTest, ShouldRejectNonArrayFacets) {
    auto ctx = getExpCtx();
    auto spec = BSON("$facet" << BSON("a" << 1));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);

    spec = BSON("$facet" << BSON("a" << BSON_ARRAY(BSON("$skip" << 4)) << "b" << 2));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);
}

TEST_F(DocumentSourceFacetTest, ShouldSucceedWhenNamespaceIsCollectionless) {
    auto ctx = getExpCtx();
    auto spec = fromjson("{$facet: {a: [{$match: {}}]}}");

    ctx->ns =
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName(boost::none, "unittests"));

    ASSERT_TRUE(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx).get());
}

TEST_F(DocumentSourceFacetTest, ShouldRejectFacetsContainingAnOutStage) {
    auto ctx = getExpCtx();
    auto spec = BSON("$facet" << BSON("a" << BSON_ARRAY(BSON("$out"
                                                             << "out_collection"))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);

    spec =
        BSON("$facet" << BSON("a" << BSON_ARRAY(BSON("$skip" << 1) << BSON("$out"
                                                                           << "out_collection"))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);

    spec = BSON("$facet" << BSON("a" << BSON_ARRAY(BSON("$out"
                                                        << "out_collection")
                                                   << BSON("$skip" << 1))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);
}

TEST_F(DocumentSourceFacetTest, ShouldRejectFacetsContainingAMergeStage) {
    auto ctx = getExpCtx();
    auto spec =
        BSON("$facet" << BSON("a" << BSON_ARRAY(BSON("$merge" << BSON("into"
                                                                      << "merge_collection")))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);

    spec =
        BSON("$facet" << BSON("a" << BSON_ARRAY(BSON("$skip" << 1)
                                                << BSON("$merge" << BSON("into"
                                                                         << "merge_collection")))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);

    spec = BSON("$facet" << BSON("a" << BSON_ARRAY(BSON("$merge" << BSON("into"
                                                                         << "merge_collection"))
                                                   << BSON("$skip" << 1))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);
}

TEST_F(DocumentSourceFacetTest, ShouldRejectFacetsContainingAFacetStage) {
    auto ctx = getExpCtx();
    auto spec = fromjson("{$facet: {a: [{$facet: {a: [{$skip: 2}]}}]}}");
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);

    spec = fromjson("{$facet: {a: [{$skip: 2}, {$facet: {a: [{$skip: 2}]}}]}}");
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);

    spec = fromjson("{$facet: {a: [{$skip: 2}], b: [{$facet: {a: [{$skip: 2}]}}]}}");
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);
}

TEST_F(DocumentSourceFacetTest, ShouldAcceptLegalSpecification) {
    auto ctx = getExpCtx();
    auto spec = BSON("$facet" << BSON("a" << BSON_ARRAY(BSON("$skip" << 4)) << "b"
                                          << BSON_ARRAY(BSON("$limit" << 3))));
    auto facetStage = DocumentSourceFacet::createFromBson(spec.firstElement(), ctx);
    ASSERT_TRUE(facetStage.get());
}

TEST_F(DocumentSourceFacetTest, ShouldRejectConflictingHostTypeRequirementsWithinSinglePipeline) {
    auto ctx = getExpCtx();
    ctx->inMongos = true;

    auto spec = fromjson(
        "{$facet: {badPipe: [{$_internalSplitPipeline: {mergeType: 'anyShard'}}, "
        "{$_internalSplitPipeline: {mergeType: 'mongos'}}]}}");

    ASSERT_THROWS_CODE(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                       AssertionException,
                       ErrorCodes::IllegalOperation);
}

TEST_F(DocumentSourceFacetTest, ShouldRejectConflictingHostTypeRequirementsAcrossPipelines) {
    auto ctx = getExpCtx();
    ctx->inMongos = true;

    auto spec = fromjson(
        "{$facet: {shardPipe: [{$_internalSplitPipeline: {mergeType: 'anyShard'}}], mongosPipe: "
        "[{$_internalSplitPipeline: {mergeType: 'mongos'}}]}}");

    ASSERT_THROWS_CODE(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                       AssertionException,
                       ErrorCodes::IllegalOperation);
}

//
// Evaluation.
//

/**
 * A dummy DocumentSource which just passes all input along to the next stage.
 */
class DocumentSourcePassthrough : public DocumentSourceMock {
public:
    DocumentSourcePassthrough(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMock({}, expCtx) {}

    StageConstraints constraints(Pipeline::SplitState pipeState) const override {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kAllowed,
                UnionRequirement::kAllowed};
    }

    DocumentSource::GetNextResult doGetNext() final {
        return pSource->getNext();
    }

    static boost::intrusive_ptr<DocumentSourcePassthrough> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourcePassthrough(expCtx);
    }
};

TEST_F(DocumentSourceFacetTest, PassthroughFacetDoesntRequireDiskAndIsOKInaTxn) {
    auto ctx = getExpCtx();
    auto passthrough = DocumentSourcePassthrough::create(ctx);
    auto passthroughPipe = Pipeline::create({passthrough}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("passthrough", std::move(passthroughPipe));

    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);
    ASSERT(facetStage->constraints(Pipeline::SplitState::kUnsplit).diskRequirement ==
           DocumentSource::DiskUseRequirement::kNoDiskUse);
    ASSERT(facetStage->constraints(Pipeline::SplitState::kUnsplit).transactionRequirement ==
           DocumentSource::TransactionRequirement::kAllowed);
}

/**
 * A dummy DocumentSource which writes persistent data.
 */
class DocumentSourceWritesPersistentData final : public DocumentSourcePassthrough {
public:
    DocumentSourceWritesPersistentData(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourcePassthrough(expCtx) {}
    StageConstraints constraints(Pipeline::SplitState) const final {
        return {
            StreamType::kStreaming,
            PositionRequirement::kNone,
            HostTypeRequirement::kNone,
            DiskUseRequirement::kWritesPersistentData,
            FacetRequirement::kAllowed,
            TransactionRequirement::kNotAllowed,
            LookupRequirement::kNotAllowed,
            UnionRequirement::kAllowed,
        };
    }

    static boost::intrusive_ptr<DocumentSourceWritesPersistentData> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceWritesPersistentData(expCtx);
    }
};

TEST_F(DocumentSourceFacetTest, FacetWithChildThatWritesDataAlsoReportsWritingData) {
    auto ctx = getExpCtx();
    auto writesDataStage = DocumentSourceWritesPersistentData::create(ctx);
    auto pipeline = Pipeline::create({writesDataStage}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("writes", std::move(pipeline));

    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);
    ASSERT(facetStage->constraints(Pipeline::SplitState::kUnsplit).diskRequirement ==
           DocumentSource::DiskUseRequirement::kWritesPersistentData);
    ASSERT(facetStage->constraints(Pipeline::SplitState::kUnsplit).transactionRequirement ==
           DocumentSource::TransactionRequirement::kNotAllowed);
}

TEST_F(DocumentSourceFacetTest, SingleFacetShouldReceiveAllDocuments) {
    auto ctx = getExpCtx();

    deque<DocumentSource::GetNextResult> inputs = {
        Document{{"_id", 0}}, Document{{"_id", 1}}, Document{{"_id", 2}}};
    auto mock = DocumentSourceMock::createForTest(inputs, getExpCtx());

    auto dummy = DocumentSourcePassthrough::create(ctx);

    auto pipeline = Pipeline::create({dummy}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("results", std::move(pipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);
    facetStage->setSource(mock.get());

    auto output = facetStage->getNext();
    ASSERT(output.isAdvanced());
    ASSERT_DOCUMENT_EQ(output.getDocument(),
                       Document(fromjson("{results: [{_id: 0}, {_id: 1}, {_id: 2}]}")));

    // Should be exhausted now.
    ASSERT(facetStage->getNext().isEOF());
    ASSERT(facetStage->getNext().isEOF());
    ASSERT(facetStage->getNext().isEOF());
}

TEST_F(DocumentSourceFacetTest, MultipleFacetsShouldSeeTheSameDocuments) {
    auto ctx = getExpCtx();

    deque<DocumentSource::GetNextResult> inputs = {
        Document{{"_id", 0}}, Document{{"_id", 1}}, Document{{"_id", 2}}};
    auto mock = DocumentSourceMock::createForTest(inputs, getExpCtx());

    auto firstDummy = DocumentSourcePassthrough::create(ctx);
    auto firstPipeline = Pipeline::create({firstDummy}, ctx);

    auto secondDummy = DocumentSourcePassthrough::create(ctx);
    auto secondPipeline = Pipeline::create({secondDummy}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("first", std::move(firstPipeline));
    facets.emplace_back("second", std::move(secondPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    facetStage->setSource(mock.get());

    auto output = facetStage->getNext();

    // The output fields are in no guaranteed order.
    vector<Value> expectedOutputs;
    for (auto&& input : inputs) {
        expectedOutputs.emplace_back(input.releaseDocument());
    }
    ASSERT(output.isAdvanced());
    ASSERT_EQ(output.getDocument().computeSize(), 2ULL);
    ASSERT_VALUE_EQ(output.getDocument()["first"], Value(expectedOutputs));
    ASSERT_VALUE_EQ(output.getDocument()["second"], Value(expectedOutputs));

    // Should be exhausted now.
    ASSERT(facetStage->getNext().isEOF());
    ASSERT(facetStage->getNext().isEOF());
    ASSERT(facetStage->getNext().isEOF());
}

TEST_F(DocumentSourceFacetTest, ShouldAcceptEmptyPipelines) {
    auto ctx = getExpCtx();
    auto spec = BSON("$facet" << BSON("a" << BSONArray()));

    deque<DocumentSource::GetNextResult> inputs = {
        Document{{"_id", 0}}, Document{{"_id", 1}}, Document{{"_id", 2}}};
    auto mock = DocumentSourceMock::createForTest(inputs, ctx);

    auto facetStage = DocumentSourceFacet::createFromBson(spec.firstElement(), ctx);
    facetStage->setSource(mock.get());

    auto output = facetStage->getNext();

    // The output fields are in no guaranteed order.
    vector<Value> expectedOutputs;
    for (auto&& input : inputs) {
        expectedOutputs.emplace_back(input.releaseDocument());
    }
    ASSERT(output.isAdvanced());
    ASSERT_EQ(output.getDocument().computeSize(), 1ULL);
    ASSERT_VALUE_EQ(output.getDocument()["a"], Value(expectedOutputs));

    // Should be exhausted now.
    ASSERT(facetStage->getNext().isEOF());
    ASSERT(facetStage->getNext().isEOF());
    ASSERT(facetStage->getNext().isEOF());
}

TEST_F(DocumentSourceFacetTest,
       ShouldCorrectlyHandleSubPipelinesYieldingDifferentNumbersOfResults) {
    auto ctx = getExpCtx();

    deque<DocumentSource::GetNextResult> inputs = {
        Document{{"_id", 0}}, Document{{"_id", 1}}, Document{{"_id", 2}}, Document{{"_id", 3}}};
    auto mock = DocumentSourceMock::createForTest(inputs, getExpCtx());

    auto passthrough = DocumentSourcePassthrough::create(ctx);
    auto passthroughPipe = Pipeline::create({passthrough}, ctx);

    auto limit = DocumentSourceLimit::create(ctx, 1);
    auto limitedPipe = Pipeline::create({limit}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("all", std::move(passthroughPipe));
    facets.emplace_back("first", std::move(limitedPipe));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    facetStage->setSource(mock.get());

    vector<Value> expectedPassthroughOutput;
    for (auto&& input : inputs) {
        expectedPassthroughOutput.emplace_back(input.getDocument());
    }
    auto output = facetStage->getNext();

    // The output fields are in no guaranteed order.
    ASSERT(output.isAdvanced());
    ASSERT_EQ(output.getDocument().computeSize(), 2ULL);
    ASSERT_VALUE_EQ(output.getDocument()["all"], Value(expectedPassthroughOutput));
    ASSERT_VALUE_EQ(output.getDocument()["first"],
                    Value(vector<Value>{Value(expectedPassthroughOutput.front())}));

    // Should be exhausted now.
    ASSERT(facetStage->getNext().isEOF());
    ASSERT(facetStage->getNext().isEOF());
    ASSERT(facetStage->getNext().isEOF());
}

TEST_F(DocumentSourceFacetTest, ShouldBeAbleToEvaluateMultipleStagesWithinOneSubPipeline) {
    auto ctx = getExpCtx();

    deque<DocumentSource::GetNextResult> inputs = {Document{{"_id", 0}}, Document{{"_id", 1}}};
    auto mock = DocumentSourceMock::createForTest(inputs, getExpCtx());

    auto firstDummy = DocumentSourcePassthrough::create(ctx);
    auto secondDummy = DocumentSourcePassthrough::create(ctx);
    auto pipeline = Pipeline::create({firstDummy, secondDummy}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("subPipe", std::move(pipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    facetStage->setSource(mock.get());

    auto output = facetStage->getNext();
    ASSERT(output.isAdvanced());
    ASSERT_DOCUMENT_EQ(output.getDocument(), Document(fromjson("{subPipe: [{_id: 0}, {_id: 1}]}")));
}

TEST_F(DocumentSourceFacetTest, ShouldPropagateDisposeThroughToSource) {
    auto ctx = getExpCtx();

    auto mockSource = DocumentSourceMock::createForTest(getExpCtx());

    auto firstDummy = DocumentSourcePassthrough::create(ctx);
    auto firstPipe = Pipeline::create({firstDummy}, ctx);
    auto secondDummy = DocumentSourcePassthrough::create(ctx);
    auto secondPipe = Pipeline::create({secondDummy}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("firstPipe", std::move(firstPipe));
    facets.emplace_back("secondPipe", std::move(secondPipe));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    facetStage->setSource(mockSource.get());

    facetStage->dispose();
    ASSERT_TRUE(mockSource->isDisposed);
}

// TODO: DocumentSourceFacet will have to propagate pauses if we ever allow nested $facets.
DEATH_TEST_REGEX_F(DocumentSourceFacetTest,
                   ShouldFailIfGivenPausedInput,
                   R"#(Invariant failure.*!input.isPaused\(\))#") {
    auto ctx = getExpCtx();
    auto mock = DocumentSourceMock::createForTest(
        DocumentSource::GetNextResult::makePauseExecution(), getExpCtx());

    auto firstDummy = DocumentSourcePassthrough::create(ctx);
    auto pipeline = Pipeline::create({firstDummy}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("subPipe", std::move(pipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    facetStage->setSource(mock.get());

    facetStage->getNext();  // This should cause a crash.
}

//
// Miscellaneous.
//

TEST_F(DocumentSourceFacetTest, ShouldBeAbleToReParseSerializedStage) {
    auto ctx = getExpCtx();

    // Create a facet stage like the following:
    // {$facet: {
    //   skippedOne: [{$skip: 1}],
    //   skippedTwo: [{$skip: 2}]
    // }}
    auto firstSkip = DocumentSourceSkip::create(ctx, 1);
    auto firstPipeline = Pipeline::create({firstSkip}, ctx);

    auto secondSkip = DocumentSourceSkip::create(ctx, 2);
    auto secondPipeline = Pipeline::create({secondSkip}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("skippedOne", std::move(firstPipeline));
    facets.emplace_back("skippedTwo", std::move(secondPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    // Serialize the facet stage.
    vector<Value> serialization;
    facetStage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::Object);

    // The fields are in no guaranteed order, so we can't make a simple Document comparison.
    ASSERT_EQ(serialization[0].getDocument().computeSize(), 1ULL);
    ASSERT_EQ(serialization[0].getDocument()["$facet"].getType(), BSONType::Object);

    // Should have two fields: "skippedOne" and "skippedTwo".
    auto serializedStage = serialization[0].getDocument()["$facet"].getDocument();
    ASSERT_EQ(serializedStage.computeSize(), 2ULL);
    ASSERT_VALUE_EQ(serializedStage["skippedOne"],
                    Value(vector<Value>{Value(Document{{"$skip", 1}})}));
    ASSERT_VALUE_EQ(serializedStage["skippedTwo"],
                    Value(vector<Value>{Value(Document{{"$skip", 2}})}));

    auto serializedBson = serialization[0].getDocument().toBson();
    auto roundTripped = DocumentSourceFacet::createFromBson(serializedBson.firstElement(), ctx);

    // Serialize one more time to make sure we get the same thing.
    vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization);

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

TEST_F(DocumentSourceFacetTest, ShouldOptimizeInnerPipelines) {
    auto ctx = getExpCtx();

    auto dummy = DocumentSourcePassthrough::create(ctx);
    auto pipeline = Pipeline::create({dummy}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("subPipe", std::move(pipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    ASSERT_FALSE(dummy->isOptimized);
    facetStage->optimize();
    ASSERT_TRUE(dummy->isOptimized);
}

TEST_F(DocumentSourceFacetTest, ShouldPropagateDetachingAndReattachingOfOpCtx) {
    auto ctx = getExpCtx();
    // We're going to be changing the OperationContext, so we need to use a MongoProcessInterface
    // that won't throw when we do so.
    ctx->mongoProcessInterface = std::make_unique<StubMongoProcessInterface>();

    auto firstDummy = DocumentSourcePassthrough::create(ctx);
    auto firstPipeline = Pipeline::create({firstDummy}, ctx);

    auto secondDummy = DocumentSourcePassthrough::create(ctx);
    auto secondPipeline = Pipeline::create({secondDummy}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("one", std::move(firstPipeline));
    facets.emplace_back("two", std::move(secondPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    // Test detaching.
    ASSERT_FALSE(firstDummy->isDetachedFromOpCtx);
    ASSERT_FALSE(secondDummy->isDetachedFromOpCtx);
    facetStage->detachFromOperationContext();
    ASSERT_TRUE(firstDummy->isDetachedFromOpCtx);
    ASSERT_TRUE(secondDummy->isDetachedFromOpCtx);

    // Test reattaching.
    facetStage->reattachToOperationContext(ctx->opCtx);
    ASSERT_FALSE(firstDummy->isDetachedFromOpCtx);
    ASSERT_FALSE(secondDummy->isDetachedFromOpCtx);
}

/**
 * A dummy DocumentSource which has one dependency: the field "a".
 */
class DocumentSourceNeedsA : public DocumentSourcePassthrough {
public:
    DocumentSourceNeedsA(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourcePassthrough(expCtx) {}

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->fields.insert("a");
        return DepsTracker::State::EXHAUSTIVE_ALL;
    }

    static boost::intrusive_ptr<DocumentSource> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceNeedsA(expCtx);
    }
};

/**
 * A dummy DocumentSource which has one dependency: the field "b".
 */
class DocumentSourceNeedsB : public DocumentSourcePassthrough {
public:
    DocumentSourceNeedsB(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourcePassthrough(expCtx) {}

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->fields.insert("b");
        return DepsTracker::State::EXHAUSTIVE_ALL;
    }

    static boost::intrusive_ptr<DocumentSource> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceNeedsB(expCtx);
    }
};

TEST_F(DocumentSourceFacetTest, ShouldUnionDependenciesOfInnerPipelines) {
    auto ctx = getExpCtx();

    auto needsA = DocumentSourceNeedsA::create(ctx);
    auto firstPipeline = Pipeline::create({needsA}, ctx);

    auto firstPipelineDeps = firstPipeline->getDependencies(DepsTracker::kNoMetadata);
    ASSERT_FALSE(firstPipelineDeps.needWholeDocument);
    ASSERT_EQ(firstPipelineDeps.fields.size(), 1UL);
    ASSERT_EQ(firstPipelineDeps.fields.count("a"), 1UL);

    auto needsB = DocumentSourceNeedsB::create(ctx);
    auto secondPipeline = Pipeline::create({needsB}, ctx);

    auto secondPipelineDeps = secondPipeline->getDependencies(DepsTracker::kNoMetadata);
    ASSERT_FALSE(secondPipelineDeps.needWholeDocument);
    ASSERT_EQ(secondPipelineDeps.fields.size(), 1UL);
    ASSERT_EQ(secondPipelineDeps.fields.count("b"), 1UL);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("needsA", std::move(firstPipeline));
    facets.emplace_back("needsB", std::move(secondPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    DepsTracker deps(DepsTracker::kNoMetadata);
    ASSERT_EQ(facetStage->getDependencies(&deps), DepsTracker::State::EXHAUSTIVE_ALL);
    ASSERT_FALSE(deps.needWholeDocument);
    ASSERT_FALSE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
    ASSERT_EQ(deps.fields.size(), 2UL);
    ASSERT_EQ(deps.fields.count("a"), 1UL);
    ASSERT_EQ(deps.fields.count("b"), 1UL);
}

/**
 * A dummy DocumentSource which has a dependency on the entire document.
 */
class DocumentSourceNeedsWholeDocument : public DocumentSourcePassthrough {
public:
    DocumentSourceNeedsWholeDocument(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourcePassthrough(expCtx) {}
    DepsTracker::State getDependencies(DepsTracker* deps) const override {
        deps->needWholeDocument = true;
        return DepsTracker::State::EXHAUSTIVE_ALL;
    }
    static boost::intrusive_ptr<DocumentSourceNeedsWholeDocument> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceNeedsWholeDocument(expCtx);
    }
};

TEST_F(DocumentSourceFacetTest, ShouldRequireWholeDocumentIfAnyPipelineRequiresWholeDocument) {
    auto ctx = getExpCtx();

    auto needsA = DocumentSourceNeedsA::create(ctx);
    auto firstPipeline = Pipeline::create({needsA}, ctx);

    auto needsWholeDocument = DocumentSourceNeedsWholeDocument::create(ctx);
    auto secondPipeline = Pipeline::create({needsWholeDocument}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("needsA", std::move(firstPipeline));
    facets.emplace_back("needsWholeDocument", std::move(secondPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    DepsTracker deps(DepsTracker::kNoMetadata);
    ASSERT_EQ(facetStage->getDependencies(&deps), DepsTracker::State::EXHAUSTIVE_ALL);
    ASSERT_TRUE(deps.needWholeDocument);
    ASSERT_FALSE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

/**
 * A dummy DocumentSource which depends on only the text score.
 */
class DocumentSourceNeedsOnlyTextScore : public DocumentSourcePassthrough {
public:
    DocumentSourceNeedsOnlyTextScore(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourcePassthrough(expCtx) {}
    DepsTracker::State getDependencies(DepsTracker* deps) const override {
        deps->setNeedsMetadata(DocumentMetadataFields::kTextScore, true);
        return DepsTracker::State::EXHAUSTIVE_ALL;
    }
    static boost::intrusive_ptr<DocumentSourceNeedsOnlyTextScore> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceNeedsOnlyTextScore(expCtx);
    }
};

TEST_F(DocumentSourceFacetTest, ShouldRequireTextScoreIfAnyPipelineRequiresTextScore) {
    auto ctx = getExpCtx();

    auto needsA = DocumentSourceNeedsA::create(ctx);
    auto firstPipeline = Pipeline::create({needsA}, ctx);

    auto needsWholeDocument = DocumentSourceNeedsWholeDocument::create(ctx);
    auto secondPipeline = Pipeline::create({needsWholeDocument}, ctx);

    auto needsTextScore = DocumentSourceNeedsOnlyTextScore::create(ctx);
    auto thirdPipeline = Pipeline::create({needsTextScore}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("needsA", std::move(firstPipeline));
    facets.emplace_back("needsWholeDocument", std::move(secondPipeline));
    facets.emplace_back("needsTextScore", std::move(thirdPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    DepsTracker deps(DepsTracker::kAllMetadata & ~DepsTracker::kOnlyTextScore);
    ASSERT_EQ(facetStage->getDependencies(&deps), DepsTracker::State::EXHAUSTIVE_ALL);
    ASSERT_TRUE(deps.needWholeDocument);
    ASSERT_TRUE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(DocumentSourceFacetTest, ShouldThrowIfAnyPipelineRequiresTextScoreButItIsNotAvailable) {
    auto ctx = getExpCtx();

    auto needsA = DocumentSourceNeedsA::create(ctx);
    auto firstPipeline = Pipeline::create({needsA}, ctx);

    auto needsTextScore = DocumentSourceNeedsOnlyTextScore::create(ctx);
    auto secondPipeline = Pipeline::create({needsTextScore}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("needsA", std::move(firstPipeline));
    facets.emplace_back("needsTextScore", std::move(secondPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    DepsTracker deps(DepsTracker::kAllMetadata);
    ASSERT_THROWS(facetStage->getDependencies(&deps), AssertionException);
}

/**
 * A dummy DocumentSource which needs to run on the primary shard.
 */
class DocumentSourceNeedsPrimaryShard final : public DocumentSourcePassthrough {
public:
    DocumentSourceNeedsPrimaryShard(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourcePassthrough(expCtx) {}
    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kPrimaryShard,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kAllowed,
                UnionRequirement::kAllowed};
    }

    static boost::intrusive_ptr<DocumentSourceNeedsPrimaryShard> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceNeedsPrimaryShard(expCtx);
    }
};

TEST_F(DocumentSourceFacetTest, ShouldRequirePrimaryShardIfAnyStageRequiresPrimaryShard) {
    auto ctx = getExpCtx();

    auto passthrough = DocumentSourcePassthrough::create(ctx);
    auto firstPipeline = Pipeline::create({passthrough}, ctx);

    auto needsPrimaryShard = DocumentSourceNeedsPrimaryShard::create(ctx);
    auto secondPipeline = Pipeline::create({needsPrimaryShard}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("passthrough", std::move(firstPipeline));
    facets.emplace_back("needsPrimaryShard", std::move(secondPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    ASSERT(facetStage->constraints(Pipeline::SplitState::kUnsplit).hostRequirement ==
           StageConstraints::HostTypeRequirement::kPrimaryShard);
    ASSERT(facetStage->constraints(Pipeline::SplitState::kUnsplit).diskRequirement ==
           StageConstraints::DiskUseRequirement::kNoDiskUse);
    ASSERT(facetStage->constraints(Pipeline::SplitState::kUnsplit).transactionRequirement ==
           StageConstraints::TransactionRequirement::kAllowed);
}

TEST_F(DocumentSourceFacetTest, ShouldNotRequirePrimaryShardIfNoStagesRequiresPrimaryShard) {
    auto ctx = getExpCtx();

    auto firstPassthrough = DocumentSourcePassthrough::create(ctx);
    auto firstPipeline = Pipeline::create({firstPassthrough}, ctx);

    auto secondPassthrough = DocumentSourcePassthrough::create(ctx);
    auto secondPipeline = Pipeline::create({secondPassthrough}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("first", std::move(firstPipeline));
    facets.emplace_back("second", std::move(secondPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    ASSERT(facetStage->constraints(Pipeline::SplitState::kUnsplit).hostRequirement ==
           StageConstraints::HostTypeRequirement::kNone);
    ASSERT(facetStage->constraints(Pipeline::SplitState::kUnsplit).diskRequirement ==
           StageConstraints::DiskUseRequirement::kNoDiskUse);
    ASSERT(facetStage->constraints(Pipeline::SplitState::kUnsplit).transactionRequirement ==
           StageConstraints::TransactionRequirement::kAllowed);
}

/**
 * A dummy DocumentSource that must run on the primary shard, can write temporary data and can't be
 * used in a transaction.
 */
class DocumentSourcePrimaryShardTmpDataNoTxn final : public DocumentSourcePassthrough {
public:
    DocumentSourcePrimaryShardTmpDataNoTxn(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourcePassthrough(expCtx) {}
    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kPrimaryShard,
                DiskUseRequirement::kWritesTmpData,
                FacetRequirement::kAllowed,
                TransactionRequirement::kNotAllowed,
                LookupRequirement::kAllowed,
                UnionRequirement::kAllowed};
    }

    static boost::intrusive_ptr<DocumentSourcePrimaryShardTmpDataNoTxn> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourcePrimaryShardTmpDataNoTxn(expCtx);
    }
};

/**
 * A DocumentSource which cannot be used in a $lookup pipeline.
 */
class DocumentSourceBannedInLookup final : public DocumentSourcePassthrough {
public:
    DocumentSourceBannedInLookup(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourcePassthrough(expCtx) {}
    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kAnyShard,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kNotAllowed,
                UnionRequirement::kAllowed};
    }

    static boost::intrusive_ptr<DocumentSourceBannedInLookup> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceBannedInLookup(expCtx);
    }
};

TEST_F(DocumentSourceFacetTest, ShouldSurfaceStrictestRequirementsOfEachConstraint) {
    auto ctx = getExpCtx();

    auto firstPassthrough = DocumentSourcePassthrough::create(ctx);
    auto firstPipeline = Pipeline::create({firstPassthrough}, ctx);

    auto secondPassthrough = DocumentSourcePrimaryShardTmpDataNoTxn::create(ctx);
    auto secondPipeline = Pipeline::create({secondPassthrough}, ctx);

    auto thirdPassthrough = DocumentSourceBannedInLookup::create(ctx);
    auto thirdPipeline = Pipeline::create({thirdPassthrough}, ctx);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("first", std::move(firstPipeline));
    facets.emplace_back("second", std::move(secondPipeline));
    facets.emplace_back("third", std::move(thirdPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    ASSERT(facetStage->constraints(Pipeline::SplitState::kUnsplit).hostRequirement ==
           StageConstraints::HostTypeRequirement::kPrimaryShard);
    ASSERT(facetStage->constraints(Pipeline::SplitState::kUnsplit).diskRequirement ==
           StageConstraints::DiskUseRequirement::kWritesTmpData);
    ASSERT(facetStage->constraints(Pipeline::SplitState::kUnsplit).transactionRequirement ==
           StageConstraints::TransactionRequirement::kNotAllowed);
    ASSERT_FALSE(
        facetStage->constraints(Pipeline::SplitState::kUnsplit).isAllowedInLookupPipeline());
}

TEST_F(DocumentSourceFacetTest, RedactsCorrectly) {
    auto spec = fromjson(R"({
        $facet: {
            a: [
                { $unwind: "$foo" },
                { $sortByCount: "$foo" }
            ],
            b: [
                {
                    $match: {
                        bar: { $exists: 1 }
                    }
                },
                {
                    $bucket: {
                        groupBy: "$bar.foo",
                        boundaries: [0, 50, 100, 200],
                        output: {
                            z: { $sum : 1 }
                        }
                    }
                }
            ],
            c: [
                {
                    $bucketAuto: {
                        groupBy: "$bar.baz",
                        buckets: 4
                    }
                }
            ]
        }
    })");
    auto docSource = DocumentSourceFacet::createFromBson(spec.firstElement(), getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$facet": {
                "HASH<a>": [
                    {
                        "$unwind": {
                            "path": "$HASH<foo>"
                        }
                    },
                    {
                        "$group": {
                            "_id": "$HASH<foo>",
                            "HASH<count>": {
                                "$sum": "?number"
                            }
                        }
                    },
                    {
                        "$sort": {
                            "HASH<count>": -1
                        }
                    }
                ],
                "HASH<b>": [
                    {
                        "$match": {
                            "HASH<bar>": {
                                "$exists": "?bool"
                            }
                        }
                    },
                    {
                        "$group": {
                            "_id": {
                                "$switch": {
                                    "branches": [
                                        {
                                            "case": {
                                                "$and": [
                                                    {
                                                        "$gte": [
                                                            "$HASH<bar>.HASH<foo>",
                                                            "?number"
                                                        ]
                                                    },
                                                    {
                                                        "$lt": [
                                                            "$HASH<bar>.HASH<foo>",
                                                            "?number"
                                                        ]
                                                    }
                                                ]
                                            },
                                            "then": "?number"
                                        },
                                        {
                                            "case": {
                                                "$and": [
                                                    {
                                                        "$gte": [
                                                            "$HASH<bar>.HASH<foo>",
                                                            "?number"
                                                        ]
                                                    },
                                                    {
                                                        "$lt": [
                                                            "$HASH<bar>.HASH<foo>",
                                                            "?number"
                                                        ]
                                                    }
                                                ]
                                            },
                                            "then": "?number"
                                        },
                                        {
                                            "case": {
                                                "$and": [
                                                    {
                                                        "$gte": [
                                                            "$HASH<bar>.HASH<foo>",
                                                            "?number"
                                                        ]
                                                    },
                                                    {
                                                        "$lt": [
                                                            "$HASH<bar>.HASH<foo>",
                                                            "?number"
                                                        ]
                                                    }
                                                ]
                                            },
                                            "then": "?number"
                                        }
                                    ]
                                }
                            },
                            "HASH<z>": {
                                "$sum": "?number"
                            }
                        }
                    },
                    {
                        "$sort": {
                            "HASH<_id>": 1
                        }
                    }
                ],
                "HASH<c>": [
                    {
                        "$bucketAuto": {
                            "groupBy": "$HASH<bar>.HASH<baz>",
                            "buckets": "?",
                            "output": {
                                "HASH<count>": {
                                    "$sum": "?number"
                                }
                            }
                        }
                    }
                ]
            }
        })",
        redact(*docSource));
}
}  // namespace
}  // namespace mongo
