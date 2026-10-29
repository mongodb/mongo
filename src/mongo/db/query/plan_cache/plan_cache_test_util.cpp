// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_cache/plan_cache_test_util.h"

#include "mongo/db/exec/plan_cache_util.h"

namespace mongo {
namespace {
const CollectionPtr emptyCollectionPtr{};
}

std::unique_ptr<plan_ranker::PlanRankingDecision> createDecision(size_t numPlans, size_t works) {
    auto why = std::make_unique<plan_ranker::PlanRankingDecision>();
    std::vector<std::unique_ptr<PlanStageStats>> stats;
    for (size_t i = 0; i < numPlans; ++i) {
        CommonStats common("COLLSCAN");
        auto stat = std::make_unique<PlanStageStats>(common, STAGE_COLLSCAN);
        stat->specific.reset(new CollectionScanStats());
        stat->common.works = works;
        stats.push_back(std::move(stat));
        why->scores.push_back(0U);
        why->candidateOrder.push_back(i);
    }
    why->stats.candidatePlanStats = std::move(stats);
    return why;
}

PlanCacheCallbacksImpl<PlanCacheKey, SolutionCacheData, plan_cache_debug_info::DebugInfo>
createCallback(const CanonicalQuery& cq, const plan_ranker::PlanRankingDecision& decision) {
    auto buildDebugInfoFn = [&]() -> plan_cache_debug_info::DebugInfo {
        return plan_cache_util::buildDebugInfo(cq, decision.clone());
    };
    auto printCachedPlanFn = [](const SolutionCacheData& plan) {
        return plan.toString();
    };
    return {cq, std::move(buildDebugInfoFn), printCachedPlanFn, emptyCollectionPtr};
}
}  // namespace mongo
