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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/agg/search/internal_search_id_lookup_local_read_executor.h"
#include "mongo/db/exec/agg/search/internal_search_id_lookup_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/single_doc_lookup/express_single_document_lookup_executor.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/catalog_resource_handle.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/process_interface/stub_lookup_single_document_process_interface.h"
#include "mongo/db/pipeline/search/lite_parsed_internal_search_id_lookup.h"
#include "mongo/db/pipeline/stage_params_to_document_source_registry.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/metadata_manager.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/temp_dir.h"

#include <vector>

#include <boost/intrusive_ptr.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

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

        auto stasher = catalogResourceHandle->getStasher();
        PipelineD::buildAndAttachInnerQueryExecutorAndBindCatalogInfoToPipeline(
            collections,
            expCtx->getNamespaceString(),
            nullptr /*resolvedAggRequest*/,
            pipeline.get(),
            stasher);
        return pipeline;
    }
};

// A lookup executor that always declines (kNotHandled). The real executors never decline an _id
// lookup, so this stub lets us verify the stage tasserts rather than silently dropping the input.
class AlwaysNotHandledLookupExecutor final : public exec::agg::SingleDocumentLookupExecutor {
public:
    LookupResult performLookup(const boost::intrusive_ptr<ExpressionContext>&,
                               const NamespaceString&,
                               boost::optional<UUID>,
                               const Document&,
                               boost::optional<Timestamp>) override {
        return {LookupResult::HandledStatus::kNotHandled, boost::none};
    }
};

/**
 * Holds the components created by the idLookup construction helper.
 */
struct IdLookupTestComponents {
    boost::intrusive_ptr<DocumentSourceInternalSearchIdLookUp> docSource;
    exec::agg::StagePtr stage;
    MultipleCollectionAccessor collections;
};

/**
 * Builds an IdLookup stage from catalog resources.
 */
IdLookupTestComponents buildIdLookup(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::pair<boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline>,
              MultipleCollectionAccessor> catalogResources) {
    auto& [sharedStasher, collections] = catalogResources;
    DocumentSourceIdLookupSpec spec;
    spec.setLimit(0LL);
    auto idLookup = make_intrusive<DocumentSourceInternalSearchIdLookUp>(std::move(spec), expCtx);
    idLookup->bindCatalogInfo(collections, sharedStasher);
    auto idLookupStage = exec::agg::buildStage(idLookup);
    return {std::move(idLookup), std::move(idLookupStage), std::move(collections)};
}

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

    void insertDocuments(const NamespaceString& nss, std::span<BSONObj> docs) {
        const auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), nss, AcquisitionPrerequisites::OperationType::kWrite),
            MODE_IX);
        WriteUnitOfWork wuow{operationContext()};
        ASSERT_OK(Helpers::insert(operationContext(), coll.getCollectionPtr(), docs));
        wuow.commit();
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
    IdLookupTestComponents createIdLookup() {
        return buildIdLookup(expCtx, createCatalogResources());
    }

    UUID collectionUUID() {
        AutoGetCollection coll(operationContext(), kTestNss, MODE_IS);
        return coll->uuid();
    }

    // Runs the basic three-document _id lookup and asserts all are returned in order. Shared by the
    // flag-off and flag-on tests so both exercise an identical body: with the Express fast path
    // disabled and enabled the stage must return the same documents.
    void assertReturnsAllDocuments(bool expressEnabled) {
        unittest::ServerParameterGuard flag{"featureFlagSearchOptimizedIdLookup", expressEnabled};
        std::vector<BSONObj> docs{BSON("_id" << 0 << "color" << "red"),
                                  BSON("_id" << 1 << "color" << "blue"),
                                  BSON("_id" << 2 << "color" << "yellow")};
        insertDocuments(kTestNss, docs);
        expCtx->setUUID(collectionUUID());

        auto [idLookup, idLookupStage, collections] = createIdLookup();

        auto mockLocalStage = exec::agg::MockStage::createForTest(
            {Document{{"_id", 0}}, Document{{"_id", 1}}, Document{{"_id", 2}}}, expCtx);
        exec::agg::MockStage::setSource_forTest(idLookupStage, mockLocalStage.get());

        auto next = idLookupStage->getNext();
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"_id", 0}, {"color", "red"sv}}));
        next = idLookupStage->getNext();
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"_id", 1}, {"color", "blue"sv}}));
        next = idLookupStage->getNext();
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"_id", 2}, {"color", "yellow"sv}}));
        ASSERT_TRUE(idLookupStage->getNext().isEOF());

        // Clearing collections as it needs to be destroyed before the stasher.
        collections.clear();
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;
};

// Both paths must return identical documents; run the shared body with the flag off and on.
TEST_F(InternalSearchIdLookupWithCatalogTest, ReturnsAllDocumentsExpressDisabled) {
    assertReturnsAllDocuments(/*expressEnabled=*/false);
}

TEST_F(InternalSearchIdLookupWithCatalogTest, ReturnsAllDocumentsExpressEnabled) {
    assertReturnsAllDocuments(/*expressEnabled=*/true);
}

