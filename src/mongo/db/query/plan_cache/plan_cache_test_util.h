// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_cache_callbacks_impl.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_ranking_decision.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * Utility function to create a PlanRankingDecision.
 */
std::unique_ptr<plan_ranker::PlanRankingDecision> createDecision(size_t numPlans, size_t works = 0);

PlanCacheCallbacksImpl<PlanCacheKey, SolutionCacheData, plan_cache_debug_info::DebugInfo>
createCallback(const CanonicalQuery& cq, const plan_ranker::PlanRankingDecision& decision);

}  // namespace mongo
