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
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/catalog_resource_handle.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/process_interface/stub_lookup_single_document_process_interface.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"

#include <vector>

#include <boost/intrusive_ptr.hpp>

namespace mongo {
namespace {

using boost::intrusive_ptr;
using std::vector;

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("unittests.pipeline_test");

class InternalSearchIdLookupTest : public unittest::Test {};

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

using InternalSearchIdLookupTestDeathTest = InternalSearchIdLookupTest;
DEATH_TEST_F(InternalSearchIdLookupTestDeathTest,
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

class MongoProcessInterfaceForTest : public StubMongoProcessInterface {
public:
    std::unique_ptr<Pipeline> attachCursorSourceToPipelineForLocalReadWithCatalog(
        std::unique_ptr<Pipeline> pipeline,
        const MultipleCollectionAccessor& collections,
        const boost::intrusive_ptr<CatalogResourceHandle>& catalogResourceHandle) override {

        const boost::intrusive_ptr<ExpressionContext>& expCtx = pipeline->getContext();

        auto cursorCatalogResourceHandle =
            make_intrusive<DSCursorCatalogResourceHandle>(catalogResourceHandle->getStasher());
        PipelineD::buildAndAttachInnerQueryExecutorToPipeline(collections,
                                                              expCtx->getNamespaceString(),
                                                              nullptr /*resolvedAggRequest*/,
                                                              pipeline.get(),
                                                              cursorCatalogResourceHandle);

        return pipeline;
    }
};

class InternalSearchIdLookupWithCatalogTest : public CatalogTestFixture {
protected:
    void setUp() final {
        CatalogTestFixture::setUp();
        OperationContext* opCtx = operationContext();
        expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kTestNss);
        expCtx->setMongoProcessInterface(std::make_shared<MongoProcessInterfaceForTest>());
        ASSERT_OK(storageInterface()->createCollection(
            operationContext(), kTestNss, CollectionOptions()));
    }

    void tearDown() final {
        expCtx.reset();
        CatalogTestFixture::tearDown();
    }

    void insertDocuments(const NamespaceString& nss, std::vector<BSONObj> docs) {
        std::vector<InsertStatement> inserts{docs.begin(), docs.end()};

        const auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), nss, AcquisitionPrerequisites::OperationType::kWrite),
            MODE_IX);
        {
            WriteUnitOfWork wuow{operationContext()};
            ASSERT_OK(collection_internal::insertDocuments(operationContext(),
                                                           coll.getCollectionPtr(),
                                                           inserts.begin(),
                                                           inserts.end(),
                                                           nullptr /* opDebug */));
            wuow.commit();
        }
    }

    std::pair<boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline>,
              MultipleCollectionAccessor>
    createCatalogResources() {

        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), kTestNss, AcquisitionPrerequisites::OperationType::kRead),
            MODE_IS);
        auto collections = MultipleCollectionAccessor(
            std::move(coll), {}, false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */);

        auto transactionResourcesStasher =
            make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>();
        stashTransactionResourcesFromOperationContext(operationContext(),
                                                      transactionResourcesStasher.get());
        return {transactionResourcesStasher, collections};
    }
    boost::intrusive_ptr<ExpressionContext> expCtx;
};

TEST_F(InternalSearchIdLookupWithCatalogTest, BasicSearchTest) {
    expCtx->setUUID(UUID::gen());
    std::vector<BSONObj> docs{BSON("_id" << 0 << "color" << "red"),
                              BSON("_id" << 1 << "color" << "blue"),
                              BSON("_id" << 2 << "color" << "yellow")};

    insertDocuments(kTestNss, docs);

    auto idLookup = make_intrusive<DocumentSourceInternalSearchIdLookUp>(expCtx, 0 /*limit*/);
    // Create catalog resources.
    auto [sharedStasher, collections] = createCatalogResources();
    idLookup->bindCatalogInfo(collections, sharedStasher);

    auto idLookupStage = exec::agg::buildStage(idLookup);

    // Mock its input.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        {Document{{"_id", 0}}, Document{{"_id", 1}}, Document{{"_id", 2}}}, expCtx);
    idLookupStage->setSource(mockLocalStage.get());

    // We should find one document here with _id = 0.
    auto next = idLookupStage->getNext();
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"_id", 0}, {"color", "red"_sd}}));

    next = idLookupStage->getNext();
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"_id", 1}, {"color", "blue"_sd}}));

    next = idLookupStage->getNext();
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"_id", 2}, {"color", "yellow"_sd}}));

    ASSERT_TRUE(idLookupStage->getNext().isEOF());

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

