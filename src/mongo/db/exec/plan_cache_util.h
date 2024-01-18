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

namespace plan_cache_util {
// The logging facility enforces the rule that logging should not be done in a header file. Since
// the template classes and functions below must be defined in the header file and since they do use
// the logging facility, we have to define the helper functions below to perform the actual logging
// operation from template code.
namespace log_detail {
void logTieForBest(std::string&& query,
                   double winnerScore,
                   double runnerUpScore,
                   std::string winnerPlanSummary,
                   std::string runnerUpPlanSummary);
void logNotCachingZeroResults(std::string&& query, double score, std::string winnerPlanSummary);
void logNotCachingNoData(std::string&& solution);
}  // namespace log_detail

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

/**
 * Caches the best candidate execution plan for 'query', chosen from the given 'candidates' based on
 * the 'ranking' decision, if the 'query' is of a type that can be cached. Otherwise, does nothing.
 *
 * The 'cachingMode' specifies whether the query should be:
 *    * Always cached.
 *    * Never cached.
 *    * Cached, except in certain special cases.
 *
 * This method is shared between Classic and SBE. The plan roots 'candidates[i].root' have different
 * types between the two:
 *    * Classic - mongo::PlanStage*
 *    * SBE     - std::unique_ptr<sbe::PlanStage>>
 * This breaks polymorphism because native pointers and std::unique_ptr must be handled differently.
 * std::is_same_v is used to distinguish in these cases.
 */
template <typename PlanStageType, typename ResultType, typename Data>
void updatePlanCacheFromCandidates(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    PlanCachingMode cachingMode,
    const CanonicalQuery& query,
    std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
    std::vector<plan_ranker::BaseCandidatePlan<PlanStageType, ResultType, Data>>& candidates) {
    auto winnerIdx = ranking->candidateOrder[0];
    invariant(winnerIdx >= 0 && winnerIdx < candidates.size());
    auto& winningPlan = candidates[winnerIdx];

    // Even if the query is of a cacheable shape, the caller might have indicated that we shouldn't
    // write to the plan cache.
    //
    // TODO: We can remove this if we introduce replanning logic to the SubplanStage.
    bool canCache = (cachingMode == PlanCachingMode::AlwaysCache);
    if (cachingMode == PlanCachingMode::SometimesCache) {
        // In "sometimes cache" mode, we cache unless we hit one of the special cases below.
        canCache = true;

        if (ranking->tieForBest()) {
            // The winning plan tied with the runner-up and we're using "sometimes cache" mode. We
            // will not write a plan cache entry.
            canCache = false;

            // These arrays having two or more entries is implied by 'tieForBest'.
            invariant(ranking->scores.size() > 1U);
            invariant(ranking->candidateOrder.size() > 1U);

            auto runnerUpIdx = ranking->candidateOrder[1];

            auto&& [winnerExplainer, runnerUpExplainer] = [&]() {
                if constexpr (std::is_same_v<PlanStageType, std::unique_ptr<sbe::PlanStage>>) {
                    return std::make_pair(
                        plan_explainer_factory::make(winningPlan.root.get(),
                                                     &winningPlan.data.stageData,
                                                     winningPlan.solution.get()),
                        plan_explainer_factory::make(candidates[runnerUpIdx].root.get(),
                                                     &candidates[runnerUpIdx].data.stageData,
                                                     candidates[runnerUpIdx].solution.get()));
                } else {
                    static_assert(std::is_same_v<PlanStageType, PlanStage*>);
                    return std::make_pair(
                        plan_explainer_factory::make(winningPlan.root),
                        plan_explainer_factory::make(candidates[runnerUpIdx].root));
                }
            }();

            log_detail::logTieForBest(query.toStringShort(),
                                      ranking->scores[0],
                                      ranking->scores[1],
                                      winnerExplainer->getPlanSummary(),
                                      runnerUpExplainer->getPlanSummary());
        }

        if (winningPlan.results.empty()) {
            // We're using the "sometimes cache" mode, and the winning plan produced no results
            // during the plan ranking trial period. We will not write a plan cache entry.
            canCache = false;
            auto winnerExplainer = [&]() {
                if constexpr (std::is_same_v<PlanStageType, std::unique_ptr<sbe::PlanStage>>) {
                    return plan_explainer_factory::make(winningPlan.root.get(),
                                                        &winningPlan.data.stageData,
                                                        winningPlan.solution.get());
                } else {
                    static_assert(std::is_same_v<PlanStageType, PlanStage*>);
                    return plan_explainer_factory::make(winningPlan.root);
                }
            }();

            log_detail::logNotCachingZeroResults(
                query.toStringShort(), ranking->scores[0], winnerExplainer->getPlanSummary());
        }
    }

    // Store the choice we just made in the cache, if the query is of a type that is safe to
    // cache.
    QuerySolution* solution = winningPlan.solution.get();
    if (canCache && !query.isUncacheableSbe() && shouldCacheQuery(query) &&
        solution->isEligibleForPlanCache()) {
        const CollectionPtr& collection = collections.getMainCollection();
        auto rankingDecision = ranking.get();

        if (solution->cacheData != nullptr) {
            if constexpr (std::is_same_v<PlanStageType, std::unique_ptr<sbe::PlanStage>>) {
                tassert(6142201,
                        "The winning CandidatePlan should contain the original plan",
                        winningPlan.clonedPlan);

                // Clone the winning SBE plan and its auxiliary data.
                auto cachedPlan = std::make_unique<sbe::CachedSbePlan>(
                    std::move(winningPlan.clonedPlan->first),
                    std::move(winningPlan.clonedPlan->second.stageData),
                    solution->hash());
                cachedPlan->indexFilterApplied = solution->indexFilterApplied;

                auto buildDebugInfoFn = [&]() -> plan_cache_debug_info::DebugInfoSBE {
                    return buildDebugInfo(solution);
                };
                PlanCacheCallbacksImpl<sbe::PlanCacheKey,
                                       sbe::CachedSbePlan,
                                       plan_cache_debug_info::DebugInfoSBE>
                    callbacks{query, buildDebugInfoFn};

                auto isSensitive = CurOp::get(opCtx)->getShouldOmitDiagnosticInformation();
                uassertStatusOK(sbe::getPlanCache(opCtx).set(
                    plan_cache_key_factory::make(
                        query, collections, canonical_query_encoder::Optimizer::kSbeStageBuilders),
                    std::move(cachedPlan),
                    *rankingDecision,
                    opCtx->getServiceContext()->getPreciseClockSource()->now(),
                    &callbacks,
                    isSensitive ? PlanSecurityLevel::kSensitive : PlanSecurityLevel::kNotSensitive,
                    boost::none /* worksGrowthCoefficient */));
            } else {
                static_assert(std::is_same_v<PlanStageType, PlanStage*>);
                auto buildDebugInfoFn = [&]() -> plan_cache_debug_info::DebugInfo {
                    return buildDebugInfo(query, std::move(ranking));
                };
                PlanCacheCallbacksImpl<PlanCacheKey,
                                       SolutionCacheData,
                                       plan_cache_debug_info::DebugInfo>
                    callbacks{query, buildDebugInfoFn};
                solution->cacheData->indexFilterApplied = solution->indexFilterApplied;
                solution->cacheData->solutionHash = solution->hash();
                auto isSensitive = CurOp::get(opCtx)->getShouldOmitDiagnosticInformation();
                uassertStatusOK(
                    CollectionQueryInfo::get(collection)
                        .getPlanCache()
                        ->set(plan_cache_key_factory::make<PlanCacheKey>(query, collection),
                              solution->cacheData->clone(),
                              *rankingDecision,
                              opCtx->getServiceContext()->getPreciseClockSource()->now(),
                              &callbacks,
                              isSensitive ? PlanSecurityLevel::kSensitive
                                          : PlanSecurityLevel::kNotSensitive,
                              boost::none /* worksGrowthCoefficient */));
            }
        } else {
            log_detail::logNotCachingNoData(solution->toString());
        }
    }
}

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
