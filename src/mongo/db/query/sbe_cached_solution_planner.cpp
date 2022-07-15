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

#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_cached_solution_planner.h"

#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_cache_key_factory.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/sbe_multi_planner.h"
#include "mongo/db/query/stage_builder_util.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::sbe {
CandidatePlans CachedSolutionPlanner::plan(
    std::vector<std::unique_ptr<QuerySolution>> solutions,
    std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots) {
    if (!_cq.pipeline().empty()) {
        // When "featureFlagSbeFull" is enabled we use the SBE plan cache. If the plan cache is
        // enabled we'd like to check if there is any foreign collection in the hash_lookup stage
        // that is no longer eligible for it. In this case we invalidate the cache and immediately
        // replan without ever running a trial period.
        auto secondaryCollectionsInfo =
            fillOutSecondaryCollectionsInformation(_opCtx, _collections, &_cq);

        if (feature_flags::gFeatureFlagSbeFull.isEnabledAndIgnoreFCV()) {
            for (const auto& foreignCollection : roots[0].second.foreignHashJoinCollections) {
                const auto collectionInfo = secondaryCollectionsInfo.find(foreignCollection);
                tassert(6693500,
                        "Foreign collection must be present in the collections info",
                        collectionInfo != secondaryCollectionsInfo.end());
                tassert(6693501, "Foreign collection must exist", collectionInfo->second.exists);

                if (!QueryPlannerAnalysis::isEligibleForHashJoin(collectionInfo->second)) {
                    return replan(/* shouldCache */ true,
                                  str::stream() << "Foreign collection " << foreignCollection
                                                << " is not eligible for hash join anymore");
                }
            }
        } else {
            // The SBE plan cache is not enabled. If the cached plan is accepted we'd like to keep
            // the results from the trials even if there are parts of agg pipelines being lowered
            // into SBE, so we run the trial with the extended plan. This works because
            // TrialRunTracker, attached to HashAgg stage in $group queries, tracks as "results" the
            // results of its child stage. For $lookup queries, the TrialRunTracker will only track
            // the number of reads from the local side. Thus, we can use the number of reads the
            // plan was cached with during multiplanning even though multiplanning ran trials of
            // pre-extended plans.
            //
            // The SBE plan cache stores the entire plan, including the part for any agg pipeline
            // pushed down to SBE. Therefore, this logic is only necessary when "featureFlagSbeFull"
            // is disabled.
            _yieldPolicy->clearRegisteredPlans();
            solutions[0] = QueryPlanner::extendWithAggPipeline(
                _cq, std::move(solutions[0]), secondaryCollectionsInfo);
            roots[0] = stage_builder::buildSlotBasedExecutableTree(
                _opCtx, _collections, _cq, *solutions[0], _yieldPolicy);
        }
    }

    // If the '_decisionReads' is not present then we do not run a trial period, keeping the current
    // plan.
    if (!_decisionReads) {
        const auto status = prepareExecutionPlan(
            roots[0].first.get(), &roots[0].second, true /* preparingFromCache */);
        uassertStatusOK(status);
        bool exitedEarly;

        // Discarding SlotAccessor pointers as they will be reacquired later.
        std::tie(std::ignore, std::ignore, exitedEarly) = status.getValue();
        tassert(
            6693502, "TrialRunTracker is not attached therefore can not exit early", !exitedEarly);
        return {makeVector(plan_ranker::CandidatePlan{std::move(solutions[0]),
                                                      std::move(roots[0].first),
                                                      std::move(roots[0].second),
                                                      false /* exitedEarly*/,
                                                      Status::OK()}),
                0};
    }

    const size_t maxReadsBeforeReplan = internalQueryCacheEvictionRatio * _decisionReads.get();

    // In cached solution planning we collect execution stats with an upper bound on reads allowed
    // per trial run computed based on previous decision reads. If the trial run ends before
    // reaching EOF, it will use the 'checkNumReads' function to determine if it should continue
    // executing or immediately terminate execution.
    auto candidate = collectExecutionStatsForCachedPlan(std::move(solutions[0]),
                                                        std::move(roots[0].first),
                                                        std::move(roots[0].second),
                                                        maxReadsBeforeReplan);
    auto explainer = plan_explainer_factory::make(
        candidate.root.get(),
        &candidate.data,
        candidate.solution.get(),
        {},    /* optimizedData */
        {},    /* rejectedCandidates */
        false, /* isMultiPlan */
        candidate.data.debugInfo
            ? std::make_unique<plan_cache_debug_info::DebugInfoSBE>(*candidate.data.debugInfo)
            : nullptr);

    if (!candidate.status.isOK()) {
        // On failure, fall back to replanning the whole query. We neither evict the existing cache
        // entry, nor cache the result of replanning.
        LOGV2_DEBUG(2057901,
                    1,
                    "Execution of cached plan failed, falling back to replan",
                    "query"_attr = redact(_cq.toStringShort()),
                    "planSummary"_attr = explainer->getPlanSummary());
        return replan(false, str::stream() << "cached plan returned: " << candidate.status);
    }

    // If the trial run did not exit early, it means no replanning is necessary and can return this
    // candidate to the executor. All results generated during the trial are stored with the
    // candidate so that the executor will be able to reuse them.
    if (!candidate.exitedEarly) {
        return {makeVector(std::move(candidate)), 0};
    }

    // If we're here, the trial period took more than 'maxReadsBeforeReplan' physical reads. This
    // plan may not be efficient any longer, so we replan from scratch.
    auto visitor = PlanStatsNumReadsVisitor{};
    candidate.root->accumulate(kEmptyPlanNodeId, &visitor);
    const auto numReads = visitor.numReads;
    LOGV2_DEBUG(
        2058001,
        1,
        "Evicting cache entry for a query and replanning it since the number of required reads "
        "mismatch the number of cached reads",
        "maxReadsBeforeReplan"_attr = maxReadsBeforeReplan,
        "decisionReads"_attr = *_decisionReads,
        "query"_attr = redact(_cq.toStringShort()),
        "planSummary"_attr = explainer->getPlanSummary());
    return replan(
        true,
        str::stream()
            << "cached plan was less efficient than expected: expected trial execution to take "
            << *_decisionReads << " reads but it took at least " << numReads << " reads");
}

