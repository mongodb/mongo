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

class CBRForNoMPResultsStrategySpy : public plan_ranking::CBRForNoMPResultsStrategy {
public:
    boost::optional<classic_runtime_planner::MultiPlanner>& getMultiPlanner() {
        return _multiPlanner;
    }
};

class CBRForNoMPResultsTest : public plan_ranking::PlanRankingTestFixture {
public:
    CBRForNoMPResultsTest() : PlanRankingTestFixture(kNss) {}

    struct ParamsHelper {
        std::vector<IndexEntry> indices = {};
        std::unique_ptr<stats::CollectionStatistics> collStats = nullptr;
        size_t options = 0;
    };

    auto makePlannerParams(ParamsHelper params) {
        auto res = std::make_shared<QueryPlannerParams>(QueryPlannerParams::ArgsForTest{});
        res->mainCollectionInfo.indexes = params.indices;
        res->mainCollectionInfo.collStats = std::move(params.collStats);
        res->mainCollectionInfo.options = params.options;
        return res;
    }
};

StatusWith<PlanRankingResult> planAndRank(plan_ranking::PlanRankingStrategy& strategy,
                                          PlannerData& plannerData) {
    return strategy.rankPlans(plannerData);
}

TEST_F(CBRForNoMPResultsTest, SingleSolutionDoesNotUseMultiPlanner) {
    insertNDocuments(10);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] = createCQAndPlannerData(colls, BSON("a" << 42 << "b" << 7));

    plannerData.plannerParams = makePlannerParams({.indices = indices});
    CBRForNoMPResultsStrategySpy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    ASSERT_FALSE(status.getValue().maybeExplainData.has_value());
    ASSERT_EQ(strategy.getMultiPlanner(), boost::none);
    ASSERT_EQ(status.getValue().needsWorksMeasured, false);

    ASSERT_FALSE(status.getValue().execState);
}

TEST_F(CBRForNoMPResultsTest, QueryPlannerFailsReturnsError) {
    auto colls = getCollsAccessor();
    // Create a query that won't have any index plans since we haven't created any indexes.
    auto [cq, plannerData] = createCQAndPlannerData(colls, BSON("a" << 42));

    // Set options to forbid table scan. Since there are no available indexes, planning will fail.
    plannerData.plannerParams =
        makePlannerParams({.indices = indices, .options = QueryPlannerParams::NO_TABLE_SCAN});

    CBRForNoMPResultsStrategySpy strategy;
    auto status = planAndRank(strategy, plannerData);

    ASSERT_NOT_OK(status.getStatus());
    ASSERT_EQ(status.getStatus().code(), ErrorCodes::NoQueryExecutionPlans);
}

TEST_F(CBRForNoMPResultsTest, EOFMultiPlannerMakesADecisionWithoutCBR) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(10);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] = createCQAndPlannerData(colls, BSON("a" << 4 << "b" << 4));

    plannerData.plannerParams = makePlannerParams({.indices = indices});
    CBRForNoMPResultsStrategySpy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    ASSERT_TRUE(status.getValue().maybeExplainData.has_value());
    auto& explainData = status.getValue().maybeExplainData.value();
    ASSERT_EQ(explainData.rejectedPlansWithStages.size(), 1);
    ASSERT_EQ(explainData.estimates.size(), 0);
    ASSERT_FALSE(explainData.rejectedPlansWithStages[0].solution == nullptr);
    auto rejectedPlanStats = explainData.rejectedPlansWithStages[0].planStage->getStats();
    ASSERT_EQ(rejectedPlanStats->common.works, 2);
    ASSERT_EQ(rejectedPlanStats->common.advanced, 1);
    ASSERT_TRUE(strategy.getMultiPlanner().has_value());
    auto stats = strategy.getMultiPlanner()->getSpecificStats();
    ASSERT_TRUE(stats->earlyExit);
    ASSERT_EQ(stats->numResultsFound, 2);  // One result per plan
    ASSERT_EQ(stats->numCandidatePlans, 2);
    ASSERT_EQ(stats->totalWorks, 4);  // 2 works per plan (seek + advance)
    ASSERT_EQ(status.getValue().needsWorksMeasured, false);

    ASSERT_TRUE(status.getValue().execState);
}

