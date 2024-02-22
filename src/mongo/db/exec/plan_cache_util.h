/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/none.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/classic_plan_cache.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_cache_callbacks.h"
#include "mongo/db/query/plan_cache_debug_info.h"
#include "mongo/db/query/plan_cache_key_factory.h"
#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/plan_ranking_decision.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_plan_cache.h"
#include "mongo/db/query/sbe_plan_ranker.h"
#include "mongo/db/query/sbe_stage_builder_plan_data.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"

namespace mongo {
/**
 * Specifies how the multi-planner should interact with the plan cache.
 */
enum class PlanCachingMode {
    // Always write a cache entry for the winning plan to the plan cache, overwriting any
    // previously existing cache entry for the query shape.
    AlwaysCache,

    // Write a cache entry for the query shape *unless* we encounter one of the following edge
    // cases:
    //  - Two or more plans tied for the win.
    //  - The winning plan returned zero query results during the plan ranking trial period.
    SometimesCache,

    // Do not write to the plan cache.
    NeverCache,
};

/**
 * Returns the stricter PlanCachingMode between 'lhs' and 'rhs'.
 */
inline PlanCachingMode stricter(PlanCachingMode lhs, PlanCachingMode rhs) {
    return std::max(lhs, rhs);
}

namespace plan_cache_util {

/**
 * Builds "DebugInfo" for storing in the classic plan cache.
 */
plan_cache_debug_info::DebugInfo buildDebugInfo(
    const CanonicalQuery& query, std::unique_ptr<const plan_ranker::PlanRankingDecision> decision);

/**
 * Builds "DebugInfoSBE" for storing in the SBE plan cache. Pre-computes necessary debugging
 * information to build "PlanExplainerSBE" when recoverying the cached SBE plan from the cache.
 */
plan_cache_debug_info::DebugInfoSBE buildDebugInfo(const QuerySolution* solution);

/*
 * Returns true if plan cache should be updated.
 */
bool shouldUpdatePlanCache(OperationContext* opCtx,
                           PlanCachingMode cachingMode,
                           const CanonicalQuery& query,
                           const plan_ranker::PlanRankingDecision& ranking,
                           const PlanExplainer* winnerExplainer,
                           const PlanExplainer* runnerUpExplainer,
                           const QuerySolution* winningPlan);

/**
 * Caches the best candidate execution plan for 'query' in Classic plan cache, chosen from the given
 * 'candidates' from Classic based on the 'ranking' decision, if the 'query' is of a type that can
 * be cached. Otherwise, does nothing.
 */
void updateClassicPlanCacheFromClassicCandidates(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    PlanCachingMode cachingMode,
    const CanonicalQuery& query,
    std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
    std::vector<plan_ranker::CandidatePlan>& candidates);

/**
 * Caches the best candidate execution plan for 'query' in SBE plan cache, chosen from the given
 * 'candidates' from SBE based on the 'ranking' decision, if the 'query' is of a type that can be
 * cached. Otherwise, does nothing.
 */
void updateSbePlanCacheFromSbeCandidates(OperationContext* opCtx,
                                         const MultipleCollectionAccessor& collections,
                                         PlanCachingMode cachingMode,
                                         const CanonicalQuery& query,
                                         std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
                                         std::vector<sbe::plan_ranker::CandidatePlan>& candidates);

/**
 * Caches the best candidate execution plan for 'query' in SBE plan cache, chosen from the given
 * 'candidates' based on the 'ranking' decision, if the 'query' is of a type that can be cached.
 * Otherwise, does nothing.
 *
 * The 'cachingMode' specifies whether the query should be:
 *    * Always cached.
 *    * Never cached.
 *    * Cached, except in certain special cases.
 */
void updateSbePlanCacheFromClassicCandidates(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    PlanCachingMode cachingMode,
    const CanonicalQuery& query,
    const plan_ranker::PlanRankingDecision& ranking,
    const std::vector<plan_ranker::CandidatePlan>& candidates,
    const std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>& sbePlanAndData,
    const QuerySolution* winningSolution);

/**
 * Caches the plan 'root' along with its accompanying 'data' if the 'query' is of a type that can be
 * cached. Otherwise, does nothing.
 *
 * The given plan will be "pinned" to the cache and will not be subject to replanning. Once put into
 * the cache, the plan immediately becomes "active".
 */
void updatePlanCache(OperationContext* opCtx,
                     const MultipleCollectionAccessor& collections,
                     const CanonicalQuery& query,
                     const QuerySolution& solution,
                     const sbe::PlanStage& root,
                     stage_builder::PlanStageData stageData);
}  // namespace plan_cache_util
}  // namespace mongo
