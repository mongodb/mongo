/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

void PlanRankingTestFixture::insertNDocuments(int count) {
    std::vector<BSONObj> docs;
    for (int i = 0; i < count; i++) {
        BSONObj obj = BSON("_id" << i << "a" << i << "b" << i << "c" << i);
        docs.push_back(obj);
    }
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

void PlanRankingTestFixture::tearDown() {
    CatalogTestFixture::tearDown();
    expCtx.reset();
}

void PlanRankingTestFixture::createIndexOnEmptyCollection(OperationContext* opCtx,
                                                          BSONObj index,
                                                          std::string indexName) {
    auto coll = getCollection();
    CollectionWriter collWriter(opCtx, &coll);

    WriteUnitOfWork wunit(opCtx);
    auto indexCatalog = collWriter.getWritableCollection(opCtx)->getIndexCatalog();
    ASSERT(indexCatalog);
    auto indexesBefore = indexCatalog->numIndexesReady();

    auto indexSpec =
        BSON("v" << IndexConfig::kLatestIndexVersion << "key" << index << "name" << indexName);
    ASSERT_OK(indexCatalog
                  ->createIndexOnEmptyCollection(
                      opCtx, collWriter.getWritableCollection(opCtx), indexSpec)
                  .getStatus());
    wunit.commit();
    ASSERT_EQ(indexesBefore + 1, indexCatalog->numIndexesReady());

    // The QueryPlannerParams should also have information about the index to consider it when
    // actually doing the planning.
    indices.push_back(IndexEntry(index,
                                 IndexNames::nameToType(IndexNames::findPluginName(index)),
                                 IndexConfig::kLatestIndexVersion,
                                 false,
                                 {},
                                 {},
                                 false,
                                 false,
                                 IndexEntry::Identifier{indexName},
                                 BSONObj(),
                                 nullptr));
}

std::pair<std::unique_ptr<CanonicalQuery>, PlannerData>
PlanRankingTestFixture::createCQAndPlannerData(const MultipleCollectionAccessor& collections,
                                               BSONObj findFilter) {
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(findFilter);

    auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = expCtx,
        .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)}});


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
