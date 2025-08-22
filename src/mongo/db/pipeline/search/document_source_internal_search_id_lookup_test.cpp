/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/process_interface/stub_lookup_single_document_process_interface.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"

#include <deque>
#include <vector>

#include <boost/intrusive_ptr.hpp>

namespace mongo {
namespace {

using boost::intrusive_ptr;
using std::deque;
using std::vector;

using MockMongoInterface = StubLookupSingleDocumentProcessInterface;
const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("unittests.pipeline_test");

class InternalSearchIdLookupTest : public ServiceContextTest {
public:
    InternalSearchIdLookupTest() : InternalSearchIdLookupTest(NamespaceString(kTestNss)) {}

    InternalSearchIdLookupTest(NamespaceString nss) {
        unittest::TempDir tempDir("AggregationContextFixture");
        _expCtx = ExpressionContextBuilder{}
                      .opCtx(_opCtx.get())
                      .ns(nss)
                      .mongoProcessInterface(std::make_unique<MockMongoInterface>(
                          std::deque<DocumentSource::GetNextResult>()))
                      .tmpDir(tempDir.path())
                      .build();
    }

    boost::intrusive_ptr<ExpressionContext> getExpCtx() {
        return _expCtx.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx = makeOperationContext();
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

TEST_F(InternalSearchIdLookupTest, ShouldSkipResultsWhenIdNotFound) {
    auto expCtx = getExpCtx();
    expCtx->setUUID(UUID::gen());
    auto specObj = BSON("$_internalSearchIdLookup" << BSONObj());
    auto spec = specObj.firstElement();

    // Set up the idLookup stage.
    auto idLookup = DocumentSourceInternalSearchIdLookUp::createFromBson(spec, expCtx);
    auto idLookupStage = exec::agg::buildStage(idLookup);

    // Mock its input.
    auto mockLocalStage =
        exec::agg::MockStage::createForTest({Document{{"_id", 0}}, Document{{"_id", 1}}}, expCtx);
    idLookupStage->setSource(mockLocalStage.get());

    // Mock documents for this namespace.
    deque<DocumentSource::GetNextResult> mockDbContents{Document{{"_id", 0}, {"color", "red"_sd}}};
    expCtx->setMongoProcessInterface(
        std::make_unique<StubLookupSingleDocumentProcessInterface>(mockDbContents));

    // We should find one document here with _id = 0.
    auto next = idLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"_id", 0}, {"color", "red"_sd}}));

    ASSERT_TRUE(idLookupStage->getNext().isEOF());
    ASSERT_TRUE(idLookupStage->getNext().isEOF());
}

TEST_F(InternalSearchIdLookupTest, ShouldNotRemoveMetadata) {
    auto expCtx = getExpCtx();
    expCtx->setUUID(UUID::gen());

    // Create a mock data source.
    MutableDocument docOne(Document({{"_id", 0}}));
    docOne.metadata().setSearchScore(0.123);
    auto searchScoreDetails = BSON("scoreDetails" << "foo");
    docOne.metadata().setSearchScoreDetails(searchScoreDetails);
    auto mockLocalStage = exec::agg::MockStage::createForTest({docOne.freeze()}, expCtx);

    // Set up the idLookup stage.
    auto specObj = BSON("$_internalSearchIdLookup" << BSONObj());
    auto spec = specObj.firstElement();

    auto idLookup = DocumentSourceInternalSearchIdLookUp::createFromBson(spec, expCtx);
    auto idLookupStage = exec::agg::buildStage(idLookup);
    idLookupStage->setSource(mockLocalStage.get());

    // Set up a project stage that asks for metadata.
    auto projectSpec = fromjson(
        "{$project: {score: {$meta: \"searchScore\"}, "
        "scoreInfo: {$meta: \"searchScoreDetails\"},"
        " _id: 1, color: 1}}");
    auto project = DocumentSourceProject::createFromBson(projectSpec.firstElement(), expCtx);
    auto projectStage = exec::agg::buildStage(project);
    projectStage->setSource(idLookupStage.get());

    // Mock documents for this namespace.
    deque<DocumentSource::GetNextResult> mockDbContents{
        Document{{"_id", 0}, {"color", "red"_sd}, {"something else", "will be projected out"_sd}}};
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>(mockDbContents));

    // We should find one document here with _id = 0.
    auto next = projectStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.releaseDocument(),
        (Document{
            {"_id", 0}, {"color", "red"_sd}, {"score", 0.123}, {"scoreInfo", searchScoreDetails}}));

    ASSERT_TRUE(idLookupStage->getNext().isEOF());
    ASSERT_TRUE(idLookupStage->getNext().isEOF());
}

