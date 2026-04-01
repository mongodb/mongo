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
    ASSERT_EQ(status.getValue().needsWorksMeasuredForPlanCache, false);

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
    ASSERT_EQ(status.getValue().needsWorksMeasuredForPlanCache, false);

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
    ASSERT_EQ(status.getValue().needsWorksMeasuredForPlanCache, false);

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
    ASSERT_EQ(status.getValue().needsWorksMeasuredForPlanCache, false);

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
    // The plan rejected from CBR is in rejectedPlansWithStages
    ASSERT_EQ(explainData.rejectedPlansWithStages.size(), 1);
    ASSERT_EQ(explainData.estimates.size(), 4);  // 2 x (IXSCAN + FETCH)
    for (const auto& rejectedPlan : explainData.rejectedPlansWithStages) {
        ASSERT_FALSE(rejectedPlan.solution == nullptr);
        ASSERT_EQ(rejectedPlan.planStage, nullptr);  // Not rejected by the multi-planner
    }
    ASSERT_TRUE(strategy.getMultiPlanner().has_value());
    auto stats = strategy.getMultiPlanner()->getSpecificStats();
    ASSERT_TRUE(stats->earlyExit);
    ASSERT_EQ(stats->numResultsFound, 0);
    ASSERT_EQ(stats->numCandidatePlans, 2);
    ASSERT_EQ(stats->totalWorks, 10001);
    ASSERT_EQ(status.getValue().needsWorksMeasuredForPlanCache, true);

    ASSERT_TRUE(status.getValue().execState);
    auto mp = dynamic_cast<MultiPlanStage*>(
        status.getValue().execState->peekExecState<ClassicExecState>()->root.get());
    ASSERT_TRUE(mp);
    ASSERT_EQ(mp->getStats()->children.size(), 2);  // One winning and one rejected plan
}

TEST_F(CBRForNoMPResultsTest, CBRCannotDecideUsesMultiPlanner) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    createIndexOnEmptyCollection(operationContext(),
                                 BSON("c" << 1),
                                 "c_1",
                                 BSON("partialFilterExpression" << BSON("b" << GTE << 0)));

    // Insert documents into the collection
    // Note that this makes b_1 the CBR loser
    {
        std::vector<BSONObj> docs;
        for (int i = 0; i < 1000; i++) {
            BSONObj obj = BSON("_id" << i << "b" << i);
            docs.push_back(obj);
        }
        for (int i = 0; i < 10001; i++) {
            BSONObj obj = BSON("_id" << i + 1000 << "a" << i << "b" << i << "c" << i);
            docs.push_back(obj);
        }
        insertDocuments(docs);
    }


    auto colls = getCollsAccessor();

    // The query matches no documents so none of the plans will advance
    // during the initial MP run, so the strategy will fall back to CBR.
    // It will cost a_1 and b_1, and a_1 will win (because of the sequential sample).
    // a_1 and c_1 will be multiplanned. Both will exit with neither EOF nor documents
    // after 10000 works.
    auto [cq, plannerData] = createCQAndPlannerData(
        colls, BSON("a" << GT << 0 << "b" << GT << 0 << "c" << GT << 0 << "d" << 0));

    plannerData.plannerParams =
        makePlannerParams({.indices = indices,
                           .collStats = std::make_unique<stats::CollectionStatisticsImpl>(
                               static_cast<double>(5001), kNss)});

    CBRForNoMPResultsStrategySpy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    auto& explainData = status.getValue().maybeExplainData.value();
    ASSERT_EQ(explainData.rejectedPlansWithStages.size(),
              5);  // 2 from multiplanner + 3 from CBR (we mark all plans considered by CBR as
                   // rejected if it cannot decide)
    // The winning plan must have made it all the way to the end of the collection
    ASSERT_EQ(explainData.multiPlannerWinningPlanTrialStats->common.works, 10000);
    ASSERT_EQ(explainData.multiPlannerWinningPlanTrialStats->common.advanced, 0);
    // There must be two (ixscan + fetch) CBR estimates for the two costable plans.
    ASSERT_EQ(explainData.estimates.size(), 4);

    // CBR winner should make it into the second runTrials
    ASSERT_EQ(explainData.rejectedPlansWithStages[0].planStage->getStats()->common.works, 10000);
    ASSERT_EQ(explainData.rejectedPlansWithStages[0].planStage->getStats()->common.advanced, 0);
    // CBR loser should get abandoned
    ASSERT_EQ(explainData.rejectedPlansWithStages[1].planStage->getStats()->common.works, 3333);
    ASSERT_EQ(explainData.rejectedPlansWithStages[1].planStage->getStats()->common.advanced, 0);
    const std::string cbrLoserPlanString =
        explainData.rejectedPlansWithStages[1].solution->toString();
    // Make sure this candidate actually was the CBR loser
    ASSERT_NE(cbrLoserPlanString.find("b_1"), std::string::npos);
    // The rest should be CBR rejections
    ASSERT(!explainData.rejectedPlansWithStages[2].planStage);
    ASSERT(!explainData.rejectedPlansWithStages[3].planStage);
    ASSERT(!explainData.rejectedPlansWithStages[4].planStage);

    ASSERT_TRUE(strategy.getMultiPlanner().has_value());
    auto stats = strategy.getMultiPlanner()->getSpecificStats();
    ASSERT_FALSE(stats->earlyExit);
    ASSERT_EQ(stats->numResultsFound, 0);
    ASSERT_EQ(stats->numCandidatePlans, 3);
    ASSERT_EQ(stats->totalWorks, 23333);  // 10000 + 10000 + 3333

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

TEST_F(CBRForNoMPResultsTest, StrategyDoesNotCollectExplainData) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(10);
    auto colls = getCollsAccessor();

    auto [cq, plannerData] = createCQAndPlannerData(colls, BSON("a" << 4 << "b" << 4));
    cq->getExpCtx()->setExplain(boost::none);

    plannerData.plannerParams = makePlannerParams({.indices = indices});
    CBRForNoMPResultsStrategySpy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    ASSERT_FALSE(status.getValue().maybeExplainData.has_value());
    ASSERT_EQ(status.getValue().needsWorksMeasuredForPlanCache, false);
    ASSERT_TRUE(status.getValue().execState);
    ASSERT_TRUE(strategy.getMultiPlanner().has_value());
}
}  // namespace
}  // namespace mongo
