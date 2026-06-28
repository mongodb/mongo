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

#include "mongo/db/query/plan_ranking/cost_based_plan_ranking.h"

#include "mongo/db/query/compiler/stats/collection_statistics_impl.h"
#include "mongo/db/query/plan_ranking/plan_ranking_test_fixture.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const NamespaceString kNss =
    NamespaceString::createNamespaceString_forTest("test.cost_based_plan_ranking");

class CostBasedPlanRankingTest : public plan_ranking::PlanRankingTestFixture {
public:
    CostBasedPlanRankingTest() : PlanRankingTestFixture(kNss) {}

    // Build planner params with collection stats pre-populated. Pass includeCollStats=true for
    // tests that expect CBR to be invoked, since CBR requires sampling statistics.
    std::shared_ptr<QueryPlannerParams> makePlannerParams(long long numRecords,
                                                          bool includeCollStats = false) {
        auto res = std::make_shared<QueryPlannerParams>(QueryPlannerParams::ArgsForTest{});
        res->mainCollectionInfo.indexes = indices;
        res->mainCollectionInfo.stats.noOfRecords = numRecords;
        if (includeCollStats) {
            res->mainCollectionInfo.collStats = std::make_unique<stats::CollectionStatisticsImpl>(
                static_cast<double>(numRecords), kNss);
        }
        return res;
    }
};

// When QueryPlanner produces a single candidate, it is returned immediately without running
// the estimation trial or calling estimateAllPlans().
TEST_F(CostBasedPlanRankingTest, SingleSolutionReturnedWithoutEstimation) {
    insertNDocuments(10);
    auto colls = getCollsAccessor();
    // No indexes created → QueryPlanner generates only a collection scan (single candidate).
    auto [cq, plannerData] = createCQAndPlannerData(colls, BSON("a" << 42));
    plannerData.plannerParams = makePlannerParams(10);

    plan_ranking::CostBasedPlanRankingStrategy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    // The single-solution shortcut does not set maybeExplainData.
    ASSERT_FALSE(status.getValue().maybeExplainData.has_value());
}

// When a candidate plan reaches EOF or fills the result batch during the short estimation
// trial, the strategy picks the MP winner immediately (earlyExit path, decision 1).
TEST_F(CostBasedPlanRankingTest, EarlyExitDuringEstimationTrialChoosesMP) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    // 10-doc collection: every plan reaches EOF well within the estimation-trial budget
    // (numWorksPerPlanEst = 384 by default).
    insertNDocuments(10);
    auto colls = getCollsAccessor();
    auto [cq, plannerData] = createCQAndPlannerData(colls, BSON("a" << GT << 0 << "b" << GT << 0));
    plannerData.plannerParams = makePlannerParams(10);

    plan_ranking::CostBasedPlanRankingStrategy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    // getBestMPPlan() does not populate maybeExplainData.
    // TODO: fix this and following tests choosing MP when explain is available.
    ASSERT_FALSE(status.getValue().maybeExplainData.has_value());
}

// When the collection is nearly exhausted after the estimation trial, the remaining work
// to EOF is small, so remMPCost < cbrCost and MP is the cheaper choice.
//
// Setup: 500-doc collection, query that never matches (productivity = 0).
//   effectiveNumWorksPerPlanMP = 500, numWorksPerPlanEst = 384
//   remNumWorks = maxRemNumWorks = 500 - 384 = 116
//   maxAchievableImprovementRatio = remMPCost / cbrCost  <  2.0
//   →  MP chosen (decision 3)
TEST_F(CostBasedPlanRankingTest, SmallCollZeroProductivityNearEOFChoosesMP) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");

    constexpr int kNumDocs = 500;
    insertNDocuments(kNumDocs);
    auto colls = getCollsAccessor();

    // 'c' is always >0 so c == -1 never matches → productivity = 0.
    auto [cq, plannerData] =
        createCQAndPlannerData(colls, BSON("a" << GT << 0 << "b" << GT << 0 << "c" << -1));
    plannerData.plannerParams = makePlannerParams(kNumDocs);

    plan_ranking::CostBasedPlanRankingStrategy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    // MP is chosen (reaching EOF is cheaper than CBR); getBestMPPlan does not populate
    // maybeExplainData whereas CBR always does.
    ASSERT_FALSE(status.getValue().maybeExplainData.has_value());
}

// With zero-result plans, bestPlanProductivity is 0, so remNumWorks = maxRemNumWorks.
// For a sufficiently large collection the remaining work to EOF is substantial, making
// maxAchievableImprovementRatio >> minRequiredImprovementRatio (2.0) and CBR is chosen.
//
//   effectiveNumWorksPerPlanMP = 2000, numWorksPerPlanEst = 384
//   remNumWorks = maxRemNumWorks = 2000 - 384 = 1616
//   remMPCost = 1616 × (C_total/384)  >>  cbrCost  →  CBR chosen (decision 4)
TEST_F(CostBasedPlanRankingTest, SmallCollZeroProductivityChoosesCBR) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(2000);
    auto colls = getCollsAccessor();
    // c is always > 0, so c == -1 never matches.
    auto [cq, plannerData] =
        createCQAndPlannerData(colls, BSON("a" << GT << 0 << "b" << GT << 0 << "c" << -1));
    plannerData.plannerParams = makePlannerParams(2000, /*includeCollStats=*/true);

    plan_ranking::CostBasedPlanRankingStrategy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    // CBR is chosen; it always populates estimates.
    ASSERT_TRUE(status.getValue().maybeExplainData.has_value());
    ASSERT_GT(status.getValue().maybeExplainData->estimates.size(), 0u);
}

