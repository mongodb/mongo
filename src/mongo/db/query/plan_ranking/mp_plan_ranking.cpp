// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_ranking/mp_plan_ranking.h"

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_ranking/plan_selection_strategy.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"

namespace mongo::plan_ranking {

StatusWith<PlanRankingResult> MPPlanRankingStrategy::rankPlans(PlannerData& pd,
                                                               RankingContext& rctx) {
    /**
     * This is a special plan ranking strategy in that it does not actually rank plans, but
     * rather returns all enumerated plans. This will result in multi-planning being used
     * to select a winning plan at runtime.
     */
    const auto strategy = rctx.solutions.size() > 1 ? PlanSelectionStrategy::kMultiPlanner
                                                    : PlanSelectionStrategy::kSinglePlan;
    PlanRankingResult out{.solutions = std::move(rctx.solutions),
                          .planSelectionStrategy = strategy};
    if (out.solutions.size() > 1) {
        // Multi-planning was fixed by configuration (feature flag, knob, or a construction-time
        // overwrite), not decided at planning time. On explain queries record that provenance for
        // rankerChoice.reason; this strategy otherwise carries no explain data, so the carrier is
        // created here. Non-explain queries pay nothing. The config reason can be empty: with the
        // knob at 'mixed' this strategy still runs for SBE-bound queries, because canUseCBR
        // requires the classic engine (see plan_ranker.cpp); such plans never reach V3 emission,
        // so nothing is recorded for them.
        if (pd.cq->getExplain()) {
            const auto reason = pd.plannerParams->getPlanRankerReasonFromConfig();
            if (reason.has_value()) {
                out.maybeExplainData.emplace();
                out.maybeExplainData->planRankerReason = reason;
            }
        }
    }
    return out;
}

}  // namespace mongo::plan_ranking