TEST_F(InternalSearchIdLookupWithCatalogTest,
       BuildIdLookupExecutorReturnsLocalReadExecutorWhenFlagOff) {
    unittest::ServerParameterGuard flag{"featureFlagSearchOptimizedIdLookup", false};
    auto [sharedStasher, collections] = createCatalogResources();
    auto catalogResourceHandle = make_intrusive<DSInternalSearchIdLookUpCatalogResourceHandle>(
        sharedStasher, collections.getMainCollectionAcquisition());
    auto executor =
        exec::agg::buildIdLookupExecutor(expCtx, catalogResourceHandle, boost::none /* view */);
    // No Express fast path: the executor is the local-read executor itself.
    ASSERT_TRUE(
        dynamic_cast<const exec::agg::InternalSearchIdLookUpLocalReadExecutor*>(executor.get()));

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

TEST_F(InternalSearchIdLookupWithCatalogTest, StageHoldsLocalReadExecutorWhenFlagOff) {
    unittest::ServerParameterGuard flag{"featureFlagSearchOptimizedIdLookup", false};
    expCtx->setUUID(UUID::gen());
    auto [idLookup, idLookupStage, collections] = createIdLookup();

    auto* stage = dynamic_cast<exec::agg::InternalSearchIdLookUpStage*>(idLookupStage.get());
    ASSERT_TRUE(stage);
    ASSERT_TRUE(dynamic_cast<const exec::agg::InternalSearchIdLookUpLocalReadExecutor*>(
        stage->getLookupExecutor_forTest()));

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

TEST_F(InternalSearchIdLookupWithCatalogTest, BuildIdLookupExecutorReturnsExpressWhenFlagOn) {
    unittest::ServerParameterGuard flag{"featureFlagSearchOptimizedIdLookup", true};
    auto [sharedStasher, collections] = createCatalogResources();
    auto catalogResourceHandle = make_intrusive<DSInternalSearchIdLookUpCatalogResourceHandle>(
        sharedStasher, collections.getMainCollectionAcquisition());
    auto executor =
        exec::agg::buildIdLookupExecutor(expCtx, catalogResourceHandle, boost::none /* view */);

    ASSERT_TRUE(
        dynamic_cast<const exec::agg::ExpressSingleDocumentLookupExecutor*>(executor.get()));

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

// Express cannot serve a view, so a view forces the local-read executor even with the flag on.
TEST_F(InternalSearchIdLookupWithCatalogTest,
       BuildIdLookupExecutorReturnsLocalReadExecutorWhenViewPresent) {
    unittest::ServerParameterGuard flag{"featureFlagSearchOptimizedIdLookup", true};
    auto [sharedStasher, collections] = createCatalogResources();
    auto catalogResourceHandle = make_intrusive<DSInternalSearchIdLookUpCatalogResourceHandle>(
        sharedStasher, collections.getMainCollectionAcquisition());
    std::vector<BSONObj> viewPipeline{BSON("$match" << BSON("active" << true))};
    auto executor =
        exec::agg::buildIdLookupExecutor(expCtx, catalogResourceHandle, std::move(viewPipeline));
    ASSERT_TRUE(
        dynamic_cast<const exec::agg::InternalSearchIdLookUpLocalReadExecutor*>(executor.get()));

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

TEST_F(InternalSearchIdLookupWithCatalogTest, StageHoldsExpressExecutorWhenFlagOn) {
    unittest::ServerParameterGuard flag{"featureFlagSearchOptimizedIdLookup", true};
    expCtx->setUUID(UUID::gen());
    auto [idLookup, idLookupStage, collections] = createIdLookup();

    auto* stage = dynamic_cast<exec::agg::InternalSearchIdLookUpStage*>(idLookupStage.get());
    ASSERT_TRUE(stage);
    ASSERT_TRUE(dynamic_cast<const exec::agg::ExpressSingleDocumentLookupExecutor*>(
        stage->getLookupExecutor_forTest()));

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

using InternalSearchIdLookupWithCatalogDeathTest = InternalSearchIdLookupWithCatalogTest;

// The installed executor must resolve every _id; kNotHandled tasserts rather than silently dropping
// the input. Drive a stub executor that always declines.
DEATH_TEST_F(InternalSearchIdLookupWithCatalogDeathTest,
             TAssertsWhenExecutorReturnsNotHandled,
             "13006201") {
    expCtx->setUUID(UUID::gen());
    auto [sharedStasher, collections] = createCatalogResources();
    auto catalogResourceHandle = make_intrusive<DSInternalSearchIdLookUpCatalogResourceHandle>(
        sharedStasher, collections.getMainCollectionAcquisition());

    DocumentSourceIdLookupSpec spec;
    spec.setLimit(0LL);
    auto metrics =
        std::make_shared<exec::agg::InternalSearchIdLookUpStage::SearchIdLookupMetrics>();
    exec::agg::StagePtr stage = make_intrusive<exec::agg::InternalSearchIdLookUpStage>(
        DocumentSourceInternalSearchIdLookUp::kStageName,
        std::move(spec),
        expCtx,
        catalogResourceHandle,
        metrics,
        std::make_unique<AlwaysNotHandledLookupExecutor>());

    auto mockLocalStage = exec::agg::MockStage::createForTest({Document{{"_id", 0}}}, expCtx);
    exec::agg::MockStage::setSource_forTest(stage, mockLocalStage.get());

    stage->getNext();  // Expected to tassert: the executor declined an _id lookup.

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

TEST_F(InternalSearchIdLookupWithCatalogTest, ExpressPathSkipsMissingIdWhenFlagOn) {
    unittest::ServerParameterGuard flag{"featureFlagSearchOptimizedIdLookup", true};
    std::vector<BSONObj> docs{BSON("_id" << 0 << "color" << "red"sv)};
    insertDocuments(kTestNss, docs);
    expCtx->setUUID(collectionUUID());

    auto [idLookup, idLookupStage, collections] = createIdLookup();
    auto mockLocalStage =
        exec::agg::MockStage::createForTest({Document{{"_id", 0}}, Document{{"_id", 1}}}, expCtx);
    exec::agg::MockStage::setSource_forTest(idLookupStage, mockLocalStage.get());

    auto next = idLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"_id", 0}, {"color", "red"sv}}));
    ASSERT_TRUE(idLookupStage->getNext().isEOF());

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

TEST_F(InternalSearchIdLookupWithCatalogTest, ExpressPathPreservesMetadataWhenFlagOn) {
    unittest::ServerParameterGuard flag{"featureFlagSearchOptimizedIdLookup", true};
    std::vector<BSONObj> docs{BSON("_id" << 0 << "color" << "red"sv)};
    insertDocuments(kTestNss, docs);
    expCtx->setUUID(collectionUUID());

    MutableDocument docOne(Document({{"_id", 0}}));
    docOne.metadata().setSearchScore(0.123);
    auto mockLocalStage = exec::agg::MockStage::createForTest({docOne.freeze()}, expCtx);

    auto [idLookup, idLookupStage, collections] = createIdLookup();
    exec::agg::MockStage::setSource_forTest(idLookupStage, mockLocalStage.get());

    auto next = idLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    auto result = next.releaseDocument();
    ASSERT_DOCUMENT_EQ(result, (Document{{"_id", 0}, {"color", "red"sv}}));
    ASSERT_EQ(result.metadata().getSearchScore(), 0.123);
    ASSERT_TRUE(idLookupStage->getNext().isEOF());

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

// The Express path must still surface per-lookup execution stats in explain (totalDocs/KeysExamined
// accumulate one per found document on the non-clustered _id index), matching the local-read path.
TEST_F(InternalSearchIdLookupWithCatalogTest, ExpressPathPopulatesExplainExecStatsWhenFlagOn) {
    unittest::ServerParameterGuard flag{"featureFlagSearchOptimizedIdLookup", true};
    std::vector<BSONObj> docs{BSON("_id" << 0 << "color" << "red"sv),
                              BSON("_id" << 1 << "color" << "blue"sv),
                              BSON("_id" << 2 << "color" << "green"sv)};
    insertDocuments(kTestNss, docs);
    expCtx->setUUID(collectionUUID());

    auto [idLookup, idLookupStage, collections] = createIdLookup();
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        {Document{{"_id", 0}}, Document{{"_id", 1}}, Document{{"_id", 2}}}, expCtx);
    exec::agg::MockStage::setSource_forTest(idLookupStage, mockLocalStage.get());

    while (idLookupStage->getNext().isAdvanced()) {
    }

    const auto* stats =
        static_cast<const DocumentSourceIdLookupStats*>(idLookupStage->getSpecificStats());
    ASSERT_EQ(3, stats->planSummaryStats.nReturned);
    ASSERT_EQ(3, stats->planSummaryStats.totalDocsExamined);
    ASSERT_EQ(3, stats->planSummaryStats.totalKeysExamined);
    // The non-clustered collection resolves each _id via its default _id index.
    ASSERT_EQ(1, stats->planSummaryStats.indexesUsed.count("_id_"));

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

TEST_F(InternalSearchIdLookupWithCatalogTest, ShouldSkipResultsWhenIdNotFound) {
    expCtx->setUUID(UUID::gen());

    // Create documents for the collection - only _id = 0 exists.
    std::vector<BSONObj> docs{BSON("_id" << 0 << "color" << "red"sv)};
    insertDocuments(kTestNss, docs);

    auto [idLookup, idLookupStage, collections] = createIdLookup();

    // Mock input to stage.
    auto mockLocalStage =
        exec::agg::MockStage::createForTest({Document{{"_id", 0}}, Document{{"_id", 1}}}, expCtx);
    exec::agg::MockStage::setSource_forTest(idLookupStage, mockLocalStage.get());

    // We should find one document here with _id = 0.
    auto next = idLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"_id", 0}, {"color", "red"sv}}));

    ASSERT_TRUE(idLookupStage->getNext().isEOF());
    ASSERT_TRUE(idLookupStage->getNext().isEOF());

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

TEST_F(InternalSearchIdLookupWithCatalogTest, ShouldNotRemoveMetadata) {
    expCtx->setUUID(UUID::gen());

    // Create documents for the collection.
    std::vector<BSONObj> docs{BSON("_id" << 0 << "color" << "red"sv << "something else"
                                         << "will be projected out"sv)};
    insertDocuments(kTestNss, docs);

    // Create a mock data source with metadata.
    MutableDocument docOne(Document({{"_id", 0}}));
    docOne.metadata().setSearchScore(0.123);
    auto searchScoreDetails = BSON("scoreDetails" << "foo");
    docOne.metadata().setSearchScoreDetails(searchScoreDetails);
    auto mockLocalStage = exec::agg::MockStage::createForTest({docOne.freeze()}, expCtx);

    auto [idLookup, idLookupStage, collections] = createIdLookup();
    exec::agg::MockStage::setSource_forTest(idLookupStage, mockLocalStage.get());

    // Set up a project stage that asks for metadata.
    auto projectSpec = fromjson(
        "{$project: {score: {$meta: \"searchScore\"}, "
        "scoreInfo: {$meta: \"searchScoreDetails\"},"
        " _id: 1, color: 1}}");
    auto project = DocumentSourceProject::createFromBson(projectSpec.firstElement(), expCtx);
    auto projectStage = exec::agg::buildStageAndStitch(project, idLookupStage);

    // We should find one document here with _id = 0.
    auto next = projectStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.releaseDocument(),
        (Document{
            {"_id", 0}, {"color", "red"sv}, {"score", 0.123}, {"scoreInfo", searchScoreDetails}}));

    ASSERT_TRUE(idLookupStage->getNext().isEOF());
    ASSERT_TRUE(idLookupStage->getNext().isEOF());

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

TEST_F(InternalSearchIdLookupWithCatalogTest, ShouldAllowStringOrObjectIdValues) {
    expCtx->setUUID(UUID::gen());

    // Create documents for the collection with string and document _ids.
    std::vector<BSONObj> docs{BSON("_id" << "tango"sv << "color"
                                         << "red"sv),
                              BSON("_id" << BSON("number" << 42 << "irrelevant"
                                                          << "something"sv))};
    insertDocuments(kTestNss, docs);

    // Mock its input with string and document _ids.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        {Document{{"_id", "tango"sv}},
         Document{{"_id", Document{{"number", 42}, {"irrelevant", "something"sv}}}}},
        expCtx);

    auto [idLookup, idLookupStage, collections] = createIdLookup();
    exec::agg::MockStage::setSource_forTest(idLookupStage, mockLocalStage.get());

    // Find documents when _id is a string or document.
    auto next = idLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"_id", "tango"sv}, {"color", "red"sv}}));

    next = idLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.releaseDocument(),
        (Document{{"_id", Document{{"number", 42}, {"irrelevant", "something"sv}}}}));

    ASSERT_TRUE(idLookupStage->getNext().isEOF());
    ASSERT_TRUE(idLookupStage->getNext().isEOF());

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