TEST_F(InternalSearchIdLookupTest, ShouldParseFromSerialized) {
    auto expCtx = getExpCtx();
    expCtx->setUUID(UUID::gen());

    DocumentSourceInternalSearchIdLookUp idLookupStage(expCtx);

    // Serialize the idLookup stage, as we would on router.
    vector<Value> serialization;
    idLookupStage.serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::object);

    BSONObj spec = BSON("$_internalSearchIdLookup" << BSONObj());
    ASSERT_BSONOBJ_EQ(serialization[0].getDocument().toBson(), spec);

    // On shard we should be able to re-parse it.
    expCtx->setInRouter(false);
    auto idLookupStageShard =
        DocumentSourceInternalSearchIdLookUp::createFromBson(spec.firstElement(), expCtx);
    ASSERT_EQ(DocumentSourceInternalSearchIdLookUp::kStageName,
              idLookupStageShard->getSourceName());
}

TEST_F(InternalSearchIdLookupTest, ShouldFailToParseInvalidArgumentTypes) {
    auto expCtx = getExpCtx();
    expCtx->setUUID(UUID::gen());

    // Test parsing with not an object.
    ASSERT_THROWS_CODE(
        DocumentSourceInternalSearchIdLookUp::createFromBson(
            BSON("$_internalSearchIdLookup" << "string spec").firstElement(), expCtx),
        AssertionException,
        ErrorCodes::FailedToParse);

    // Test parsing with an unknown field.
    ASSERT_THROWS_CODE(
        DocumentSourceInternalSearchIdLookUp::createFromBson(
            BSON("$_internalSearchIdLookup" << BSON("unknownParameter" << "a")).firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::IDLUnknownField);
}

TEST_F(InternalSearchIdLookupTest, ShouldAllowStringOrObjectIdValues) {
    auto expCtx = getExpCtx();
    expCtx->setUUID(UUID::gen());
    auto specObj = BSON("$_internalSearchIdLookup" << BSONObj());
    auto spec = specObj.firstElement();

    // Set up the idLookup stage.
    auto idLookup = DocumentSourceInternalSearchIdLookUp::createFromBson(spec, expCtx);
    auto idLookupStage = exec::agg::buildStage(idLookup);

    // Mock its input.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        {Document{{"_id", "tango"_sd}},
         Document{{"_id", Document{{"number", 42}, {"irrelevant", "something"_sd}}}}},
        expCtx);
    idLookupStage->setSource(mockLocalStage.get());

    // Mock documents for this namespace.
    deque<DocumentSource::GetNextResult> mockDbContents{
        Document{{"_id", "tango"_sd}, {"color", "red"_sd}},
        Document{{"_id", Document{{"number", 42}, {"irrelevant", "something"_sd}}}}};
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>(mockDbContents));

    // Find documents when _id is a string or document.
    auto next = idLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"_id", "tango"_sd}, {"color", "red"_sd}}));

    next = idLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.releaseDocument(),
        (Document{{"_id", Document{{"number", 42}, {"irrelevant", "something"_sd}}}}));

    ASSERT_TRUE(idLookupStage->getNext().isEOF());
    ASSERT_TRUE(idLookupStage->getNext().isEOF());
}

