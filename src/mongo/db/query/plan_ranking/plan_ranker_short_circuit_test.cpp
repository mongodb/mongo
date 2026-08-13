// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/collection_statistics_impl.h"
#include "mongo/db/query/plan_ranking/plan_ranker.h"
#include "mongo/db/query/plan_ranking/plan_ranker_reason.h"
#include "mongo/db/query/plan_ranking/plan_ranking_test_fixture.h"
#include "mongo/db/query/plan_ranking/plan_selection_strategy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.plan_ranker");

// Exercises 'PlanRanker::rankPlans' directly, covering both the unified single-solution short
// circuit that decides whether a ranking strategy runs at all, and the PlanSelectionStrategy that
// the result reports. The short circuit lives entirely in 'PlanRanker::rankPlans' (see
// plan_ranker.cpp): a single solution skips ranking when CBR is unavailable, when the solution is a
// count scan, or when the query is not an explain. Otherwise the solution is ranked so that costing
// information can be displayed.
class PlanRankerTest : public plan_ranking::PlanRankingTestFixture {
public:
    PlanRankerTest() : PlanRankingTestFixture(kNss) {}

    std::shared_ptr<QueryPlannerParams> makePlannerParams(bool cbrEnabled) {
        auto res = std::make_shared<QueryPlannerParams>(QueryPlannerParams::ArgsForTest{});
        res->mainCollectionInfo.indexes = indices;
        res->planRanker =
            cbrEnabled ? QueryPlanRankerEnum::kCostBased : QueryPlanRankerEnum::kMultiPlanner;
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

    // A short circuited solution is returned untouched: no strategy ran, so there is no exec state
    // and no explain data, and the default plan strategy kSinglePlan stands.
    void assertNotRanked(const PlanRankingResult& result) {
        ASSERT_EQ(result.solutions.size(), 1);
        ASSERT_FALSE(result.execState);
        ASSERT_FALSE(result.maybeExplainData.has_value());
        ASSERT_EQ(result.planSelectionStrategy, PlanSelectionStrategy::kSinglePlan);
    }
};

// Multiple candidate solutions are never short circuited; the strategy must run. With CBR disabled
// the multiplanning strategy does not rank, it hands every solution downstream for later runtime
// multiplanning, and reports itself as the strategy that will pick the winner.
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
    ASSERT_EQ(status.getValue().planSelectionStrategy, PlanSelectionStrategy::kMultiPlanner);
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
    assertNotRanked(status.getValue());
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
    assertNotRanked(status.getValue());
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
    assertNotRanked(status.getValue());
}

// With CBR available and the query being an explain, a single non-count solution is ranked so that
// the explain output can carry costing information. Under a costing strategy (here heuristic CE),
// the lone solution is costed and the resulting estimates are surfaced in the explain data.
TEST_F(PlanRankerTest, SingleSolutionWithExplainIsCosted) {
    insertNDocuments(10);
    auto colls = getCollsAccessor();

    unittest::ServerParameterGuard planRankerGuard{"internalQueryPlanRanker", "costBased"};
    unittest::ServerParameterGuard ceModeGuard{"internalQueryCBRCEMode", "heuristicCE"};

    // The fixture sets explain to kQueryPlanner by default.
    auto [cq, plannerData] = createCQAndPlannerData(colls, BSON("a" << 42 << "b" << 7));
    plannerData.plannerParams = makePlannerParams(/* cbrEnabled */ true);
    plannerData.plannerParams->mainCollectionInfo.collStats =
        std::make_unique<stats::CollectionStatisticsImpl>(static_cast<double>(10), kNss);

    auto status = rankPlans(*cq, std::move(plannerData), /* isClassic */ true);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    // The strategy ran rather than being short circuited, so it produced costing estimates.
    ASSERT_TRUE(status.getValue().maybeExplainData.has_value());
    ASSERT_FALSE(status.getValue().maybeExplainData->estimates.empty());
    // A sole candidate must report kSinglePlan even when CBR costed it for the explain.
    ASSERT_EQ(status.getValue().planSelectionStrategy, PlanSelectionStrategy::kSinglePlan);
}

