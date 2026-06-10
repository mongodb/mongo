/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/stats/collection_statistics_impl.h"
#include "mongo/db/query/plan_ranking/plan_ranker.h"
#include "mongo/db/query/plan_ranking/plan_ranking_test_fixture.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.plan_ranker");

// Exercises 'PlanRanker::rankPlans' directly, focusing on the unified single-solution short circuit
// that decides whether a ranking strategy runs at all. The decision lives entirely in
// 'PlanRanker::rankPlans' (see plan_ranker.cpp): a single solution skips ranking when CBR is
// unavailable, when the solution is a count scan, or when the query is not an explain. Otherwise
// the solution is ranked so that costing information can be displayed.
class PlanRankerTest : public plan_ranking::PlanRankingTestFixture {
public:
    PlanRankerTest() : PlanRankingTestFixture(kNss) {}

    std::shared_ptr<QueryPlannerParams> makePlannerParams(
        bool cbrEnabled,
        QueryPlanRankerModeEnum planRankerMode = QueryPlanRankerModeEnum::kAutomaticCE) {
        auto res = std::make_shared<QueryPlannerParams>(QueryPlannerParams::ArgsForTest{});
        res->mainCollectionInfo.indexes = indices;
        res->cbrEnabled = cbrEnabled;
        res->planRankerMode = planRankerMode;
        return res;
    }

    StatusWith<PlanRankingResult> rankPlans(CanonicalQuery& cq,
                                            PlannerData plannerData,
                                            bool isClassic) {
        // Capture references before moving 'plannerData'; the QueryPlannerParams stays alive via
        // the shared_ptr and 'collections' references the externally owned accessor.
        auto* opCtx = plannerData.opCtx;
        auto& plannerParams = *plannerData.plannerParams;
        const auto& collections = plannerData.collections;
        auto yieldPolicy = plannerData.yieldPolicy;

        plan_ranking::PlanRanker planRanker;
        return planRanker.rankPlans(
            opCtx, cq, plannerParams, yieldPolicy, collections, std::move(plannerData), isClassic);
    }
};

// Multiple candidate solutions are never short circuited; the strategy must run. With CBR disabled
// the multiplanning strategy returns every solution for later runtime multiplanning.
TEST_F(PlanRankerTest, MultipleSolutionsAreRanked) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(10);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] = createCQAndPlannerData(colls, BSON("a" << 4 << "b" << 4));
    plannerData.plannerParams = makePlannerParams(/* cbrEnabled */ false);

    auto status = rankPlans(*cq, std::move(plannerData), /* isClassic */ true);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 2);
    ASSERT_FALSE(status.getValue().execState);
}

// A single solution skips ranking when CBR is disabled: no costing or multiplanning is required, so
// the lone solution is returned untouched.
TEST_F(PlanRankerTest, SingleSolutionWithCbrDisabledIsNotRanked) {
    insertNDocuments(10);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] = createCQAndPlannerData(colls, BSON("a" << 42 << "b" << 7));
    plannerData.plannerParams = makePlannerParams(/* cbrEnabled */ false);

    auto status = rankPlans(*cq, std::move(plannerData), /* isClassic */ true);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    ASSERT_FALSE(status.getValue().execState);
    ASSERT_FALSE(status.getValue().maybeExplainData.has_value());
}

// canUseCBR also requires the classic engine. A single solution destined for SBE skips ranking even
// when cbrEnabled is set, because CBR fallback strategies only run with classic.
TEST_F(PlanRankerTest, SingleSolutionForSbeIsNotRanked) {
    insertNDocuments(10);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] = createCQAndPlannerData(colls, BSON("a" << 42 << "b" << 7));
    plannerData.plannerParams = makePlannerParams(/* cbrEnabled */ true);

    auto status = rankPlans(*cq, std::move(plannerData), /* isClassic */ false);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    ASSERT_FALSE(status.getValue().execState);
    ASSERT_FALSE(status.getValue().maybeExplainData.has_value());
}

// With CBR available but the query not being an explain, a single solution skips ranking: there is
// nothing to rank against and the costing information would never be displayed.
TEST_F(PlanRankerTest, SingleSolutionWithoutExplainIsNotRanked) {
    insertNDocuments(10);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] = createCQAndPlannerData(colls, BSON("a" << 42 << "b" << 7));
    cq->getExpCtx()->setExplain(boost::none);
    plannerData.plannerParams = makePlannerParams(/* cbrEnabled */ true);

    auto status = rankPlans(*cq, std::move(plannerData), /* isClassic */ true);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    // No strategy ran, so no execution state was produced.
    ASSERT_FALSE(status.getValue().execState);
    ASSERT_FALSE(status.getValue().maybeExplainData.has_value());
}

// With CBR available and the query being an explain, a single non-count solution is ranked so that
// the explain output can carry costing information. Under a costing strategy (here heuristic CE),
// the lone solution is costed and the resulting estimates are surfaced in the explain data.
TEST_F(PlanRankerTest, SingleSolutionWithExplainIsCosted) {
    insertNDocuments(10);
    auto colls = getCollsAccessor();

    // The fixture sets explain to kQueryPlanner by default.
    auto [cq, plannerData] = createCQAndPlannerData(colls, BSON("a" << 42 << "b" << 7));
    plannerData.plannerParams =
        makePlannerParams(/* cbrEnabled */ true, QueryPlanRankerModeEnum::kHeuristicCE);
    plannerData.plannerParams->mainCollectionInfo.collStats =
        std::make_unique<stats::CollectionStatisticsImpl>(static_cast<double>(10), kNss);

    auto status = rankPlans(*cq, std::move(plannerData), /* isClassic */ true);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    // The strategy ran rather than being short circuited, so it produced costing estimates.
    ASSERT_TRUE(status.getValue().maybeExplainData.has_value());
    ASSERT_FALSE(status.getValue().maybeExplainData->estimates.empty());
}

}  // namespace
}  // namespace mongo