TEST_F(InternalSearchIdLookupWithCatalogTest, ShouldNotErrorOnEmptyResult) {
    expCtx->setUUID(UUID::gen());

    // Create a document for the collection.
    std::vector<BSONObj> docs{BSON("_id" << 0 << "color" << "red"sv)};
    insertDocuments(kTestNss, docs);

    auto [idLookup, idLookupStage, collections] = createIdLookup();

    // Mock its input.
    auto mockLocalStage = exec::agg::MockStage::createForTest({}, expCtx);
    exec::agg::MockStage::setSource_forTest(idLookupStage, mockLocalStage.get());

    // Should return EOF since the input is empty.
    ASSERT_TRUE(idLookupStage->getNext().isEOF());
    ASSERT_TRUE(idLookupStage->getNext().isEOF());

    // Clearing collections as it needs to be destroyed before the stasher.
    collections.clear();
}

class InternalSearchIdLookupOrphanFilteringTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();
        OperationContext* opCtx = operationContext();

        _client = std::make_unique<DBDirectClient>(opCtx);
        _client->createCollection(kTestNss);

        _expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kTestNss);
        _expCtx->setMongoProcessInterface(std::make_shared<MongoProcessInterfaceForTest>());
    }

    void insertDocuments(const std::vector<BSONObj>& docs) {
        for (const auto& doc : docs) {
            _client->insert(kTestNss, doc);
        }
    }

    /**
     * Sets up sharding metadata for the collection. The shard key is on the "skey" field.
     * Documents with skey in [min, splitPoint) are owned by this shard.
     * Documents with skey in [splitPoint, max) are orphans (owned by other shard).
     */
    CollectionMetadata setupShardingMetadata(int splitPoint) {
        OperationContext* opCtx = operationContext();
        const UUID uuid = [&] {
            AutoGetCollection autoColl(opCtx, kTestNss, MODE_IS);
            return autoColl->uuid();
        }();

        const ShardKeyPattern shardKeyPattern(BSON("skey" << 1));
        const KeyPattern keyPattern = shardKeyPattern.getKeyPattern();

        const OID epoch = OID::gen();
        const Timestamp timestamp(1, 1);
        ChunkVersion version({epoch, timestamp}, {1, 0});

        // Chunk owned by this shard: [MinKey, splitPoint)
        ChunkType ownedChunk(uuid,
                             ChunkRange{keyPattern.globalMin(), BSON("skey" << splitPoint)},
                             version,
                             kMyShardName);
        version.incMinor();

        // Chunk owned by other shard (orphans): [splitPoint, MaxKey)
        ChunkType orphanChunk(uuid,
                              ChunkRange{BSON("skey" << splitPoint), keyPattern.globalMax()},
                              version,
                              ShardId("otherShard"));

        auto rt = RoutingTableHistory::makeNew(kTestNss,
                                               uuid,
                                               keyPattern,
                                               false, /* unsplittable */
                                               nullptr,
                                               false,
                                               epoch,
                                               timestamp,
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               true /* allowMigrations */,
                                               {ownedChunk, orphanChunk});

        CurrentChunkManager cm(makeStandaloneRoutingTableHistory(std::move(rt)));
        ASSERT_EQ(2, cm.numChunks());

        CollectionMetadata metadata(std::move(cm), kMyShardName);

        {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, kTestNss);
            scopedCsr->setCollectionMetadata(opCtx, metadata);
        }

        return metadata;
    }

    std::pair<boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline>,
              MultipleCollectionAccessor>
    createCatalogResources(boost::optional<CollectionMetadata> metadata = boost::none) {
        OperationContext* opCtx = operationContext();

        // Set the shard version to enable shard filtering if metadata is provided.
        boost::optional<ScopedSetShardRole> scopedSetShardRole;
        if (metadata) {
            scopedSetShardRole.emplace(opCtx,
                                       kTestNss,
                                       ShardVersionFactory::make(*metadata),
                                       boost::none /* databaseVersion */);
        }

        auto coll =
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest::fromOpCtx(
                                  opCtx, kTestNss, AcquisitionPrerequisites::OperationType::kRead),
                              MODE_IS);

        auto collections = MultipleCollectionAccessor(
            std::move(coll), {}, false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */);

        auto transactionResourcesStasher =
            make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>();
        stashTransactionResourcesFromOperationContext(opCtx, transactionResourcesStasher.get());

        return {transactionResourcesStasher, std::move(collections)};
    }

    IdLookupTestComponents createIdLookup(
        boost::optional<CollectionMetadata> metadata = boost::none) {
        return buildIdLookup(_expCtx, createCatalogResources(std::move(metadata)));
    }

    // Runs the orphan-filtering scenario (shard key "skey", split at 10) and asserts only the 3
    // owned documents are returned. Shared by the flag-off and flag-on tests so both exercise an
    // identical body: orphan filtering must hold whether or not the Express fast path is enabled.
    void assertFiltersOrphans(bool expressEnabled) {
        unittest::ServerParameterGuard flag{"featureFlagSearchOptimizedIdLookup", expressEnabled};

        std::vector<BSONObj> docs{
            BSON("_id" << 0 << "skey" << 0 << "color" << "red"),      // owned
            BSON("_id" << 1 << "skey" << 5 << "color" << "blue"),     // owned
            BSON("_id" << 2 << "skey" << 10 << "color" << "green"),   // orphan
            BSON("_id" << 3 << "skey" << 15 << "color" << "yellow"),  // orphan
            BSON("_id" << 4 << "skey" << 9 << "color" << "purple"),   // owned
        };
        insertDocuments(docs);

        // Use the real collection UUID: with the Express fast path on, the stage passes
        // expCtx->getUUID() to PreAcquiredCollectionAcquirer, which requires it to match the held
        // acquisition's UUID. (The flag-off local-read path does not validate it.)
        _expCtx->setUUID([&] {
            AutoGetCollection autoColl(operationContext(), kTestNss, MODE_IS);
            return autoColl->uuid();
        }());

        auto metadata = setupShardingMetadata(10);
        auto [idLookup, idLookupStage, collections] = createIdLookup(metadata);

        auto mockLocalStage = exec::agg::MockStage::createForTest({Document{{"_id", 0}},
                                                                   Document{{"_id", 1}},
                                                                   Document{{"_id", 2}},
                                                                   Document{{"_id", 3}},
                                                                   Document{{"_id", 4}}},
                                                                  _expCtx);
        exec::agg::MockStage::setSource_forTest(idLookupStage, mockLocalStage.get());

        // Only the 3 owned documents (skey < 10) are returned; orphans (_id 2, 3) are filtered.
        auto next = idLookupStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                           (Document{{"_id", 0}, {"skey", 0}, {"color", "red"sv}}));
        next = idLookupStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                           (Document{{"_id", 1}, {"skey", 5}, {"color", "blue"sv}}));
        next = idLookupStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                           (Document{{"_id", 4}, {"skey", 9}, {"color", "purple"sv}}));
        ASSERT_TRUE(idLookupStage->getNext().isEOF());

        // Verify metrics: 5 docs seen, 3 returned (2 orphans filtered).
        auto metrics = idLookup->getSearchIdLookupMetrics();
        ASSERT_EQ(5, metrics->getDocsSeenByIdLookup());
        ASSERT_EQ(3, metrics->getDocsReturnedByIdLookup());

        // Clearing collections as it needs to be destroyed before the stasher.
        collections.clear();
    }

    boost::intrusive_ptr<ExpressionContext> _expCtx;
    std::unique_ptr<DBDirectClient> _client;
};

