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
#include "mongo/db/query/plan_ranking/cbr_for_no_mp_results.h"

#include "mongo/db/query/compiler/stats/collection_statistics_impl.h"
#include "mongo/db/query/plan_ranking/plan_ranking_test_fixture.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.collection");

class CBRForNoMPResultsStrategyMock : public plan_ranking::CBRForNoMPResultsStrategy {
public:
    boost::optional<classic_runtime_planner::MultiPlanner>& getMultiPlanner() {
        return _multiPlanner;
    }
};

class CBRForNoMPResultsTest : public plan_ranking::PlanRankingTestFixture {
public:
    CBRForNoMPResultsTest() : PlanRankingTestFixture(kNss) {}
};

TEST_F(CBRForNoMPResultsTest, SingleSolutionDoesNotUseMultiPlanner) {
    insertNDocuments(10);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] = createCQAndPlannerData(colls, BSON("a" << 42 << "b" << 7));

    auto params = QueryPlannerParams{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.indexes = indices;
    CBRForNoMPResultsStrategyMock strategy;
    auto status = strategy.rankPlans(*cq,
                                     params,
                                     PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                     colls,
                                     operationContext(),
                                     std::move(plannerData));
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    ASSERT_EQ(status.getValue().rejectedPlans.size(), 0);
    ASSERT_EQ(status.getValue().estimates.size(), 0);
    ASSERT_EQ(strategy.getMultiPlanner(), boost::none);
}

TEST_F(CBRForNoMPResultsTest, EOFMultiPlannerMakesADecisionWithoutCBR) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(10);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] = createCQAndPlannerData(colls, BSON("a" << 4 << "b" << 4));

    auto params = QueryPlannerParams{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.indexes = indices;
    CBRForNoMPResultsStrategyMock strategy;
    auto status = strategy.rankPlans(*cq,
                                     params,
                                     PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                     colls,
                                     operationContext(),
                                     std::move(plannerData));
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    // TODO SERVER-115402. Fix this to include rejected plans.
    ASSERT_EQ(status.getValue().rejectedPlans.size(), 0);
    ASSERT_EQ(status.getValue().estimates.size(), 0);
    ASSERT_TRUE(strategy.getMultiPlanner().has_value());
    auto stats = strategy.getMultiPlanner()->getSpecificStats();
    ASSERT_TRUE(stats->earlyExit);
    ASSERT_EQ(stats->numResultsFound, 2);  // One result per plan
    ASSERT_EQ(stats->numCandidatePlans, 2);
    ASSERT_EQ(stats->totalWorks, 4);  // 2 works per plan (seek + advance)
}

TEST_F(CBRForNoMPResultsTest, BatchFilledMultiPlannerMakesADecisionWithoutCBR) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(200);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] =
        createCQAndPlannerData(colls, BSON("a" << GT << 1 << "b" << LT << 200));

    auto params = QueryPlannerParams{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.indexes = indices;
    CBRForNoMPResultsStrategyMock strategy;
    auto status = strategy.rankPlans(*cq,
                                     params,
                                     PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                     colls,
                                     operationContext(),
                                     std::move(plannerData));
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    // TODO SERVER-115402. Fix this to include rejected plans.
    ASSERT_EQ(status.getValue().rejectedPlans.size(), 0);
    ASSERT_EQ(status.getValue().estimates.size(), 0);
    ASSERT_TRUE(strategy.getMultiPlanner().has_value());
    auto stats = strategy.getMultiPlanner()->getSpecificStats();
    ASSERT_TRUE(stats->earlyExit);
    ASSERT_EQ(stats->numResultsFound, 200);  // Batch filled during trials
    ASSERT_EQ(stats->numCandidatePlans, 2);
    ASSERT_EQ(stats->totalWorks, 202);  // 1 work to seek + 200 advances per plan
}

TEST_F(CBRForNoMPResultsTest, LittleResultsMultiPlannerMakesADecisionWithoutCBR) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(10001);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] =
        createCQAndPlannerData(colls, BSON("a" << GT << 0 << "b" << GT << 0 << "c" << 10));

    auto params = QueryPlannerParams{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.indexes = indices;
    CBRForNoMPResultsStrategyMock strategy;
    auto status = strategy.rankPlans(*cq,
                                     params,
                                     PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                     colls,
                                     operationContext(),
                                     std::move(plannerData));
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    // TODO SERVER-115402. Fix this to include rejected plans.
    ASSERT_EQ(status.getValue().rejectedPlans.size(), 0);
    ASSERT_EQ(status.getValue().estimates.size(), 0);
    ASSERT_TRUE(strategy.getMultiPlanner().has_value());
    auto stats = strategy.getMultiPlanner()->getSpecificStats();
    ASSERT_FALSE(stats->earlyExit);
    ASSERT_EQ(stats->numResultsFound, 2);  // One per plan
    ASSERT_EQ(stats->numCandidatePlans, 2);
    ASSERT_EQ(stats->totalWorks, 20000);  // Each plan exhausted all credits (10k)
}

TEST_F(CBRForNoMPResultsTest, NoResultsMultiPlannerUsesCBR) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(5001);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] =
        createCQAndPlannerData(colls, BSON("a" << GT << 0 << "b" << GT << 0 << "c" << -1));

    auto params = QueryPlannerParams{QueryPlannerParams::ArgsForTest{}};
    params.mainCollectionInfo.indexes = indices;
    params.mainCollectionInfo.collStats =
        std::make_unique<stats::CollectionStatisticsImpl>(static_cast<double>(5001), kNss);
    CBRForNoMPResultsStrategyMock strategy;
    auto status = strategy.rankPlans(*cq,
                                     params,
                                     PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                     colls,
                                     operationContext(),
                                     std::move(plannerData));
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    ASSERT_EQ(status.getValue().rejectedPlans.size(), 1);
    ASSERT_EQ(status.getValue().estimates.size(), 4);  // 2 x (IXSCAN + FETCH)
    ASSERT_TRUE(strategy.getMultiPlanner().has_value());
    auto stats = strategy.getMultiPlanner()->getSpecificStats();
    ASSERT_FALSE(stats->earlyExit);
    ASSERT_EQ(stats->numResultsFound, 0);
    ASSERT_EQ(stats->numCandidatePlans, 2);
    ASSERT_EQ(stats->totalWorks, 10000);
}
}  // namespace
}  // namespace mongo