TEST_F(InternalSearchIdLookupTest, ShouldNotErrorOnEmptyResult) {
    auto expCtx = getExpCtx();
    expCtx->setUUID(UUID::gen());
    auto specObj = BSON("$_internalSearchIdLookup" << BSONObj());
    auto spec = specObj.firstElement();

    // Set up the idLookup stage.
    auto idLookup = DocumentSourceInternalSearchIdLookUp::createFromBson(spec, expCtx);
    auto idLookupStage = exec::agg::buildStage(idLookup);

    // Mock its input.
    auto mockLocalStage = exec::agg::MockStage::createForTest({}, expCtx);
    idLookupStage->setSource(mockLocalStage.get());

    // Mock documents for this namespace.
    deque<DocumentSource::GetNextResult> mockDbContents{Document{{"_id", 0}, {"color", "red"_sd}}};
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>(mockDbContents));

    ASSERT_TRUE(idLookupStage->getNext().isEOF());
    ASSERT_TRUE(idLookupStage->getNext().isEOF());
}

TEST_F(InternalSearchIdLookupTest, RedactsCorrectly) {
    auto expCtx = getExpCtx();
    expCtx->setUUID(UUID::gen());
    auto specObj = BSON("$_internalSearchIdLookup" << BSONObj());
    auto spec = specObj.firstElement();

    auto idLookupStage = DocumentSourceInternalSearchIdLookUp::createFromBson(spec, expCtx);

    auto opts =
        SerializationOptions{.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString};
    std::vector<Value> vec;
    idLookupStage->serializeToArray(vec, opts);
    ASSERT_BSONOBJ_EQ(vec[0].getDocument().toBson(), specObj);

    vec.clear();
    auto limitedLookup = DocumentSourceInternalSearchIdLookUp(expCtx, 5);
    limitedLookup.serializeToArray(vec, opts);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalSearchIdLookup":{"limit":"?number"}})",
        vec[0].getDocument().toBson());
}

TEST_F(InternalSearchIdLookupTest, TestSearchIdLookupMetricsGetLookupSuccessRate) {
    // Test the expected / in-bounds modes of the 'getDocsLookupByIdSuccessRate()' function.
    DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics searchIdLookupMetrics;

    // First, check that with zero documents seen that the success rate is 0.
    ASSERT_EQUALS(double(0), searchIdLookupMetrics.getIdLookupSuccessRate());

    // Now, add some docs as seen.
    for (int i = 0; i < 6; i++) {
        searchIdLookupMetrics.incrementDocsSeenByIdLookup();
    }

    // Ensure that the success rate remains 0.
    ASSERT_EQUALS(double(0), searchIdLookupMetrics.getIdLookupSuccessRate());

    // Now, add some docs successfully found.
    for (int i = 0; i < 3; i++) {
        searchIdLookupMetrics.incrementDocsReturnedByIdLookup();
    }

    // Lastly, ensure that the sucess rate is as expected.
    ASSERT_EQUALS(double(0.5), searchIdLookupMetrics.getIdLookupSuccessRate());
}

DEATH_TEST_F(InternalSearchIdLookupTest,
             TestSearchIdLookupMetricsGetLookupSuccessRateTAssert,
             "9074400") {
    // Check the (should be impossible) case where the number of documents
    // that are found by id is greater than the number of document seen
    // (indicating an error in how / where the metrics are being tracked).
    DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics searchIdLookupMetrics;
    for (int i = 0; i < 5; i++) {
        searchIdLookupMetrics.incrementDocsSeenByIdLookup();
        searchIdLookupMetrics.incrementDocsReturnedByIdLookup();
    }
    // More docs returned than seen.
    searchIdLookupMetrics.incrementDocsReturnedByIdLookup();

    // Expect tassert to be tripped here.
    searchIdLookupMetrics.getIdLookupSuccessRate();
}

}  // namespace
}  // namespace mongo