// Orphan filtering must hold identically on both paths: Express drops orphans via the acquisition's
// shard filter, local-read via INCLUDE_SHARD_FILTER. Run the shared body with the flag off and on.
TEST_F(InternalSearchIdLookupOrphanFilteringTest, ShouldFilterOrphanDocumentsExpressDisabled) {
    assertFiltersOrphans(/*expressEnabled=*/false);
}

TEST_F(InternalSearchIdLookupOrphanFilteringTest, ShouldFilterOrphanDocumentsExpressEnabled) {
    assertFiltersOrphans(/*expressEnabled=*/true);
}

// Helper namespace strings for view-binding tests.
const NamespaceString kViewNss =
    NamespaceString::createNamespaceString_forTest("unittests.view_test");
const NamespaceString kResolvedNss =
    NamespaceString::createNamespaceString_forTest("unittests.resolved_coll");

/**
 * Integration test verifying the full flow from LiteParsed -> StageParams -> DocumentSource.
 */
class InternalSearchIdLookupBuildDocumentSourceTest : public AggregationContextFixture {};

const auto kExplain = query_shape::SerializationOptions{
    .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)};

TEST_F(InternalSearchIdLookupBuildDocumentSourceTest,
       BuildDocumentSourceFromLiteParsedWithViewPipeline) {
    BSONObj spec = BSON(DocumentSourceInternalSearchIdLookUp::kStageName << BSON("limit" << 50LL));
    auto liteParsed =
        LiteParsedInternalSearchIdLookUp::parse(kTestNss, spec.firstElement(), LiteParserOptions{});

    // Simulate what handleView() does: invoke bindResolvedNamespace with a view pipeline.
    std::vector<BSONObj> viewPipeline = {BSON("$match" << BSON("active" << true)),
                                         BSON("$sort" << BSON("createdAt" << -1))};
    auto view = ResolvedNamespace::makeForView(kViewNss, kResolvedNss, viewPipeline);

    liteParsed->bindResolvedNamespace(view, {});

    // Now use the registry to build the DocumentSource from the LiteParsed.
    auto docSources = buildDocumentSource(*liteParsed, getExpCtx());

    ASSERT_EQ(docSources.size(), 1U);
    auto* idLookup = dynamic_cast<DocumentSourceInternalSearchIdLookUp*>(docSources.front().get());
    ASSERT_TRUE(idLookup != nullptr);

    // Verify that the stage was created with the correct limit.
    auto serialized = idLookup->serialize(kExplain);
    ASSERT_TRUE(serialized.isObject());
    auto serializedObj = serialized.getDocument().toBson();
    ASSERT_EQ(
        serializedObj[DocumentSourceInternalSearchIdLookUp::kStageName]["limit"].safeNumberLong(),
        50);

    // Verify that the stage was created with the correct view pipeline.
    ASSERT_EQ(serializedObj[DocumentSourceInternalSearchIdLookUp::kStageName]["subPipeline"]
                  .Array()
                  .size(),
              3);
    // $match added by idLookup.
    ASSERT_EQ(serializedObj[DocumentSourceInternalSearchIdLookUp::kStageName]["subPipeline"]
                  .Array()[0]
                  .Obj()
                  .firstElementFieldNameStringData(),
              "$match");
    // $match from the view pipeline.
    ASSERT_EQ(serializedObj[DocumentSourceInternalSearchIdLookUp::kStageName]["subPipeline"]
                  .Array()[1]
                  .Obj()
                  .firstElementFieldNameStringData(),
              "$match");
    // $sort from the view pipeline.
    ASSERT_EQ(serializedObj[DocumentSourceInternalSearchIdLookUp::kStageName]["subPipeline"]
                  .Array()[2]
                  .Obj()
                  .firstElementFieldNameStringData(),
              "$sort");
}

