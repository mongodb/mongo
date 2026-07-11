// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/exec/runtime_planners/planner_interface.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_ranking/plan_ranker.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace plan_ranking {
class CBRForNoMPResultsStrategy : public PlanRankingStrategy {
public:
    StatusWith<PlanRankingResult> rankPlans(PlannerData& pd, RankingContext& rctx) override;

protected:
    // TODO SERVER-115496. Once solutions are received as argument, this no longer needs to be
    // optional.
    boost::optional<classic_runtime_planner::MultiPlanner> _multiPlanner;

private:
    /**
     * Resumes the multi-planner and picks the best plan from it.
     * @param trialsConfig The trials configuration to use when resuming the multi-planner.
     * @param isExplain If true extract explain data from the multiplanner.
     */
    StatusWith<PlanRankingResult> resumeMultiPlannerAndPickBestPlan(
        OperationContext* opCtx,
        const trial_period::TrialPhaseConfig& trialsConfig,
        bool isExplain);
};
}  // namespace plan_ranking
}  // namespace mongo
