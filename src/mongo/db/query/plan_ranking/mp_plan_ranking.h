// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/plan_ranking/plan_ranker.h"
#include "mongo/util/modules.h"

namespace mongo::plan_ranking {

// TODO SERVER-117118: Investigate having strategies *always* complete ranking, with the output
// being one solution or an error, not falling back to MultiPlanning.
// In that case, this strategy will explicitly multiplan, rather than return
// all solutions.
class MPPlanRankingStrategy : public PlanRankingStrategy {
public:
    StatusWith<PlanRankingResult> rankPlans(PlannerData& pd, RankingContext& rctx) override;
};

}  // namespace mongo::plan_ranking