TEST_F(InternalSearchIdLookupBuildDocumentSourceTest,
       BuildDocumentSourceFromLiteParsedWithoutViewPipeline) {
    BSONObj spec = BSON(DocumentSourceInternalSearchIdLookUp::kStageName << BSON("limit" << 25LL));
    auto liteParsed =
        LiteParsedInternalSearchIdLookUp::parse(kTestNss, spec.firstElement(), LiteParserOptions{});

    // Don't invoke bindResolvedNamespace.

    auto docSources = buildDocumentSource(*liteParsed, getExpCtx());

    ASSERT_EQ(docSources.size(), 1U);
    auto* idLookup = dynamic_cast<DocumentSourceInternalSearchIdLookUp*>(docSources.front().get());
    ASSERT_TRUE(idLookup != nullptr);

    // Verify the limit is correct.
    auto serialized = idLookup->serialize(kExplain);
    ASSERT_TRUE(serialized.isObject());
    auto serializedObj = serialized.getDocument().toBson();
    ASSERT_EQ(
        serializedObj[DocumentSourceInternalSearchIdLookUp::kStageName]["limit"].safeNumberLong(),
        25);

    // Verify that the stage was created with no view pipeline.
    ASSERT_EQ(serializedObj[DocumentSourceInternalSearchIdLookUp::kStageName]["subPipeline"]
                  .Array()
                  .size(),
              1);
    // $match added by idLookup.
    ASSERT_EQ(serializedObj[DocumentSourceInternalSearchIdLookUp::kStageName]["subPipeline"]
                  .Array()[0]
                  .Obj()
                  .firstElementFieldNameStringData(),
              "$match");
}