TEST_F(CBRForNoMPResultsTest, BatchFilledMultiPlannerMakesADecisionWithoutCBR) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(200);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] =
        createCQAndPlannerData(colls, BSON("a" << GT << 1 << "b" << LT << 200));

    plannerData.plannerParams = makePlannerParams({.indices = indices});
    CBRForNoMPResultsStrategySpy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    auto& explainData = status.getValue().maybeExplainData.value();
    ASSERT_EQ(explainData.rejectedPlansWithStages.size(), 1);
    ASSERT_EQ(explainData.estimates.size(), 0);
    ASSERT_EQ(explainData.rejectedPlansWithStages.size(), 1);
    ASSERT_FALSE(explainData.rejectedPlansWithStages[0].solution == nullptr);
    auto rejectedPlanStats = explainData.rejectedPlansWithStages[0].planStage->getStats();
    ASSERT_EQ(rejectedPlanStats->common.works, 101);
    ASSERT_EQ(rejectedPlanStats->common.advanced, 99);
    ASSERT_TRUE(strategy.getMultiPlanner().has_value());
    auto stats = strategy.getMultiPlanner()->getSpecificStats();
    ASSERT_TRUE(stats->earlyExit);
    ASSERT_EQ(stats->numResultsFound, 200);  // Batch filled during trials
    ASSERT_EQ(stats->numCandidatePlans, 2);
    ASSERT_EQ(stats->totalWorks, 202);  // 2 * 101 works in each plan
    ASSERT_EQ(status.getValue().needsWorksMeasured, false);

    ASSERT_TRUE(status.getValue().execState);
}

TEST_F(CBRForNoMPResultsTest, LittleResultsMultiPlannerMakesADecisionWithoutCBR) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(10001);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] =
        createCQAndPlannerData(colls, BSON("a" << GT << 0 << "b" << GT << 0 << "c" << 10));

    plannerData.plannerParams = makePlannerParams({.indices = indices});
    CBRForNoMPResultsStrategySpy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    auto& explainData = status.getValue().maybeExplainData.value();
    ASSERT_EQ(explainData.estimates.size(), 0);
    ASSERT_EQ(explainData.rejectedPlansWithStages.size(), 1);
    ASSERT_FALSE(explainData.rejectedPlansWithStages[0].solution == nullptr);
    auto rejectedPlanStats = explainData.rejectedPlansWithStages[0].planStage->getStats();
    ASSERT_EQ(rejectedPlanStats->common.works, 10000);
    ASSERT_EQ(rejectedPlanStats->common.advanced, 1);
    ASSERT_TRUE(strategy.getMultiPlanner().has_value());
    auto stats = strategy.getMultiPlanner()->getSpecificStats();
    ASSERT_FALSE(stats->earlyExit);
    ASSERT_EQ(stats->numResultsFound, 2);  // One per plan
    ASSERT_EQ(stats->numCandidatePlans, 2);
    ASSERT_EQ(stats->totalWorks, 20000);  // Each plan exhausted all credits (10k)
    ASSERT_EQ(status.getValue().needsWorksMeasured, false);

    ASSERT_TRUE(status.getValue().execState);
}

TEST_F(CBRForNoMPResultsTest, NoResultsMultiPlannerUsesCBR) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(5001);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] =
        createCQAndPlannerData(colls, BSON("a" << GT << 0 << "b" << GT << 0 << "c" << -1));

    plannerData.plannerParams =
        makePlannerParams({.indices = indices,
                           .collStats = std::make_unique<stats::CollectionStatisticsImpl>(
                               static_cast<double>(5001), kNss)});

    CBRForNoMPResultsStrategySpy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    auto& explainData = status.getValue().maybeExplainData.value();
    ASSERT_EQ(explainData.rejectedPlansWithStages.size(), 3);  // 2 from MP + 1 from CBR
    ASSERT_EQ(explainData.estimates.size(), 4);                // 2 x (IXSCAN + FETCH)
    int numCBRRejectedPlans = 0;
    for (const auto& rejectedPlan : explainData.rejectedPlansWithStages) {
        ASSERT_FALSE(rejectedPlan.solution == nullptr);
        if (rejectedPlan.planStage) {  // It is rejected by multi-planner
            ASSERT_EQ(rejectedPlan.planStage->getStats()->common.works, 5000);
            ASSERT_EQ(rejectedPlan.planStage->getStats()->common.advanced, 0);
        } else {
            numCBRRejectedPlans++;
        }
    }
    ASSERT_EQ(numCBRRejectedPlans, 1);
    ASSERT_TRUE(strategy.getMultiPlanner().has_value());
    auto stats = strategy.getMultiPlanner()->getSpecificStats();
    ASSERT_FALSE(stats->earlyExit);
    ASSERT_EQ(stats->numResultsFound, 0);
    ASSERT_EQ(stats->numCandidatePlans, 2);
    ASSERT_EQ(stats->totalWorks, 10000);
    ASSERT_EQ(status.getValue().needsWorksMeasured, true);

    ASSERT_FALSE(status.getValue().execState);
}

