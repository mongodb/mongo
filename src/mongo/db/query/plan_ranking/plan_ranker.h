// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/runtime_planners/planner_types.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/util/modules.h"


namespace mongo {
namespace plan_ranking {

struct RankingContext {
    QuerySolutionVector solutions;
    StringSet topLevelSampleFieldNames = {};
    bool hasRelevantMultikeyIndex = false;
};

class PlanRankingStrategy {
public:
    virtual StatusWith<PlanRankingResult> rankPlans(PlannerData& pd, RankingContext& rctx) = 0;

    virtual ~PlanRankingStrategy() = default;
};

std::unique_ptr<PlanRankingStrategy> makeStrategy(QueryPlanRankerEnum planRanker,
                                                  QueryMixedPlanRankingStrategyEnum mixedStrategy);

/**
 * The PlanRanker is responsible for ranking candidate query plans and selecting the best plan(s)
 * to be executed.
 *
 * It will work as a dispatcher to the appropriate plan ranking strategy based on the provided plan
 * ranking mode. Currently, it supports both cost-based ranking (CBR) and multi-planning strategies.
 */
class PlanRanker {
public:
    // If the plan will be executed in SBE (i.e. when 'isClassic' is false) then we will not use one
    // of the CBR fallback strategies for plan ranking and instead use multiplanning.
    // TODO SERVER-117707: Remove this restriction.
    StatusWith<PlanRankingResult> rankPlans(
        OperationContext* opCtx,
        CanonicalQuery& query,
        QueryPlannerParams& plannerParams,
        PlanYieldPolicy::YieldPolicy yieldPolicy,
        const MultipleCollectionAccessor& collections,
        // PlannerData for classic multiplanner. We only need the classic one since
        // multiplanning only runs with classic, even if SBE is enabled.
        PlannerData multiPlannerData,
        bool isClassic);
};
}  // namespace plan_ranking
}  // namespace mongo
