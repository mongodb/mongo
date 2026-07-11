// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/query/plan_ranking/cost_based_plan_ranking.h"

#include "mongo/db/curop.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/compiler/stats/collection_statistics_impl.h"
#include "mongo/db/query/plan_ranking/plan_ranker.h"
#include "mongo/db/query/plan_ranking/plan_ranker_method.h"
#include "mongo/db/query/plan_ranking/plan_ranking_test_fixture.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.collection");

class CostBasedPlanRankingTest : public plan_ranking::PlanRankingTestFixture {
public:
    CostBasedPlanRankingTest() : PlanRankingTestFixture(kNss) {}

    std::shared_ptr<QueryPlannerParams> makePlannerParams(std::vector<IndexEntry> idx,
                                                          double numRecords) {
        auto res = std::make_shared<QueryPlannerParams>(QueryPlannerParams::ArgsForTest{});
        res->mainCollectionInfo.indexes = std::move(idx);
        res->mainCollectionInfo.collStats =
            std::make_unique<stats::CollectionStatisticsImpl>(numRecords, kNss);
        return res;
    }
};

StatusWith<PlanRankingResult> planAndRank(plan_ranking::PlanRankingStrategy& strategy,
                                          PlannerData& plannerData) {
    auto& query = *plannerData.cq;
    auto topLevelSampleFieldNames =
        ce::extractTopLevelFieldsFromMatchExpression(query.getPrimaryMatchExpression());
    bool hasRelevantMultikeyIndex = false;
    auto statusWithMultiPlanSolns =
        QueryPlanner::plan(query,
                           *plannerData.plannerParams,
                           topLevelSampleFieldNames,
                           boost::optional<bool&>(hasRelevantMultikeyIndex));
    if (!statusWithMultiPlanSolns.isOK()) {
        return statusWithMultiPlanSolns.getStatus();
    }

    plan_ranking::RankingContext rctx{.solutions = std::move(statusWithMultiPlanSolns.getValue()),
                                      .topLevelSampleFieldNames =
                                          std::move(topLevelSampleFieldNames),
                                      .hasRelevantMultikeyIndex = hasRelevantMultikeyIndex};
    return strategy.rankPlans(plannerData, rctx);
}

// Regression test for SERVER-130766. internalQueryPlanEvaluationMaxResults == 1 is the smallest
// value the validator now allows. For a no-result, multi-plan query the estimation trial runs to
// completion (no plan advances, and the collection is large enough that the capped trial does not
// reach EOF), so the strategy reaches tassert 11306805 and passes it because numResultsMP (1) >
// bestPlanNumResults (0). The very low productivity then makes it choose CBR.
TEST_F(CostBasedPlanRankingTest, MaxResultsOfOneDoesNotTripFullBatchAssertion) {
    unittest::ServerParameterGuard maxResults("internalQueryPlanEvaluationMaxResults", 1);

    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(5001);  // {_id:i, a:i, b:i, c:i}
    auto colls = getCollsAccessor();

    // No document has c == -1, so no candidate plan advances during the trial.
    auto [cq, plannerData] =
        createCQAndPlannerData(colls, BSON("a" << GT << 0 << "b" << GT << 0 << "c" << -1));
    plannerData.plannerParams = makePlannerParams(indices, 5001);

    plan_ranking::CostBasedPlanRankingStrategy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    ASSERT_EQ(CurOp::get(operationContext())->debug().planRankerMethod,
              PlanRankerMethod::kCostBasedRanker);
}

TEST_F(CostBasedPlanRankingTest, MaxResultsOfOneEarlyExitsWhenBatchFilled) {
    unittest::ServerParameterGuard maxResults("internalQueryPlanEvaluationMaxResults", 1);

    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(5001);  // {_id:i, a:i, b:i, c:i}
    auto colls = getCollsAccessor();

    // Matches i in [1, 5000], so a candidate plan advances to 1 result (== maxResults), filling a
    // full batch and triggering early-exit.
    auto [cq, plannerData] = createCQAndPlannerData(colls, BSON("a" << GT << 0 << "b" << GT << 0));
    plannerData.plannerParams = makePlannerParams(indices, 5001);

    plan_ranking::CostBasedPlanRankingStrategy strategy;
    auto status = planAndRank(strategy, plannerData);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    ASSERT_EQ(CurOp::get(operationContext())->debug().planRankerMethod,
              PlanRankerMethod::kMultiPlanner);
}

}  // namespace
}  // namespace mongo