// When CBR cannot estimate every plan (here because returnKey introduces an inestimable RETURN_KEY
// node in each candidate) it returns all the solutions to be ranked downstream by the
// multi-planner. The plan strategy result must report kMultiPlanner.
TEST_F(PlanRankerTest, InestimableReturnKeyRecordsMultiPlanner) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(10);
    auto colls = getCollsAccessor();

    unittest::ServerParameterGuard planRankerGuard{"internalQueryPlanRanker", "costBased"};
    unittest::ServerParameterGuard ceModeGuard{"internalQueryCBRCEMode", "heuristicCE"};

    // 'a > 0' and 'b > 0' give two competing index scans, each wrapped in a RETURN_KEY node.
    auto [cq, plannerData] =
        createCQAndPlannerData(colls, BSON("a" << GT << 0 << "b" << GT << 0), [](auto& findCmd) {
            findCmd.setReturnKey(true);
        });
    plannerData.plannerParams = makePlannerParams(/* cbrEnabled */ true);
    plannerData.plannerParams->mainCollectionInfo.collStats =
        std::make_unique<stats::CollectionStatisticsImpl>(static_cast<double>(10), kNss);

    auto status = rankPlans(*cq, std::move(plannerData), /* isClassic */ true);
    ASSERT_OK(status.getStatus());
    // CBR could not choose a single winner, so both solutions are handed off to the multi-planner.
    ASSERT_EQ(status.getValue().solutions.size(), 2);
    ASSERT_FALSE(status.getValue().needsWorksMeasuredForPlanCache);
    ASSERT_EQ(status.getValue().planSelectionStrategy, PlanSelectionStrategy::kMultiPlanner);
    // The inestimable-node fallback is recorded on the explain data for rankerChoice.reason.
    ASSERT_TRUE(status.getValue().maybeExplainData.has_value());
    ASSERT_TRUE(status.getValue().maybeExplainData->planRankerReason.has_value());
    ASSERT_EQ(*status.getValue().maybeExplainData->planRankerReason,
              PlanRankerReason::kCBRInestimableNode);
}

// When CBR can estimate all plans and choose a single winner, the result reports itself as
// cost-based ranked - the counterpart to the inestimable fallback above.
TEST_F(PlanRankerTest, EstimablePlansRecordCostBasedRanker) {
    createIndexOnEmptyCollection(operationContext(), BSON("a" << 1), "a_1");
    createIndexOnEmptyCollection(operationContext(), BSON("b" << 1), "b_1");
    insertNDocuments(10);
    auto colls = getCollsAccessor();

    unittest::ServerParameterGuard planRankerGuard{"internalQueryPlanRanker", "costBased"};
    unittest::ServerParameterGuard ceModeGuard{"internalQueryCBRCEMode", "heuristicCE"};

    auto [cq, plannerData] = createCQAndPlannerData(colls, BSON("a" << GT << 0 << "b" << GT << 0));
    plannerData.plannerParams = makePlannerParams(/* cbrEnabled */ true);
    plannerData.plannerParams->mainCollectionInfo.collStats =
        std::make_unique<stats::CollectionStatisticsImpl>(static_cast<double>(10), kNss);

    auto status = rankPlans(*cq, std::move(plannerData), /* isClassic */ true);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    ASSERT_TRUE(status.getValue().needsWorksMeasuredForPlanCache);
    ASSERT_EQ(status.getValue().planSelectionStrategy, PlanSelectionStrategy::kCostBasedRanker);
    // Strict CBR (planRanker == kCostBased) is a configuration-fixed choice: the recorded reason
    // is the config provenance, kQueryPlanRankerKnob.
    ASSERT_TRUE(status.getValue().maybeExplainData.has_value());
    ASSERT_TRUE(status.getValue().maybeExplainData->planRankerReason.has_value());
    ASSERT_EQ(*status.getValue().maybeExplainData->planRankerReason,
              PlanRankerReason::kQueryPlanRankerKnob);
}

// A sole candidate reports kSinglePlan even when CBR cannot estimate it.
TEST_F(PlanRankerTest, CostBasedRankingReportsSinglePlanForSoleInestimableCandidate) {
    insertNDocuments(10);
    auto colls = getCollsAccessor();

    unittest::ServerParameterGuard planRankerGuard{"internalQueryPlanRanker", "costBased"};
    unittest::ServerParameterGuard ceModeGuard{"internalQueryCBRCEMode", "heuristicCE"};

    auto [cq, plannerData] = createCQAndPlannerData(
        colls, BSON("a" << GT << 0), [](auto& findCmd) { findCmd.setReturnKey(true); });
    plannerData.plannerParams = makePlannerParams(/* cbrEnabled */ true);
    plannerData.plannerParams->mainCollectionInfo.collStats =
        std::make_unique<stats::CollectionStatisticsImpl>(static_cast<double>(10), kNss);

    auto status = rankPlans(*cq, std::move(plannerData), /* isClassic */ true);
    ASSERT_OK(status.getStatus());
    ASSERT_EQ(status.getValue().solutions.size(), 1);
    ASSERT_EQ(status.getValue().planSelectionStrategy, PlanSelectionStrategy::kSinglePlan);
}

}  // namespace
}  // namespace mongo
