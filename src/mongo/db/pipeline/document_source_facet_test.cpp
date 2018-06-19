/**
 *    Copyright (C) 2016 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_value_test_util.h"
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

TEST_F(DocumentSourceFacetTest, ShouldRejectEmptyPipelines) {
    auto ctx = getExpCtx();
    auto spec = BSON("$facet" << BSON("a" << BSONArray()));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);

    spec = BSON("$facet" << BSON("a" << BSON_ARRAY(BSON("$skip" << 4)) << "b" << BSONArray()));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx),
                  AssertionException);
}

TEST_F(DocumentSourceFacetTest, ShouldSucceedWhenNamespaceIsCollectionless) {
    auto ctx = getExpCtx();
    auto spec = fromjson("{$facet: {a: [{$match: {}}]}}");

    ctx->ns = NamespaceString::makeCollectionlessAggregateNSS("unittests");

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
    DocumentSourcePassthrough() : DocumentSourceMock({}) {}

    StageConstraints constraints(Pipeline::SplitState pipeState) const override {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed};
    }

    DocumentSource::GetNextResult getNext() final {
        return pSource->getNext();
    }

    static boost::intrusive_ptr<DocumentSourcePassthrough> create() {
        return new DocumentSourcePassthrough();
    }
};

TEST_F(DocumentSourceFacetTest, SingleFacetShouldReceiveAllDocuments) {
    auto ctx = getExpCtx();

    deque<DocumentSource::GetNextResult> inputs = {
        Document{{"_id", 0}}, Document{{"_id", 1}}, Document{{"_id", 2}}};
    auto mock = DocumentSourceMock::create(inputs);

    auto dummy = DocumentSourcePassthrough::create();

    auto statusWithPipeline = Pipeline::createFacetPipeline({dummy}, ctx);
    ASSERT_OK(statusWithPipeline.getStatus());
    auto pipeline = std::move(statusWithPipeline.getValue());

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
    auto mock = DocumentSourceMock::create(inputs);

    auto firstDummy = DocumentSourcePassthrough::create();
    auto firstPipeline = uassertStatusOK(Pipeline::createFacetPipeline({firstDummy}, ctx));

    auto secondDummy = DocumentSourcePassthrough::create();
    auto secondPipeline = uassertStatusOK(Pipeline::createFacetPipeline({secondDummy}, ctx));

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
    ASSERT_EQ(output.getDocument().size(), 2UL);
    ASSERT_VALUE_EQ(output.getDocument()["first"], Value(expectedOutputs));
    ASSERT_VALUE_EQ(output.getDocument()["second"], Value(expectedOutputs));

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
    auto mock = DocumentSourceMock::create(inputs);

    auto passthrough = DocumentSourcePassthrough::create();
    auto passthroughPipe = uassertStatusOK(Pipeline::createFacetPipeline({passthrough}, ctx));

    auto limit = DocumentSourceLimit::create(ctx, 1);
    auto limitedPipe = uassertStatusOK(Pipeline::createFacetPipeline({limit}, ctx));

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
    ASSERT_EQ(output.getDocument().size(), 2UL);
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
    auto mock = DocumentSourceMock::create(inputs);

    auto firstDummy = DocumentSourcePassthrough::create();
    auto secondDummy = DocumentSourcePassthrough::create();
    auto pipeline = uassertStatusOK(Pipeline::createFacetPipeline({firstDummy, secondDummy}, ctx));

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

    auto mockSource = DocumentSourceMock::create();

    auto firstDummy = DocumentSourcePassthrough::create();
    auto firstPipe = uassertStatusOK(Pipeline::createFacetPipeline({firstDummy}, ctx));
    auto secondDummy = DocumentSourcePassthrough::create();
    auto secondPipe = uassertStatusOK(Pipeline::createFacetPipeline({secondDummy}, ctx));

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("firstPipe", std::move(firstPipe));
    facets.emplace_back("secondPipe", std::move(secondPipe));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    facetStage->setSource(mockSource.get());

    facetStage->dispose();
    ASSERT_TRUE(mockSource->isDisposed);
}

// TODO: DocumentSourceFacet will have to propagate pauses if we ever allow nested $facets.
DEATH_TEST_F(DocumentSourceFacetTest,
             ShouldFailIfGivenPausedInput,
             "Invariant failure !input.isPaused()") {
    auto ctx = getExpCtx();
    auto mock = DocumentSourceMock::create(DocumentSource::GetNextResult::makePauseExecution());

    auto firstDummy = DocumentSourcePassthrough::create();
    auto pipeline = uassertStatusOK(Pipeline::createFacetPipeline({firstDummy}, ctx));

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
    auto firstPipeline = uassertStatusOK(Pipeline::createFacetPipeline({firstSkip}, ctx));

    auto secondSkip = DocumentSourceSkip::create(ctx, 2);
    auto secondPipeline = uassertStatusOK(Pipeline::createFacetPipeline({secondSkip}, ctx));

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
    ASSERT_EQ(serialization[0].getDocument().size(), 1UL);
    ASSERT_EQ(serialization[0].getDocument()["$facet"].getType(), BSONType::Object);

    // Should have two fields: "skippedOne" and "skippedTwo".
    auto serializedStage = serialization[0].getDocument()["$facet"].getDocument();
    ASSERT_EQ(serializedStage.size(), 2UL);
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

    auto dummy = DocumentSourcePassthrough::create();
    auto pipeline = unittest::assertGet(Pipeline::createFacetPipeline({dummy}, ctx));

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("subPipe", std::move(pipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    ASSERT_FALSE(dummy->isOptimized);
    facetStage->optimize();
    ASSERT_TRUE(dummy->isOptimized);
}

/**
 * An implementation of the MongoProcessInterface that is okay with changing the OperationContext,
 * but has no other parts of the interface implemented.
 */
