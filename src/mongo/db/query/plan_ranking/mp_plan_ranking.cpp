// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_ranking/mp_plan_ranking.h"

#include "mongo/db/curop.h"
#include "mongo/db/query/plan_ranking/plan_ranker_method.h"
#include "mongo/db/query/query_planner.h"

namespace mongo::plan_ranking {

StatusWith<PlanRankingResult> MPPlanRankingStrategy::rankPlans(PlannerData& pd,
                                                               RankingContext& rctx) {
    /**
     * This is a special plan ranking strategy in that it does not actually rank plans, but
     * rather returns all enumerated plans. This will result in multi-planning being used
     * to select a winning plan at runtime.
     */
    if (rctx.solutions.size() > 1) {
        CurOp::get(pd.opCtx)->debug().planRankerMethod = PlanRankerMethod::kMultiPlanner;
    }
    return PlanRankingResult{.solutions = std::move(rctx.solutions)};
}

}  // namespace mongo::plan_ranking
