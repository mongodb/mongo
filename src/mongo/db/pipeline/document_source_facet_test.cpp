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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/util/assert_util.h"

namespace mongo {

// Crutch.
bool isMongos() {
    return false;
}

namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceFacetTest = AggregationContextFixture;

//
// Parsing and serialization.
//

TEST_F(DocumentSourceFacetTest, ShouldRejectNonObjectSpec) {
    auto ctx = getExpCtx();
    auto spec = BSON("$facet"
                     << "string");
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);

    spec = BSON("$facet" << 1);
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);

    spec = BSON("$facet" << BSON_ARRAY(1 << 2));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);
}

TEST_F(DocumentSourceFacetTest, ShouldRejectEmptyObject) {
    auto ctx = getExpCtx();
    auto spec = BSON("$facet" << BSONObj());
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);
}

TEST_F(DocumentSourceFacetTest, ShouldRejectFacetsWithInvalidNames) {
    auto ctx = getExpCtx();
    auto spec = BSON("$facet" << BSON("" << BSON_ARRAY(BSON("$skip" << 4))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);

    spec = BSON("$facet" << BSON("a.b" << BSON_ARRAY(BSON("$skip" << 4))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);

    spec = BSON("$facet" << BSON("$a" << BSON_ARRAY(BSON("$skip" << 4))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);
}

TEST_F(DocumentSourceFacetTest, ShouldRejectNonArrayFacets) {
    auto ctx = getExpCtx();
    auto spec = BSON("$facet" << BSON("a" << 1));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);

    spec = BSON("$facet" << BSON("a" << BSON_ARRAY(BSON("$skip" << 4)) << "b" << 2));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);
}

TEST_F(DocumentSourceFacetTest, ShouldRejectEmptyPipelines) {
    auto ctx = getExpCtx();
    auto spec = BSON("$facet" << BSON("a" << BSONArray()));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);

    spec = BSON("$facet" << BSON("a" << BSON_ARRAY(BSON("$skip" << 4)) << "b" << BSONArray()));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);
}