plan_ranker::CandidatePlan CachedSolutionPlanner::collectExecutionStatsForCachedPlan(
    std::unique_ptr<QuerySolution> solution,
    std::unique_ptr<PlanStage> root,
    stage_builder::PlanStageData data,
    size_t maxTrialPeriodNumReads) {
    const auto maxNumResults{trial_period::getTrialPeriodNumToReturn(_cq)};

    plan_ranker::CandidatePlan candidate{std::move(solution),
                                         std::move(root),
                                         std::move(data),
                                         false /* exitedEarly*/,
                                         Status::OK()};

    ON_BLOCK_EXIT([rootPtr = candidate.root.get()] { rootPtr->detachFromTrialRunTracker(); });

    // Callback for the tracker when it exceeds any of the tracked metrics. If the tracker exceeds
    // the number of reads without reaching the expected number of results first (which for plans
    // without blocking stages is observed through 'maxNumResults' passed to
    // 'executeCandidateTrial()' and for plans with blocking stages is observed by the tracker
    // itself), it means that the cached plan isn't performing as well as it used to and we'll need
    // to replan, so we let the tracker terminate the trial. Otherwise, the plan is still good and
    // we promote it from a candidate to "normal" by detaching the tracker and letting
    // 'executeCandidateTrial()' reach 'maxNumResults'.
    auto onMetricReached = [&candidate](TrialRunTracker::TrialRunMetric metric) {
        switch (metric) {
            case TrialRunTracker::kNumReads:
                return true;  // terminate the trial run
            case TrialRunTracker::kNumResults:
                candidate.root->detachFromTrialRunTracker();
                return false;  // upgrade the trial run into a normal one
            default:
                MONGO_UNREACHABLE;
        }
    };
    auto tracker = std::make_unique<TrialRunTracker>(
        std::move(onMetricReached), maxNumResults, maxTrialPeriodNumReads);
    candidate.root->attachToTrialRunTracker(tracker.get());
    executeCandidateTrial(&candidate, maxNumResults, /*isCachedPlanTrial*/ true);

    return candidate;
}

CandidatePlans CachedSolutionPlanner::replan(bool shouldCache, std::string reason) const {
    // The plan drawn from the cache is being discarded, and should no longer be registered with the
    // yield policy.
    _yieldPolicy->clearRegisteredPlans();

    if (shouldCache) {
        const auto& mainColl = _collections.getMainCollection();
        // Deactivate the current cache entry.
        //
        // TODO SERVER-64882: We currently deactivate cache entries in both the classic and SBE plan
        // caches. Once we always use the SBE plan cache for queries eligible for SBE, this code can
        // be simplified to only deactivate the entry in the SBE plan cache.
        auto cache = CollectionQueryInfo::get(mainColl).getPlanCache();
        cache->deactivate(plan_cache_key_factory::make<mongo::PlanCacheKey>(_cq, mainColl));
        if (feature_flags::gFeatureFlagSbeFull.isEnabledAndIgnoreFCV()) {
            auto&& sbePlanCache = sbe::getPlanCache(_opCtx);
            sbePlanCache.deactivate(plan_cache_key_factory::make(_cq, _collections));
        }
    }

    auto buildExecutableTree = [&](const QuerySolution& sol) {
        auto [root, data] = stage_builder::buildSlotBasedExecutableTree(
            _opCtx, _collections, _cq, sol, _yieldPolicy);
        data.replanReason.emplace(reason);
        return std::make_pair(std::move(root), std::move(data));
    };

    QueryPlannerParams plannerParams;
    plannerParams.options = _queryParams.options;
    fillOutPlannerParams(_opCtx, _collections, &_cq, &plannerParams);
    // Use the query planning module to plan the whole query.
    auto statusWithMultiPlanSolns = QueryPlanner::plan(_cq, plannerParams);
    auto solutions = uassertStatusOK(std::move(statusWithMultiPlanSolns));

    if (solutions.size() == 1) {
        if (!_cq.pipeline().empty()) {
            auto secondaryCollectionsInfo =
                fillOutSecondaryCollectionsInformation(_opCtx, _collections, &_cq);
            solutions[0] = QueryPlanner::extendWithAggPipeline(
                _cq, std::move(solutions[0]), secondaryCollectionsInfo);
        }

        // Only one possible plan. Build the stages from the solution.
        auto [root, data] = buildExecutableTree(*solutions[0]);
        auto status = prepareExecutionPlan(root.get(), &data);
        uassertStatusOK(status);
        auto [result, recordId, exitedEarly] = status.getValue();
        tassert(
            5323800, "cached planner unexpectedly exited early during prepare phase", !exitedEarly);

        auto explainer = plan_explainer_factory::make(root.get(), &data, solutions[0].get());
        LOGV2_DEBUG(2058101,
                    1,
                    "Replanning of query resulted in a single query solution",
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
        solution->indexFilterApplied = plannerParams.indexFiltersApplied;

        roots.push_back(buildExecutableTree(*solution));
    }

    const auto cachingMode =
        shouldCache ? PlanCachingMode::AlwaysCache : PlanCachingMode::NeverCache;
    MultiPlanner multiPlanner{_opCtx, _collections, _cq, plannerParams, cachingMode, _yieldPolicy};
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