class StubMongoProcessOkWithOpCtxChanges : public StubMongoProcessInterface {
public:
    void setOperationContext(OperationContext* opCtx) final {
        return;
    }
};

TEST_F(DocumentSourceFacetTest, ShouldPropagateDetachingAndReattachingOfOpCtx) {
    auto ctx = getExpCtx();
    // We're going to be changing the OperationContext, so we need to use a MongoProcessInterface
    // that won't throw when we do so.
    ctx->mongoProcessInterface = stdx::make_unique<StubMongoProcessOkWithOpCtxChanges>();

    auto firstDummy = DocumentSourcePassthrough::create();
    auto firstPipeline = unittest::assertGet(Pipeline::createFacetPipeline({firstDummy}, ctx));

    auto secondDummy = DocumentSourcePassthrough::create();
    auto secondPipeline = unittest::assertGet(Pipeline::createFacetPipeline({secondDummy}, ctx));

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
    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        deps->fields.insert("a");
        return GetDepsReturn::EXHAUSTIVE_ALL;
    }

    static boost::intrusive_ptr<DocumentSource> create() {
        return new DocumentSourceNeedsA();
    }
};

/**
 * A dummy DocumentSource which has one dependency: the field "b".
 */
class DocumentSourceNeedsB : public DocumentSourcePassthrough {
public:
    GetDepsReturn getDependencies(DepsTracker* deps) const final {
        deps->fields.insert("b");
        return GetDepsReturn::EXHAUSTIVE_ALL;
    }

    static boost::intrusive_ptr<DocumentSource> create() {
        return new DocumentSourceNeedsB();
    }
};