TEST_F(InternalSearchIdLookupBuildDocumentSourceTest,
       HandleViewAndParseFromLiteParsedWithViewPipeline) {
    // Create a user pipeline with $_internalSearchIdLookup as the first stage and a $project after.
    std::vector<BSONObj> userStages = {
        BSON(DocumentSourceInternalSearchIdLookUp::kStageName << BSON("limit" << 100LL)),
        BSON("$project" << BSON("color" << 1 << "_id" << 1))};
    LiteParsedPipeline liteParsedPipeline(kTestNss, userStages);

    // Create a view with a $match and $addFields stage.
    std::vector<BSONObj> viewStages = {BSON("$match" << BSON("status" << "active")),
                                       BSON("$addFields" << BSON("timestamp" << "$$NOW"))};
    auto view = ResolvedNamespace::makeForView(kViewNss, kResolvedNss, viewStages);

    // Call handleView() on the full pipeline, simulating what runAggregate() does.
    liteParsedPipeline.handleView(view, ResolvedNamespaceMap{});

    // Since $_internalSearchIdLookup has kDoNothing policy, the view pipeline should NOT be
    // prepended. The LiteParsedPipeline should still have 2 stages, not 4.
    ASSERT_EQ(liteParsedPipeline.getStages().size(), 2U);
    ASSERT_EQ(liteParsedPipeline.getStages()[0]->getParseTimeName(),
              DocumentSourceInternalSearchIdLookUp::kStageName);
    ASSERT_EQ(liteParsedPipeline.getStages()[1]->getParseTimeName(), "$project");

    // Parse the full pipeline using Pipeline::parseFromLiteParsed().
    auto pipeline = Pipeline::parseFromLiteParsed(liteParsedPipeline, getExpCtx());
    ASSERT_TRUE(pipeline != nullptr);

    // The pipeline should have 2 document sources.
    const auto& sources = pipeline->getSources();
    ASSERT_EQ(sources.size(), 2U);

    // First stage should be $_internalSearchIdLookup with the view pipeline stored internally.
    auto* idLookup = dynamic_cast<DocumentSourceInternalSearchIdLookUp*>(sources.front().get());
    ASSERT_TRUE(idLookup != nullptr);

    auto serialized = idLookup->serialize(kExplain);
    auto serializedObj = serialized.getDocument().toBson();

    // Verify the view pipeline was captured in the idLookup's subPipeline. subPipeline should
    // contain: $match (id filter) + $match (view) + $addFields (view)
    auto subPipeline =
        serializedObj[DocumentSourceInternalSearchIdLookUp::kStageName]["subPipeline"].Array();
    ASSERT_EQ(subPipeline.size(), 3U);
    ASSERT_EQ(subPipeline[0].Obj().firstElementFieldNameStringData(), "$match");      // id filter
    ASSERT_EQ(subPipeline[1].Obj().firstElementFieldNameStringData(), "$match");      // from view
    ASSERT_EQ(subPipeline[2].Obj().firstElementFieldNameStringData(), "$addFields");  // from view
}

