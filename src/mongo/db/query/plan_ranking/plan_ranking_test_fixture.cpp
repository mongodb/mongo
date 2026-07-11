// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/plan_ranking/plan_ranking_test_fixture.h"

#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/runtime_planners/planner_interface.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"

#include <memory>
#include <utility>

namespace mongo {
namespace plan_ranking {
void PlanRankingTestFixture::setUp() {
    CatalogTestFixture::setUp();
    expCtx = make_intrusive<ExpressionContextForTest>(operationContext(), nss);
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));
}

CollectionAcquisition PlanRankingTestFixture::getCollection() {
    return acquireCollection(
        operationContext(),
        CollectionAcquisitionRequest::fromOpCtx(
            operationContext(), nss, AcquisitionPrerequisites::OperationType::kWrite),
        MODE_X);
}

void PlanRankingTestFixture::insertDocuments(const std::vector<BSONObj>& docs) {
    std::vector<InsertStatement> inserts{docs.begin(), docs.end()};

    auto coll = getCollection();
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

void PlanRankingTestFixture::insertNDocuments(int count) {
    std::vector<BSONObj> docs;
    for (int i = 0; i < count; i++) {
        BSONObj obj = BSON("_id" << i << "a" << i << "b" << i << "c" << i);
        docs.push_back(obj);
    }
    insertDocuments(docs);
}

void PlanRankingTestFixture::tearDown() {
    CatalogTestFixture::tearDown();
    expCtx.reset();
}

void PlanRankingTestFixture::createIndexOnEmptyCollection(OperationContext* opCtx,
                                                          BSONObj index,
                                                          std::string indexName,
                                                          BSONObj options) {
    auto coll = getCollection();
    CollectionWriter collWriter(opCtx, &coll);

    WriteUnitOfWork wunit(opCtx);
    auto indexCatalog = collWriter.getWritableCollection(opCtx)->getIndexCatalog();
    ASSERT(indexCatalog);
    auto indexesBefore = indexCatalog->numIndexesReady();

    auto indexSpec =
        BSON("v" << IndexConfig::kLatestIndexVersion << "key" << index << "name" << indexName)
            .addFields(options);
    const auto insertedSpec = indexCatalog->createIndexOnEmptyCollection(
        opCtx, collWriter.getWritableCollection(opCtx), indexSpec);
    ASSERT_OK(insertedSpec.getStatus());
    wunit.commit();
    ASSERT_EQ(indexesBefore + 1, indexCatalog->numIndexesReady());

    // The QueryPlannerParams should also have information about the index to consider it when
    // actually doing the planning.
    indices.push_back(
        IndexEntry(index,
                   IndexNames::nameToType(IndexNames::findPluginName(index)),
                   IndexConfig::kLatestIndexVersion,
                   false,
                   {},
                   {},
                   false,
                   false,
                   IndexEntry::Identifier{indexName},
                   BSONObj(),
                   nullptr,
                   indexCatalog->findIndexByName(opCtx, indexName)->shared_from_this()));
}

std::pair<std::unique_ptr<CanonicalQuery>, PlannerData>
PlanRankingTestFixture::createCQAndPlannerData(
    const MultipleCollectionAccessor& collections,
    BSONObj findFilter,
    std::function<void(FindCommandRequest&)> modifyFindCmd) {
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(findFilter);
    modifyFindCmd(*findCommand);

    auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = expCtx,
        .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)}});
    cq->getExpCtx()->setExplain(ExplainOptions::Verbosity::kQueryPlanner);

    PlannerData plannerData{
        operationContext(),
        cq.get(),
        std::make_unique<WorkingSet>(),
        collections,
        std::make_shared<QueryPlannerParams>(QueryPlannerParams{QueryPlannerParams::ArgsForTest{}}),
        PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
        /* cachedPlanHash */ boost::none};
    return std::make_pair(std::move(cq), std::move(plannerData));
}

MultipleCollectionAccessor PlanRankingTestFixture::getCollsAccessor() {
    auto coll = getCollection();
    return MultipleCollectionAccessor(
        std::move(coll), {}, false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */);
}

}  // namespace plan_ranking
}  // namespace mongo