TEST_F(CBRForNoMPResultsTest, CBRCannotDecideUsesMultiPlanner) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(10001);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] =
        createCQAndPlannerData(colls,
                               BSON("a" << GT << 0 << "b" << GT << 0 << "c" << -1),
                               [](FindCommandRequest& findCmd) { findCmd.setReturnKey(true); });

    plannerData.plannerParams =
        makePlannerParams({.indices = indices,
                           .collStats = std::make_unique<stats::CollectionStatisticsImpl>(
                               static_cast<double>(5001), kNss)});

    CBRForNoMPResultsStrategySpy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    auto& explainData = status.getValue().maybeExplainData.value();
    ASSERT_EQ(explainData.rejectedPlansWithStages.size(), 3);  // 1 from multiplanner + 2 from CBR
    ASSERT_FALSE(explainData.rejectedPlansWithStages[0].solution == nullptr);
    auto multiPlannerRejectedPlanStats =
        explainData.rejectedPlansWithStages[0].planStage->getStats();
    ASSERT_EQ(multiPlannerRejectedPlanStats->common.works, 10000);
    ASSERT_EQ(multiPlannerRejectedPlanStats->common.advanced, 0);
    ASSERT_EQ(explainData.estimates.size(),
              0);  // Empty estimates map as setReturnKey cannot be estimated
    ASSERT_FALSE(explainData.rejectedPlansWithStages[1].solution == nullptr);
    ASSERT_TRUE(explainData.rejectedPlansWithStages[1].planStage ==
                nullptr);  // No plan stage as CBR rejected
    ASSERT_FALSE(explainData.rejectedPlansWithStages[2].solution == nullptr);
    ASSERT_TRUE(explainData.rejectedPlansWithStages[2].planStage ==
                nullptr);  // No plan stage as CBR rejected
    ASSERT_TRUE(strategy.getMultiPlanner().has_value());
    auto stats = strategy.getMultiPlanner()->getSpecificStats();
    ASSERT_FALSE(stats->earlyExit);
    ASSERT_EQ(stats->numResultsFound, 0);
    ASSERT_EQ(stats->numCandidatePlans, 2);
    ASSERT_EQ(stats->totalWorks, 20000);

    ASSERT_TRUE(status.getValue().execState);
}

TEST_F(CBRForNoMPResultsTest, MPPicksBlockingSortAndEOFs) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(10);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] =
        createCQAndPlannerData(colls, BSON("b" << LT << 0), [](FindCommandRequest& findCmd) {
            findCmd.setSort(BSON("a" << 1));
        });

    plannerData.plannerParams =
        makePlannerParams({.indices = indices,
                           .collStats = std::make_unique<stats::CollectionStatisticsImpl>(
                               static_cast<double>(5001), kNss)});
    CBRForNoMPResultsStrategySpy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    auto& explainData = status.getValue().maybeExplainData.value();
    // No rejected plans since the winner includes a blocking sort hence the other is backup.
    ASSERT_EQ(explainData.rejectedPlansWithStages.size(), 0);
    ASSERT_TRUE(strategy.getMultiPlanner().has_value());
    auto stats = strategy.getMultiPlanner()->getSpecificStats();
    ASSERT_TRUE(stats->earlyExit);
    ASSERT_EQ(stats->numResultsFound, 0);
    ASSERT_EQ(stats->numCandidatePlans, 2);
    ASSERT_EQ(stats->totalWorks, 4);

    ASSERT_TRUE(status.getValue().execState);
}
}  // namespace
}  // namespace mongo