TEST_F(InternalSearchIdLookupBuildDocumentSourceTest,
       HandleViewWithIdLookupNotFirstStagePrependsViewPipeline) {
    // Create a user pipeline where $_internalSearchIdLookup is NOT the first stage. In real usage,
    // this would be after a mongot stage like $search.
    std::vector<BSONObj> userStages = {
        BSON("$match" << BSON("x" << 1)),
        BSON(DocumentSourceInternalSearchIdLookUp::kStageName << BSON("limit" << 30LL))};
    LiteParsedPipeline liteParsedPipeline(kTestNss, userStages);

    ASSERT_EQ(liteParsedPipeline.getStages().size(), 2U);

    // Create a view pipeline.
    std::vector<BSONObj> viewStages = {BSON("$match" << BSON("active" << true))};
    auto view = ResolvedNamespace::makeForView(kViewNss, kResolvedNss, viewStages);

    // Call handleView().
    liteParsedPipeline.handleView(view, ResolvedNamespaceMap{});

    // Since the first stage is $match (which has kDefaultPrepend policy), the view pipeline should
    // be prepended. The LiteParsedPipeline should now have 3 stages.
    ASSERT_EQ(liteParsedPipeline.getStages().size(), 3U);
    ASSERT_EQ(liteParsedPipeline.getStages()[0]->getParseTimeName(), "$match");  // from view
    ASSERT_EQ(liteParsedPipeline.getStages()[1]->getParseTimeName(), "$match");  // original first
    ASSERT_EQ(liteParsedPipeline.getStages()[2]->getParseTimeName(),
              DocumentSourceInternalSearchIdLookUp::kStageName);

    // Parse the pipeline.
    auto pipeline = Pipeline::parseFromLiteParsed(liteParsedPipeline, getExpCtx());
    ASSERT_TRUE(pipeline != nullptr);

    const auto& sources = pipeline->getSources();
    ASSERT_EQ(sources.size(), 3U);

    // The $_internalSearchIdLookup stage (now third) should also have captured the view pipeline
    // via the callback.
    auto it = sources.begin();
    std::advance(it, 2);
    auto* idLookup = dynamic_cast<DocumentSourceInternalSearchIdLookUp*>(it->get());
    ASSERT_TRUE(idLookup != nullptr);

    auto serialized = idLookup->serialize(kExplain);
    auto serializedObj = serialized.getDocument().toBson();

    // Even though the view pipeline was prepended, the idLookup also captured it via its callback.
    // subPipeline should contain: $match (id filter) + $match (from view).
    auto subPipeline =
        serializedObj[DocumentSourceInternalSearchIdLookUp::kStageName]["subPipeline"].Array();
    ASSERT_EQ(subPipeline.size(), 2U);
    ASSERT_EQ(subPipeline[0].Obj().firstElementFieldNameStringData(), "$match");  // id filter
    ASSERT_EQ(subPipeline[1].Obj().firstElementFieldNameStringData(), "$match");  // from view
}