TEST_F(InternalSearchIdLookupWithCatalogTest, ShouldSkipResultsWhenIdNotFound) {
    expCtx->setUUID(UUID::gen());

    // Create documents for the collection - only _id = 0 exists.
    std::vector<BSONObj> docs{BSON("_id" << 0 << "color" << "red"_sd)};
    insertDocuments(kTestNss, docs);

    auto idLookup = make_intrusive<DocumentSourceInternalSearchIdLookUp>(expCtx, 0 /*limit*/);
    // Create catalog resources.
    auto [sharedStasher, collections] = createCatalogResources();
    idLookup->bindCatalogInfo(collections, sharedStasher);

    auto idLookupStage = exec::agg::buildStage(idLookup);

    // Mock input to stage.
    auto mockLocalStage =
        exec::agg::MockStage::createForTest({Document{{"_id", 0}}, Document{{"_id", 1}}}, expCtx);
    idLookupStage->setSource(mockLocalStage.get());

    // We should find one document here with _id = 0.
    auto next = idLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"_id", 0}, {"color", "red"_sd}}));

    ASSERT_TRUE(idLookupStage->getNext().isEOF());
    ASSERT_TRUE(idLookupStage->getNext().isEOF());

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

TEST_F(InternalSearchIdLookupWithCatalogTest, ShouldNotRemoveMetadata) {
    expCtx->setUUID(UUID::gen());

    // Create documents for the collection.
    std::vector<BSONObj> docs{BSON("_id" << 0 << "color" << "red"_sd << "something else"
                                         << "will be projected out"_sd)};
    insertDocuments(kTestNss, docs);

    // Create a mock data source with metadata.
    MutableDocument docOne(Document({{"_id", 0}}));
    docOne.metadata().setSearchScore(0.123);
    auto searchScoreDetails = BSON("scoreDetails" << "foo");
    docOne.metadata().setSearchScoreDetails(searchScoreDetails);
    auto mockLocalStage = exec::agg::MockStage::createForTest({docOne.freeze()}, expCtx);

    auto idLookup = make_intrusive<DocumentSourceInternalSearchIdLookUp>(expCtx, 0 /*limit*/);
    // Create catalog resources.
    auto [sharedStasher, collections] = createCatalogResources();
    idLookup->bindCatalogInfo(collections, sharedStasher);

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

    // We should find one document here with _id = 0.
    auto next = projectStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.releaseDocument(),
        (Document{
            {"_id", 0}, {"color", "red"_sd}, {"score", 0.123}, {"scoreInfo", searchScoreDetails}}));

    ASSERT_TRUE(idLookupStage->getNext().isEOF());
    ASSERT_TRUE(idLookupStage->getNext().isEOF());

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

TEST_F(InternalSearchIdLookupWithCatalogTest, ShouldAllowStringOrObjectIdValues) {
    expCtx->setUUID(UUID::gen());

    // Create documents for the collection with string and document _ids.
    std::vector<BSONObj> docs{BSON("_id" << "tango"_sd << "color"
                                         << "red"_sd),
                              BSON("_id" << BSON("number" << 42 << "irrelevant"
                                                          << "something"_sd))};
    insertDocuments(kTestNss, docs);

    // Mock its input with string and document _ids.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        {Document{{"_id", "tango"_sd}},
         Document{{"_id", Document{{"number", 42}, {"irrelevant", "something"_sd}}}}},
        expCtx);

    auto idLookup = make_intrusive<DocumentSourceInternalSearchIdLookUp>(expCtx, 0 /*limit*/);
    // Create catalog resources.
    auto [sharedStasher, collections] = createCatalogResources();
    idLookup->bindCatalogInfo(collections, sharedStasher);

    auto idLookupStage = exec::agg::buildStage(idLookup);
    idLookupStage->setSource(mockLocalStage.get());

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

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

TEST_F(InternalSearchIdLookupWithCatalogTest, ShouldNotErrorOnEmptyResult) {
    expCtx->setUUID(UUID::gen());

    // Create a document for the collection.
    std::vector<BSONObj> docs{BSON("_id" << 0 << "color" << "red"_sd)};
    insertDocuments(kTestNss, docs);

    auto idLookup = make_intrusive<DocumentSourceInternalSearchIdLookUp>(expCtx, 0 /*limit*/);
    // Create catalog resources.
    auto [sharedStasher, collections] = createCatalogResources();
    idLookup->bindCatalogInfo(collections, sharedStasher);

    auto idLookupStage = exec::agg::buildStage(idLookup);

    // Mock its input.
    auto mockLocalStage = exec::agg::MockStage::createForTest({}, expCtx);
    idLookupStage->setSource(mockLocalStage.get());

    // Should return EOF since the input is empty.
    ASSERT_TRUE(idLookupStage->getNext().isEOF());
    ASSERT_TRUE(idLookupStage->getNext().isEOF());

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

}  // namespace
}  // namespace mongo