// Similar to the test above for low productivity query.
//
// Setup: 2000-doc collection where every 100th document has d == 1 (~20 matching, 1%
// selectivity). In the 384-work estimation trial each plan returns ~3 results:
//   productivity ≈ 3/384 ≈ 0.0078
//   remNumWorks = min(98/0.0078, 1616) = 1616  (capped by maxRemNumWorks)
//   maxAchievableImprovementRatio = remMPCost / cbrCost  >>  2.0  →  CBR (decision 4)
TEST_F(CostBasedPlanRankingTest, SmallCollectionLowProductivityChoosesCBR) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");

    constexpr int kNumDocs = 2000;
    std::vector<BSONObj> docs;
    for (int i = 0; i < kNumDocs; i++) {
        // Every 100th document has d == 1 (~20 matching docs total, 1% selectivity).
        docs.push_back(BSON("_id" << i << "a" << i << "b" << i << "d" << (i % 100 == 0 ? 1 : 0)));
    }
    insertDocuments(docs);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] =
        createCQAndPlannerData(colls, BSON("a" << GT << 0 << "b" << GT << 0 << "d" << 1));
    plannerData.plannerParams = makePlannerParams(kNumDocs, /*includeCollStats=*/true);

    plan_ranking::CostBasedPlanRankingStrategy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    // CBR is chosen: remNumWorks is capped at maxRemNumWorks (1616), making remMPCost >> cbrCost.
    ASSERT_TRUE(status.getValue().maybeExplainData.has_value());
    ASSERT_GT(status.getValue().maybeExplainData->estimates.size(), 0u);
}

// For a large collection (collSize > numWorksPerPlanMP = 10 000), effectiveNumWorksPerPlanMP
// is NOT bounded by the collection size and stays at numWorksPerPlanMP. The remaining multiplanner
// work to EOF is therefore larger than the CBR cost, so CBR is chosen.
//
//   collSize = 10001  >  numWorksPerPlanMP = 10000
//   effectiveNumWorksPerPlanMP = numWorksPerPlanMP = 10000  (no cap applied)
//   remNumWorks = maxRemNumWorks = 10000 - 384 = 9616
//   remMPCost = 9616 × (C_total/384)  >>  cbrCost  →  CBR chosen (decision 4)
TEST_F(CostBasedPlanRankingTest, LargeCollZeroProductivityChoosesCBR) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(10001);
    auto colls = getCollsAccessor();
    // c is always > 0, so c == -1 never matches.
    auto [cq, plannerData] =
        createCQAndPlannerData(colls, BSON("a" << GT << 0 << "b" << GT << 0 << "c" << -1));
    plannerData.plannerParams = makePlannerParams(10001, /*includeCollStats=*/true);

    plan_ranking::CostBasedPlanRankingStrategy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    // CBR is chosen; it always populates estimates.
    ASSERT_TRUE(status.getValue().maybeExplainData.has_value());
    ASSERT_GT(status.getValue().maybeExplainData->estimates.size(), 0u);
}

// For a large collection (collSize > numWorksPerPlanMP = 10 000), effectiveNumWorksPerPlanMP
// stays at 10 000 (no cap). With 20% selectivity the best plan returns ~76 results in the
// 384-work trial, leaving only 25 docs to fill the batch. remNumWorks is small and
// remMPCost << cbrCost and MP wins.
//
//   effectiveNumWorksPerPlanMP = 10000  (10001 > numWorksPerPlanMP, no cap)
//   After 384-work trial: bestPlanNumResults ≈ 76, productivity ≈ 76/384 ≈ 0.198
//   numDocsRem = 101 - 76 = 25
//   remNumWorks = min(25 / 0.198, 9616) = 126  (batch-bound, not EOF-bound)
//   maxAchievableImprovementRatio = remMPCost / cbrCost  <  2.0
//   →  MP chosen (decision 3)
TEST_F(CostBasedPlanRankingTest, LargeCollProductivityNearlyFullBatchChoosesMP) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");

    constexpr int kNumDocs = 10001;
    std::vector<BSONObj> docs;
    for (int i = 0; i < kNumDocs; i++) {
        // Every 5th document has d == 1 (~20% selectivity, ~2000 matching docs total).
        docs.push_back(BSON("_id" << i << "a" << i << "b" << i << "d" << (i % 5 == 0 ? 1 : 0)));
    }
    insertDocuments(docs);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] =
        createCQAndPlannerData(colls, BSON("a" << GT << 0 << "b" << GT << 0 << "d" << 1));
    plannerData.plannerParams = makePlannerParams(kNumDocs);

    plan_ranking::CostBasedPlanRankingStrategy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    // MP is chosen: remNumWorks (≈126) is batch-bound and
    // remMPCost << cbrCost. getBestMPPlan does not populate maybeExplainData.
    ASSERT_FALSE(status.getValue().maybeExplainData.has_value());
}

}  // namespace
}  // namespace mongo