TEST_F(DocumentSourceFacetTest, ShouldUnionDependenciesOfInnerPipelines) {
    auto ctx = getExpCtx();

    auto needsA = DocumentSourceNeedsA::create();
    auto firstPipeline = unittest::assertGet(Pipeline::createFacetPipeline({needsA}, ctx));

    auto firstPipelineDeps =
        firstPipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_FALSE(firstPipelineDeps.needWholeDocument);
    ASSERT_EQ(firstPipelineDeps.fields.size(), 1UL);
    ASSERT_EQ(firstPipelineDeps.fields.count("a"), 1UL);

    auto needsB = DocumentSourceNeedsB::create();
    auto secondPipeline = unittest::assertGet(Pipeline::createFacetPipeline({needsB}, ctx));

    auto secondPipelineDeps =
        secondPipeline->getDependencies(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_FALSE(secondPipelineDeps.needWholeDocument);
    ASSERT_EQ(secondPipelineDeps.fields.size(), 1UL);
    ASSERT_EQ(secondPipelineDeps.fields.count("b"), 1UL);

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("needsA", std::move(firstPipeline));
    facets.emplace_back("needsB", std::move(secondPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    DepsTracker deps(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_EQ(facetStage->getDependencies(&deps), DocumentSource::GetDepsReturn::EXHAUSTIVE_ALL);
    ASSERT_FALSE(deps.needWholeDocument);
    ASSERT_FALSE(deps.getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE));
    ASSERT_EQ(deps.fields.size(), 2UL);
    ASSERT_EQ(deps.fields.count("a"), 1UL);
    ASSERT_EQ(deps.fields.count("b"), 1UL);
}

/**
 * A dummy DocumentSource which has a dependency on the entire document.
 */
class DocumentSourceNeedsWholeDocument : public DocumentSourcePassthrough {
public:
    GetDepsReturn getDependencies(DepsTracker* deps) const override {
        deps->needWholeDocument = true;
        return GetDepsReturn::EXHAUSTIVE_ALL;
    }
    static boost::intrusive_ptr<DocumentSourceNeedsWholeDocument> create() {
        return new DocumentSourceNeedsWholeDocument();
    }
};

TEST_F(DocumentSourceFacetTest, ShouldRequireWholeDocumentIfAnyPipelineRequiresWholeDocument) {
    auto ctx = getExpCtx();

    auto needsA = DocumentSourceNeedsA::create();
    auto firstPipeline = unittest::assertGet(Pipeline::createFacetPipeline({needsA}, ctx));

    auto needsWholeDocument = DocumentSourceNeedsWholeDocument::create();
    auto secondPipeline =
        unittest::assertGet(Pipeline::createFacetPipeline({needsWholeDocument}, ctx));

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("needsA", std::move(firstPipeline));
    facets.emplace_back("needsWholeDocument", std::move(secondPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    DepsTracker deps(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_EQ(facetStage->getDependencies(&deps), DocumentSource::GetDepsReturn::EXHAUSTIVE_ALL);
    ASSERT_TRUE(deps.needWholeDocument);
    ASSERT_FALSE(deps.getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE));
}

/**
 * A dummy DocumentSource which depends on only the text score.
 */
class DocumentSourceNeedsOnlyTextScore : public DocumentSourcePassthrough {
public:
    GetDepsReturn getDependencies(DepsTracker* deps) const override {
        deps->setNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE, true);
        return GetDepsReturn::EXHAUSTIVE_ALL;
    }
    static boost::intrusive_ptr<DocumentSourceNeedsOnlyTextScore> create() {
        return new DocumentSourceNeedsOnlyTextScore();
    }
};

TEST_F(DocumentSourceFacetTest, ShouldRequireTextScoreIfAnyPipelineRequiresTextScore) {
    auto ctx = getExpCtx();

    auto needsA = DocumentSourceNeedsA::create();
    auto firstPipeline = unittest::assertGet(Pipeline::createFacetPipeline({needsA}, ctx));

    auto needsWholeDocument = DocumentSourceNeedsWholeDocument::create();
    auto secondPipeline =
        unittest::assertGet(Pipeline::createFacetPipeline({needsWholeDocument}, ctx));

    auto needsTextScore = DocumentSourceNeedsOnlyTextScore::create();
    auto thirdPipeline = unittest::assertGet(Pipeline::createFacetPipeline({needsTextScore}, ctx));

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("needsA", std::move(firstPipeline));
    facets.emplace_back("needsWholeDocument", std::move(secondPipeline));
    facets.emplace_back("needsTextScore", std::move(thirdPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    DepsTracker deps(DepsTracker::MetadataAvailable::kTextScore);
    ASSERT_EQ(facetStage->getDependencies(&deps), DocumentSource::GetDepsReturn::EXHAUSTIVE_ALL);
    ASSERT_TRUE(deps.needWholeDocument);
    ASSERT_TRUE(deps.getNeedsMetadata(DepsTracker::MetadataType::TEXT_SCORE));
}

TEST_F(DocumentSourceFacetTest, ShouldThrowIfAnyPipelineRequiresTextScoreButItIsNotAvailable) {
    auto ctx = getExpCtx();

    auto needsA = DocumentSourceNeedsA::create();
    auto firstPipeline = unittest::assertGet(Pipeline::createFacetPipeline({needsA}, ctx));

    auto needsTextScore = DocumentSourceNeedsOnlyTextScore::create();
    auto secondPipeline = unittest::assertGet(Pipeline::createFacetPipeline({needsTextScore}, ctx));

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("needsA", std::move(firstPipeline));
    facets.emplace_back("needsTextScore", std::move(secondPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    DepsTracker deps(DepsTracker::MetadataAvailable::kNoMetadata);
    ASSERT_THROWS(facetStage->getDependencies(&deps), AssertionException);
}

/**
 * A dummy DocumentSource which needs to run on the primary shard.
 */
class DocumentSourceNeedsPrimaryShard final : public DocumentSourcePassthrough {
public:
    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kPrimaryShard,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed};
    }

    static boost::intrusive_ptr<DocumentSourceNeedsPrimaryShard> create() {
        return new DocumentSourceNeedsPrimaryShard();
    }
};

TEST_F(DocumentSourceFacetTest, ShouldRequirePrimaryShardIfAnyStageRequiresPrimaryShard) {
    auto ctx = getExpCtx();

    auto passthrough = DocumentSourcePassthrough::create();
    auto firstPipeline = unittest::assertGet(Pipeline::createFacetPipeline({passthrough}, ctx));

    auto needsPrimaryShard = DocumentSourceNeedsPrimaryShard::create();
    auto secondPipeline =
        unittest::assertGet(Pipeline::createFacetPipeline({needsPrimaryShard}, ctx));

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("passthrough", std::move(firstPipeline));
    facets.emplace_back("needsPrimaryShard", std::move(secondPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    ASSERT(facetStage->constraints(Pipeline::SplitState::kUnsplit).hostRequirement ==
           DocumentSource::StageConstraints::HostTypeRequirement::kPrimaryShard);
}

TEST_F(DocumentSourceFacetTest, ShouldNotRequirePrimaryShardIfNoStagesRequiresPrimaryShard) {
    auto ctx = getExpCtx();

    auto firstPassthrough = DocumentSourcePassthrough::create();
    auto firstPipeline =
        unittest::assertGet(Pipeline::createFacetPipeline({firstPassthrough}, ctx));

    auto secondPassthrough = DocumentSourcePassthrough::create();
    auto secondPipeline =
        unittest::assertGet(Pipeline::createFacetPipeline({secondPassthrough}, ctx));

    std::vector<DocumentSourceFacet::FacetPipeline> facets;
    facets.emplace_back("first", std::move(firstPipeline));
    facets.emplace_back("second", std::move(secondPipeline));
    auto facetStage = DocumentSourceFacet::create(std::move(facets), ctx);

    ASSERT(facetStage->constraints(Pipeline::SplitState::kUnsplit).hostRequirement ==
           DocumentSource::StageConstraints::HostTypeRequirement::kNone);
}

}  // namespace
}  // namespace mongo
