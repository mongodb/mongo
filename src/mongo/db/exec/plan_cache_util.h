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

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/sbe_plan_ranker.h"

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
 * Caches the best candidate plan, chosen from the given 'candidates' based on the 'ranking'
 * decision, if the 'query' is of a type that can be cached. Otherwise, does nothing.
 *
 * The 'cachingMode' specifies whether the query should be:
 *    * Always cached.
 *    * Never cached.
 *    * Cached, except in certain special cases.
 */
template <typename PlanStageType, typename ResultType, typename Data>
void updatePlanCache(
    OperationContext* opCtx,
    const Collection* collection,
    PlanCachingMode cachingMode,
    const CanonicalQuery& query,
    std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
    const std::vector<plan_ranker::BaseCandidatePlan<PlanStageType, ResultType, Data>>&
        candidates) {
    auto winnerIdx = ranking->candidateOrder[0];
    invariant(winnerIdx >= 0 && winnerIdx < candidates.size());

    // Even if the query is of a cacheable shape, the caller might have indicated that we shouldn't
    // write to the plan cache.
    //
    // TODO: We can remove this if we introduce replanning logic to the SubplanStage.
    bool canCache = (cachingMode == PlanCachingMode::AlwaysCache);
    if (cachingMode == PlanCachingMode::SometimesCache) {
        // In "sometimes cache" mode, we cache unless we hit one of the special cases below.
        canCache = true;

        if (ranking->tieForBest) {
            // The winning plan tied with the runner-up and we're using "sometimes cache" mode. We
            // will not write a plan cache entry.
            canCache = false;

            // These arrays having two or more entries is implied by 'tieForBest'.
            invariant(ranking->scores.size() > 1U);
            invariant(ranking->candidateOrder.size() > 1U);

            size_t winnerIdx = ranking->candidateOrder[0];
            size_t runnerUpIdx = ranking->candidateOrder[1];

            log_detail::logTieForBest(query.toStringShort(),
                                      ranking->scores[0],
                                      ranking->scores[1],
                                      Explain::getPlanSummary(&*candidates[winnerIdx].root),
                                      Explain::getPlanSummary(&*candidates[runnerUpIdx].root));
        }

        if (candidates[winnerIdx].results.empty()) {
            // We're using the "sometimes cache" mode, and the winning plan produced no results
            // during the plan ranking trial period. We will not write a plan cache entry.
            canCache = false;
            log_detail::logNotCachingZeroResults(
                query.toStringShort(),
                ranking->scores[0],
                Explain::getPlanSummary(&*candidates[winnerIdx].root));
        }
    }

    // Store the choice we just made in the cache, if the query is of a type that is safe to
    // cache.
    if (PlanCache::shouldCacheQuery(query) && canCache) {
        // Create list of candidate solutions for the cache with the best solution at the front.
        std::vector<QuerySolution*> solutions;

        // Generate solutions and ranking decisions sorted by score.
        for (auto&& ix : ranking->candidateOrder) {
            solutions.push_back(candidates[ix].solution.get());
        }
        // Insert the failed plans in the back.
        for (auto&& ix : ranking->failedCandidates) {
            solutions.push_back(candidates[ix].solution.get());
        }

        // Check solution cache data. Do not add to cache if we have any invalid SolutionCacheData
        // data. One known example is 2D queries.
        bool validSolutions = true;
        for (size_t ix = 0; ix < solutions.size(); ++ix) {
            if (nullptr == solutions[ix]->cacheData.get()) {
                log_detail::logNotCachingNoData(solutions[ix]->toString());
                validSolutions = false;
                break;
            }
        }

        if (validSolutions) {
            uassertStatusOK(CollectionQueryInfo::get(collection)
                                .getPlanCache()
                                ->set(query,
                                      solutions,
                                      std::move(ranking),
                                      opCtx->getServiceContext()->getPreciseClockSource()->now()));
        }
    }
}
}  // namespace plan_cache_util
}  // namespace mongo
