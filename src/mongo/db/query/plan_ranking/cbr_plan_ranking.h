// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_ranking/plan_ranker.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace plan_ranking {

// SERVER-118020: Investigate a more distinctive name to contrast with CostBasedPlanRankingStrategy
class CBRPlanRankingStrategy : public PlanRankingStrategy {
public:
    StatusWith<PlanRankingResult> rankPlans(PlannerData& pd, RankingContext& rctx) override;

    // Separate impl taking components from PlannerData retained temporarily for call from
    // getBestCBRPlan
    StatusWith<PlanRankingResult> rankPlans(OperationContext* opCtx,
                                            CanonicalQuery& query,
                                            QueryPlannerParams& plannerParams,
                                            PlanYieldPolicy::YieldPolicy yieldPolicy,
                                            const MultipleCollectionAccessor& collections,
                                            QuerySolutionVector solutions,
                                            StringSet topLevelSampleFieldNames,
                                            bool hasRelevantMultikeyIndex) const;
};

StatusWith<PlanRankingResult> getBestCBRPlan(OperationContext* opCtx,
                                             CanonicalQuery& query,
                                             QueryPlannerParams& plannerParams,
                                             PlanYieldPolicy::YieldPolicy yieldPolicy,
                                             const MultipleCollectionAccessor& collections,
                                             StringSet topLevelSampleFieldNames,
                                             bool hasRelevantMultikeyIndex);


}  // namespace plan_ranking
}  // namespace mongo