TEST_F(DocumentSourceFacetTest, ShouldRejectFacetsWithStagesThatMustBeTheFirstStage) {
    auto ctx = getExpCtx();
    auto spec = BSON("$facet" << BSON("a" << BSON_ARRAY(BSON("$indexStats" << BSONObj()))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);

    spec = BSON("$facet" << BSON(
                    "a" << BSON_ARRAY(BSON("$limit" << 1) << BSON("$indexStats" << BSONObj()))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);
}

TEST_F(DocumentSourceFacetTest, ShouldRejectFacetsContainingAnOutStage) {
    auto ctx = getExpCtx();
    auto spec = BSON("$facet" << BSON("a" << BSON_ARRAY(BSON("$out"
                                                             << "out_collection"))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);

    spec =
        BSON("$facet" << BSON("a" << BSON_ARRAY(BSON("$skip" << 1) << BSON("$out"
                                                                           << "out_collection"))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);

    spec = BSON("$facet" << BSON("a" << BSON_ARRAY(BSON("$out"
                                                        << "out_collection")
                                                   << BSON("$skip" << 1))));
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);
}

TEST_F(DocumentSourceFacetTest, ShouldRejectFacetsContainingAFacetStage) {
    auto ctx = getExpCtx();
    auto spec = fromjson("{$facet: {a: [{$facet: {a: [{$skip: 2}]}}]}}");
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);

    spec = fromjson("{$facet: {a: [{$skip: 2}, {$facet: {a: [{$skip: 2}]}}]}}");
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);

    spec = fromjson("{$facet: {a: [{$skip: 2}], b: [{$facet: {a: [{$skip: 2}]}}]}}");
    ASSERT_THROWS(DocumentSourceFacet::createFromBson(spec.firstElement(), ctx), UserException);
}

TEST_F(DocumentSourceFacetTest, ShouldAcceptLegalSpecification) {
    auto ctx = getExpCtx();
    auto spec = BSON("$facet" << BSON("a" << BSON_ARRAY(BSON("$skip" << 4)) << "b"
                                          << BSON_ARRAY(BSON("$limit" << 3))));
    auto facetStage = DocumentSourceFacet::createFromBson(spec.firstElement(), ctx);
    ASSERT_TRUE(facetStage.get());
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

    // We need this to be false so that it can be used in a $facet stage.
    bool isValidInitialSource() const final {
        return false;
    }

    boost::optional<Document> getNext() final {
        return pSource->getNext();
    }

    static boost::intrusive_ptr<DocumentSourcePassthrough> create(
        boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourcePassthrough(expCtx);
    }
};

TEST_F(DocumentSourceFacetTest, SingleFacetShouldReceiveAllDocuments) {
    auto ctx = getExpCtx();

    auto dummy = DocumentSourcePassthrough::create(ctx);

    auto statusWithPipeline = Pipeline::create({dummy}, ctx);
    ASSERT_OK(statusWithPipeline.getStatus());
    auto pipeline = std::move(statusWithPipeline.getValue());

    auto facetStage = DocumentSourceFacet::create({{"results", pipeline}}, ctx);

    std::deque<Document> inputs = {Document{{"_id", 0}}, Document{{"_id", 1}}};
    auto mock = DocumentSourceMock::create(inputs);
    facetStage->setSource(mock.get());

    auto output = facetStage->getNext();
    ASSERT_TRUE(output);
    ASSERT_EQ(*output, Document(fromjson("{results: [{_id: 0}, {_id: 1}]}")));

    // Should be exhausted now.
    ASSERT_FALSE(facetStage->getNext());
    ASSERT_FALSE(facetStage->getNext());
    ASSERT_FALSE(facetStage->getNext());
}

TEST_F(DocumentSourceFacetTest, MultipleFacetsShouldSeeTheSameDocuments) {
    auto ctx = getExpCtx();

    auto firstDummy = DocumentSourcePassthrough::create(ctx);
    auto firstPipeline = uassertStatusOK(Pipeline::create({firstDummy}, ctx));

    auto secondDummy = DocumentSourcePassthrough::create(ctx);
    auto secondPipeline = uassertStatusOK(Pipeline::create({secondDummy}, ctx));

    auto facetStage =
        DocumentSourceFacet::create({{"first", firstPipeline}, {"second", secondPipeline}}, ctx);

    std::deque<Document> inputs = {Document{{"_id", 0}}, Document{{"_id", 1}}};
    auto mock = DocumentSourceMock::create(inputs);
    facetStage->setSource(mock.get());

    auto output = facetStage->getNext();

    // The output fields are in no guaranteed order.
    std::vector<Value> expectedOutputs(inputs.begin(), inputs.end());
    ASSERT_TRUE(output);
    ASSERT_EQ((*output).size(), 2UL);
    ASSERT_EQ((*output)["first"], Value(expectedOutputs));
    ASSERT_EQ((*output)["second"], Value(expectedOutputs));

    // Should be exhausted now.
    ASSERT_FALSE(facetStage->getNext());
    ASSERT_FALSE(facetStage->getNext());
    ASSERT_FALSE(facetStage->getNext());
}

TEST_F(DocumentSourceFacetTest, ShouldBeAbleToEvaluateMultipleStagesWithinOneSubPipeline) {
    auto ctx = getExpCtx();

    auto firstDummy = DocumentSourcePassthrough::create(ctx);
    auto secondDummy = DocumentSourcePassthrough::create(ctx);
    auto pipeline = uassertStatusOK(Pipeline::create({firstDummy, secondDummy}, ctx));

    auto facetStage = DocumentSourceFacet::create({{"subPipe", pipeline}}, ctx);

    std::deque<Document> inputs = {Document{{"_id", 0}}, Document{{"_id", 1}}};
    auto mock = DocumentSourceMock::create(inputs);
    facetStage->setSource(mock.get());

    auto output = facetStage->getNext();
    ASSERT_TRUE(output);
    ASSERT_EQ(*output, Document(fromjson("{subPipe: [{_id: 0}, {_id: 1}]}")));
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
    auto firstSkip = DocumentSourceSkip::create(ctx);
    firstSkip->setSkip(1);
    auto firstPipeline = uassertStatusOK(Pipeline::create({firstSkip}, ctx));

    auto secondSkip = DocumentSourceSkip::create(ctx);
    secondSkip->setSkip(2);
    auto secondPipeline = uassertStatusOK(Pipeline::create({secondSkip}, ctx));

    auto facetStage = DocumentSourceFacet::create(
        {{"skippedOne", firstPipeline}, {"skippedTwo", secondPipeline}}, ctx);

    // Serialize the facet stage.
    std::vector<Value> serialization;
    facetStage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::Object);

    // The fields are in no guaranteed order, so we can't make a simple Document comparison.
    ASSERT_EQ(serialization[0].getDocument().size(), 1UL);
    ASSERT_EQ(serialization[0].getDocument()["$facet"].getType(), BSONType::Object);

    // Should have two fields: "skippedOne" and "skippedTwo".
    auto serializedStage = serialization[0].getDocument()["$facet"].getDocument();
    ASSERT_EQ(serializedStage.size(), 2UL);
    ASSERT_EQ(serializedStage["skippedOne"],
              Value(std::vector<Value>{Value(Document{{"$skip", 1}})}));
    ASSERT_EQ(serializedStage["skippedTwo"],
              Value(std::vector<Value>{Value(Document{{"$skip", 2}})}));

    auto serializedBson = serialization[0].getDocument().toBson();
    auto roundTripped = DocumentSourceFacet::createFromBson(serializedBson.firstElement(), ctx);

    // Serialize one more time to make sure we get the same thing.
    std::vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization);

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_EQ(newSerialization[0], serialization[0]);
}

