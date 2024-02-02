/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/json.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/pipeline/document_source_internal_projection.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/classic_runtime_planner_for_sbe/planner_interface.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_cache_key_factory.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/shard_role.h"

namespace mongo::classic_runtime_planner_for_sbe {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.collection");
/**
 * Fixture for classic_runtime_planner_for_sbe::PlannerInterface implementations. As a test query,
 * it uses an aggregation pipeline [{$match: {a: {$gte: 0}, b: {$gte: 0}}}, {$addFields: {sum:
 * {$add: ["$a", "$b"]}}}] where $match is pushed down to the find query and $addFields is left to
 * be a single-stage agg pipeline.
 */
class ClassicRuntimePlannerForSbeTest : public CatalogTestFixture {
protected:
    const BSONObj kFindFilter = fromjson("{a: {$gte: 0}, b: {$gte: 0}}");
    const BSONObj kAddFieldsSpec = fromjson(R"({sum: {$add: ["$a", "$b"]}})");

    void setUp() final {
        CatalogTestFixture::setUp();
        OperationContext* opCtx = operationContext();
        expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kNss);

        ASSERT_OK(storageInterface()->createCollection(opCtx, kNss, CollectionOptions{}));
        _collections.emplace(acquireCollectionMaybeLockFree(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, kNss, AcquisitionPrerequisites::kRead)));
    }

    void tearDown() final {
        _collections.reset();
        expCtx.reset();
        CatalogTestFixture::tearDown();
    }

    /**
     * Creates PlannerData for the following pipeline: [
     *   {$match: <kFindFilter>}},
     *   {$addField: <kAddFieldsSpec>}}
     * ]
     */
    PlannerData createPlannerData() {
        auto findCommand = std::make_unique<FindCommandRequest>(kNss);
        findCommand->setFilter(kFindFilter);

        auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = expCtx,
            .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)},
            .pipeline = {make_intrusive<DocumentSourceInternalProjection>(
                expCtx, kAddFieldsSpec, InternalProjectionPolicyEnum::kAddFields)}});
        cq->setSbeCompatible(true);

        return {
            .cq = std::move(cq),
            .sbeYieldPolicy = makeYieldPolicy(),
            .workingSet = std::make_unique<WorkingSet>(),
            .collections = *_collections,
            .plannerParams = _params,
            .cachedPlanHash = boost::none,
        };
    }

    /**
     * For a given PlanExecutor, asserts the following:
     * 1. PlanExecutor returns EOF after exactly expectedSums.size() documents.
     * 2. Each returned document have a field "sum" with an integer value, equal to corresponding
     *    element in expectedSums.
     */
    void assertPlanExecutorReturnsCorrectSums(std::vector<int> expectedSums, PlanExecutor* exec) {
        for (int expectedSum : expectedSums) {
            BSONObj out;
            auto state = exec->getNext(&out, nullptr);
            ASSERT_EQ(PlanExecutor::ExecState::ADVANCED, state);
            ASSERT_EQ(expectedSum, out.getField("sum").Int());
        }
        ASSERT_EQ(PlanExecutor::ExecState::IS_EOF, exec->getNextDocument(nullptr, nullptr));
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;

private:
    std::unique_ptr<PlanYieldPolicySBE> makeYieldPolicy() {
        return PlanYieldPolicySBE::make(
            operationContext(),
            PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
            operationContext()->getServiceContext()->getFastClockSource(),
            0,
            Milliseconds::zero(),
            PlanYieldPolicy::YieldThroughAcquisitions{});
    }

    boost::optional<MultipleCollectionAccessor> _collections;
    QueryPlannerParams _params{QueryPlannerParams::DEFAULT};
};