// Tests for the non-ticketed interval tracking behavior introduced to exclude mongot network I/O
// wait time from the tracked intervals, while still tracking in-memory post-search work.
// See internal_search_id_lookup_stage.cpp for the implementation.
//
// After each per-document acquire()/release() cycle, the inner local-lookup pipeline also runs
// and its own cursor stage calls release() upon completion, leaving hasIntervalStart=true. This
// interval covers the legitimate in-memory work period that follows the local read. The outer
// release() interval (which would span mongot network I/O) is immediately suppressed before the
// inner pipeline runs.

TEST_F(InternalSearchIdLookupWithCatalogTest, SearchIntervalOpenAfterPerDocumentLookup) {
    expCtx->setUUID(UUID::gen());
    std::vector<BSONObj> docs{BSON("_id" << 0 << "x" << "a"), BSON("_id" << 1 << "x" << "b")};
    insertDocuments(kTestNss, docs);

    auto [idLookup, idLookupStage, collections] = createIdLookup();
    auto mockLocalStage =
        exec::agg::MockStage::createForTest({Document{{"_id", 0}}, Document{{"_id", 1}}}, expCtx);
    exec::agg::MockStage::setSource_forTest(idLookupStage, mockLocalStage.get());

    auto& tracker = getAggNonTicketedIntervalTracker(operationContext());
    ASSERT_FALSE(tracker.hasIntervalStart);

    // After each id lookup the inner cursor's release() opens an interval, so hasIntervalStart
    // is true when getNext() returns. That interval is then closed by acquire() at the start of
    // the next document's cycle, which suppresses any accumulated mongot network-wait time.
    auto next = idLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_TRUE(tracker.hasIntervalStart);

    next = idLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_TRUE(tracker.hasIntervalStart);

    // After EOF the interval remains open for subsequent in-memory stages (e.g. $sort, $group).
    ASSERT_TRUE(idLookupStage->getNext().isEOF());
    ASSERT_TRUE(tracker.hasIntervalStart);

    collections.clear();
}

// When the search source is empty (zero docs from mongot), no inner cursor ever runs, so
// openIntervalForSubsequentWork() must explicitly open the interval at EOF to ensure that
// subsequent in-memory aggregation stages (e.g. $sort on an empty result set) are tracked.
TEST_F(InternalSearchIdLookupWithCatalogTest, SearchIntervalExplicitlyOpenedForEmptySource) {
    expCtx->setUUID(UUID::gen());

    auto [idLookup, idLookupStage, collections] = createIdLookup();
    auto mockLocalStage = exec::agg::MockStage::createForTest({}, expCtx);
    exec::agg::MockStage::setSource_forTest(idLookupStage, mockLocalStage.get());

    auto& tracker = getAggNonTicketedIntervalTracker(operationContext());
    ASSERT_FALSE(tracker.hasIntervalStart);

    ASSERT_TRUE(idLookupStage->getNext().isEOF());
    ASSERT_TRUE(tracker.hasIntervalStart);

    collections.clear();
}

TEST_F(InternalSearchIdLookupWithCatalogTest, SearchIntervalOpenedAtLimitBasedEOF) {
    expCtx->setUUID(UUID::gen());
    std::vector<BSONObj> docs{BSON("_id" << 0 << "x" << "a"), BSON("_id" << 1 << "x" << "b")};
    insertDocuments(kTestNss, docs);

    // Build a stage with limit=1 to exercise the limit-based early EOF path.
    auto catalogResources = createCatalogResources();
    auto& [sharedStasher, collections] = catalogResources;
    DocumentSourceIdLookupSpec spec;
    spec.setLimit(1LL);
    auto idLookup = make_intrusive<DocumentSourceInternalSearchIdLookUp>(std::move(spec), expCtx);
    idLookup->bindCatalogInfo(collections, sharedStasher);
    auto idLookupStage = exec::agg::buildStage(idLookup);

    auto mockLocalStage =
        exec::agg::MockStage::createForTest({Document{{"_id", 0}}, Document{{"_id", 1}}}, expCtx);
    exec::agg::MockStage::setSource_forTest(idLookupStage, mockLocalStage.get());

    auto& tracker = getAggNonTicketedIntervalTracker(operationContext());
    ASSERT_FALSE(tracker.hasIntervalStart);

    // After the first document the inner cursor's release() opened an interval.
    auto next = idLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_TRUE(tracker.hasIntervalStart);

    // The next call hits the limit check; the interval remains open for subsequent in-memory work.
    ASSERT_TRUE(idLookupStage->getNext().isEOF());
    ASSERT_TRUE(tracker.hasIntervalStart);

    collections.clear();
}

}  // namespace
}  // namespace mongo