TEST_F(DocumentSourceFacetTest, ShouldOptimizeInnerPipelines) {
    auto ctx = getExpCtx();

    auto dummy = DocumentSourcePassthrough::create(ctx);
    auto pipeline = uassertStatusOK(Pipeline::create({dummy}, ctx));

    auto facetStage = DocumentSourceFacet::create({{"subPipe", pipeline}}, ctx);

    ASSERT_FALSE(dummy->isOptimized);
    facetStage->optimize();
    ASSERT_TRUE(dummy->isOptimized);
}

TEST_F(DocumentSourceFacetTest, ShouldPropogateDetachingAndReattachingOfOpCtx) {
    auto ctx = getExpCtx();

    auto firstDummy = DocumentSourcePassthrough::create(ctx);
    auto firstPipeline = uassertStatusOK(Pipeline::create({firstDummy}, ctx));

    auto secondDummy = DocumentSourcePassthrough::create(ctx);
    auto secondPipeline = uassertStatusOK(Pipeline::create({secondDummy}, ctx));

    auto facetStage =
        DocumentSourceFacet::create({{"one", firstPipeline}, {"two", secondPipeline}}, ctx);

    // Test detaching.
    ASSERT_FALSE(firstDummy->isDetachedFromOpCtx);
    ASSERT_FALSE(secondDummy->isDetachedFromOpCtx);
    facetStage->doDetachFromOperationContext();
    ASSERT_TRUE(firstDummy->isDetachedFromOpCtx);
    ASSERT_TRUE(secondDummy->isDetachedFromOpCtx);

    // Test reattaching.
    facetStage->doReattachToOperationContext(ctx->opCtx);
    ASSERT_FALSE(firstDummy->isDetachedFromOpCtx);
    ASSERT_FALSE(secondDummy->isDetachedFromOpCtx);
}
}  // namespace
}  // namespace mongo