TEST_F(ClassicRuntimePlannerForSbeTest, SingleSolutionPassthroughPlannerCreatesCacheEntry) {
    static const std::vector<BSONArray> kDocs = {
        BSON_ARRAY(int64_t{0} << BSON("a" << 1 << "b" << 2)),
        BSON_ARRAY(int64_t{1} << BSON("a" << 2 << "b" << 2)),
        BSON_ARRAY(int64_t{2} << BSON("a" << 3 << "b" << 2))};

    {  // Run SingleSolutionPassthroughPlanner to create pinned cached entry
        auto root = std::make_unique<VirtualScanNode>(
            kDocs, VirtualScanNode::ScanType::kIxscan, true /*hasRecordId*/, BSON("a" << 1));
        auto solution = std::make_unique<QuerySolution>();
        solution->setRoot(std::move(root));

        SingleSolutionPassthroughPlanner planner{
            operationContext(), createPlannerData(), std::move(solution)};
        auto exec = planner.plan();
        assertPlanExecutorReturnsCorrectSums({3, 4, 5}, exec.get());
    }

    {  // Run CachedPlanner to executed the cached plan
        PlannerData plannerData = createPlannerData();
        auto planCacheKey =
            plan_cache_key_factory::make(*plannerData.cq,
                                         plannerData.collections,
                                         canonical_query_encoder::Optimizer::kSbeStageBuilders);
        auto&& planCache = sbe::getPlanCache(operationContext());
        auto cacheEntry = planCache.getCacheEntryIfActive(planCacheKey);
        ASSERT_TRUE(cacheEntry);
        CachedPlanner cachedPlanner{
            operationContext(), std::move(plannerData), std::move(cacheEntry)};

        auto cachedExec = cachedPlanner.plan();
        assertPlanExecutorReturnsCorrectSums({3, 4, 5}, cachedExec.get());
    }
}

TEST_F(ClassicRuntimePlannerForSbeTest, MultiPlannerPicksMoreEfficientPlan) {
    PlannerData plannerData = createPlannerData();
    std::vector<std::unique_ptr<QuerySolution>> solutions;

    auto addVirtualScanSolution = [&](std::vector<BSONArray> docs) {
        auto virtScan = std::make_unique<VirtualScanNode>(
            std::move(docs), VirtualScanNode::ScanType::kCollScan, false /*hasRecordId*/);
        virtScan->filter = plannerData.cq->getPrimaryMatchExpression()->clone();
        auto solution = std::make_unique<QuerySolution>();
        solution->setRoot(std::move(virtScan));
        solutions.push_back(std::move(solution));
    };

    std::vector<int> expectedSums;
    expectedSums.reserve(200);
    {  // Generate a plan with 400 total documents and 200 documents matching the filter. We expect
       // the multi-planner to choose this plan.
        std::vector<BSONArray> docs;
        docs.reserve(400);
        for (int i = 1; i <= 200; ++i) {
            docs.push_back(BSON_ARRAY(BSON("a" << i << "b" << 1)));
            docs.push_back(BSON_ARRAY(BSON("a" << -i << "b" << 1)));
            expectedSums.push_back(i + 1);
        }
        addVirtualScanSolution(std::move(docs));
    }
    {  // Generate a plan with 600 total documents and 200 documents matching the filter.
        std::vector<BSONArray> docs;
        docs.reserve(600);
        for (int i = 1; i <= 200; ++i) {
            docs.push_back(BSON_ARRAY(BSON("a" << i << "b" << 2)));
            docs.push_back(BSON_ARRAY(BSON("a" << -i << "b" << 2)));
            docs.push_back(BSON_ARRAY(BSON("a" << -i << "b" << 2)));
        }
        addVirtualScanSolution(std::move(docs));
    }

    MultiPlanner planner{operationContext(),
                         std::move(plannerData),
                         PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                         std::move(solutions),
                         PlanCachingMode::AlwaysCache};
    auto exec = planner.plan();
    assertPlanExecutorReturnsCorrectSums(std::move(expectedSums), exec.get());
}

}  // namespace
}  // namespace mongo::classic_runtime_planner_for_sbe
