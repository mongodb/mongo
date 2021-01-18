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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_cached_solution_planner.h"

#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/sbe_multi_planner.h"
#include "mongo/db/query/stage_builder_util.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/logv2/log.h"

namespace mongo::sbe {
CandidatePlans CachedSolutionPlanner::plan(
    std::vector<std::unique_ptr<QuerySolution>> solutions,
    std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots) {
    invariant(solutions.size() == 1);
    invariant(solutions.size() == roots.size());

    auto candidate = [&]() {
        auto candidates = collectExecutionStats(std::move(solutions), std::move(roots));
        invariant(candidates.size() == 1);
        return std::move(candidates[0]);
    }();
    auto explainer = plan_explainer_factory::make(
        candidate.root.get(), &candidate.data, candidate.solution.get());

    if (candidate.failed) {
        // On failure, fall back to replanning the whole query. We neither evict the existing cache
        // entry, nor cache the result of replanning.
        LOGV2_DEBUG(2057901,
                    1,
                    "Execution of cached plan failed, falling back to replan",
                    "query"_attr = redact(_cq.toStringShort()),
                    "planSummary"_attr = explainer->getPlanSummary());
        return replan(false);
    }

    auto stats{candidate.root->getStats(false /* includeDebugInfo  */)};
    auto numReads{calculateNumberOfReads(stats.get())};
    // If the cached plan hit EOF quickly enough, or still as efficient as before, then no need to
    // replan. Finalize the cached plan and return it.
    if (stats->common.isEOF || numReads <= _decisionReads) {
        return {makeVector<plan_ranker::CandidatePlan>(
                    finalizeExecutionPlan(std::move(stats), std::move(candidate))),
                0};
    }

    // If we're here, the trial period took more than 'maxReadsBeforeReplan' physical reads. This
    // plan may not be efficient any longer, so we replan from scratch.
    LOGV2_DEBUG(
        2058001,
        1,
        "Evicting cache entry for a query and replanning it since the number of required works "
        "mismatch the number of cached works",
        "maxReadsBeforeReplan"_attr = numReads,
        "decisionReads"_attr = _decisionReads,
        "query"_attr = redact(_cq.toStringShort()),
        "planSummary"_attr = explainer->getPlanSummary());
    return replan(true);
}

plan_ranker::CandidatePlan CachedSolutionPlanner::finalizeExecutionPlan(
    std::unique_ptr<PlanStageStats> stats, plan_ranker::CandidatePlan candidate) const {
    // If the winning stage has exited early, clear the results queue and reopen the plan stage
    // tree, as we cannot resume such execution tree from where the trial run has stopped, and, as
    // a result, we cannot stash the results returned so far in the plan executor.
    if (!stats->common.isEOF && candidate.exitedEarly) {
        candidate.root->close();
        candidate.root->open(true);
        // Clear the results queue.
        candidate.results = decltype(candidate.results){};
    }

    return candidate;
}

CandidatePlans CachedSolutionPlanner::replan(bool shouldCache) const {
    // The plan drawn from the cache is being discarded, and should no longer be registered with the
    // yield policy.
    _yieldPolicy->clearRegisteredPlans();

    if (shouldCache) {
        // Deactivate the current cache entry.
        auto cache = CollectionQueryInfo::get(_collection).getPlanCache();
        cache->deactivate(_cq);
    }

    // Use the query planning module to plan the whole query.
    auto solutions = uassertStatusOK(QueryPlanner::plan(_cq, _queryParams));
    if (solutions.size() == 1) {
        // Only one possible plan. Build the stages from the solution.
        auto&& [root, data] = stage_builder::buildSlotBasedExecutableTree(
            _opCtx, _collection, _cq, *solutions[0], _yieldPolicy);
        prepareExecutionPlan(root.get(), &data);

        auto explainer = plan_explainer_factory::make(root.get(), &data, solutions[0].get());
        LOGV2_DEBUG(
            2058101,
            1,
            "Replanning of query resulted in a single query solution, which will not be cached. ",
            "query"_attr = redact(_cq.toStringShort()),
            "planSummary"_attr = explainer->getPlanSummary(),
            "shouldCache"_attr = (shouldCache ? "yes" : "no"));
        return {makeVector<plan_ranker::CandidatePlan>(plan_ranker::CandidatePlan{
                    std::move(solutions[0]), std::move(root), std::move(data)}),
                0};
    }

    // Many solutions. Build a plan stage tree for each solution and create a multi planner to pick
    // the best, update the cache, and so on.
    std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots;
    for (auto&& solution : solutions) {
        if (solution->cacheData.get()) {
            solution->cacheData->indexFilterApplied = _queryParams.indexFiltersApplied;
        }

        roots.push_back(stage_builder::buildSlotBasedExecutableTree(
            _opCtx, _collection, _cq, *solution, _yieldPolicy));
    }

    const auto cachingMode =
        shouldCache ? PlanCachingMode::AlwaysCache : PlanCachingMode::NeverCache;
    MultiPlanner multiPlanner{_opCtx, _collection, _cq, cachingMode, _yieldPolicy};
    auto&& [candidates, winnerIdx] = multiPlanner.plan(std::move(solutions), std::move(roots));
    auto explainer = plan_explainer_factory::make(candidates[winnerIdx].root.get(),
                                                  &candidates[winnerIdx].data,
                                                  candidates[winnerIdx].solution.get());
    LOGV2_DEBUG(2058201,
                1,
                "Query plan after replanning and its cache status",
                "query"_attr = redact(_cq.toStringShort()),
                "planSummary"_attr = explainer->getPlanSummary(),
                "shouldCache"_attr = (shouldCache ? "yes" : "no"));
    return {std::move(candidates), winnerIdx};
}
}  // namespace mongo::sbe
