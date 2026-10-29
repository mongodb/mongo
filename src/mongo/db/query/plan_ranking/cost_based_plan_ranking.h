// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/exec/runtime_planners/planner_interface.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_ranking/cbr_plan_ranking.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/util/modules.h"

namespace mongo::plan_ranking {

// SERVER-118020: Investigate a more distinctive name to contrast with CBRPlanRankingStrategy
class CostBasedPlanRankingStrategy : public PlanRankingStrategy {
public:
    /**
     * Choose a plan ranking approach - either multi-planning (MP) or CBR for the plans of 'query'
     * based on a cost model of MP and CBR.
     * The main idea is to run multi-planning (MP) for a small number of works sufficient to
     * generate some execution statistics. This statistics is then used to
     * (a) estimate the cost of MP, and
     * (b) extrapolate the remaining number of works needed to fill a batch.
     * Additionally, we estimate the cost of SamplingCE (cost of sample generation + cost of
     * estimating all plans).
     * The two costs are compared wrt the amount of work needed to find the best plan in addition
     * to the work already done by MP. CBR is chosen if it is better than the remaining MP work by
     * some factor (default 2.0 times).
     *
     * Returns the best plan or error.
     */
    StatusWith<PlanRankingResult> rankPlans(PlannerData& pd, RankingContext& rctx) override;
};

}  // namespace mongo::plan_ranking

