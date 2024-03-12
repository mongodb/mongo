/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/exec/trial_period_utils.h"
#include "mongo/db/query/classic_runtime_planner_for_sbe/planner_interface.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/sbe_trial_runtime_executor.h"

#include "mongo/db/query/stage_builder_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::classic_runtime_planner_for_sbe {

CachedPlanner::CachedPlanner(PlannerDataForSBE plannerData,
                             PlanYieldPolicy::YieldPolicy yieldPolicy,
                             std::unique_ptr<sbe::CachedPlanHolder> cachedPlanHolder)
    : PlannerBase(std::move(plannerData)),
      _yieldPolicy(std::move(yieldPolicy)),
      _cachedPlanHolder(std::move(cachedPlanHolder)) {}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> CachedPlanner::plan() {
    _cachedPlanHolder->cachedPlan->planStageData.debugInfo = _cachedPlanHolder->debugInfo;
    const auto& decisionReads = _cachedPlanHolder->decisionWorks;
    LOGV2_DEBUG(
        8523404, 5, "Recovering SBE plan from the cache", "decisionReads"_attr = decisionReads);
    if (!decisionReads) {
        return prepareSbePlanExecutor(nullptr /*solution*/,
                                      {std::move(_cachedPlanHolder->cachedPlan->root),
                                       std::move(_cachedPlanHolder->cachedPlan->planStageData)},
                                      true /*isFromPlanCache*/,
                                      cachedPlanHash(),
                                      nullptr /*classicRuntimePlannerStage*/);
    }

    auto sbePlan = std::move(_cachedPlanHolder->cachedPlan->root);
    auto planStageData = std::move(_cachedPlanHolder->cachedPlan->planStageData);

    const size_t maxReadsBeforeReplan = internalQueryCacheEvictionRatio * decisionReads.get();
    auto candidate = _collectExecutionStatsForCachedPlan(
        std::move(sbePlan), std::move(planStageData), maxReadsBeforeReplan);

    tassert(8523801, "'debugInfo' should be initialized", candidate.data.stageData.debugInfo);
    auto explainer = plan_explainer_factory::make(candidate.root.get(),
                                                  &candidate.data.stageData,
                                                  candidate.solution.get(),
                                                  {},    /* optimizedData */
                                                  {},    /* rejectedCandidates */
                                                  false, /* isMultiPlan */
                                                  true /* isFromPlanCache */,
                                                  true /*matchesCachedPlan*/,
                                                  candidate.data.stageData.debugInfo);

    if (!candidate.status.isOK()) {
        // On failure, fall back to replanning the whole query. We neither evict the existing cache
        // entry, nor cache the result of replanning.
        LOGV2_DEBUG(8523802,
                    1,
                    "Execution of cached plan failed, falling back to replan",
                    "query"_attr = redact(cq()->toStringShort()),
                    "planSummary"_attr = explainer->getPlanSummary(),
                    "error"_attr = candidate.status.toString());
        return _replan(false /*shouldCache*/,
                       str::stream() << "cached plan returned: " << candidate.status);
    }

    // If the trial run did not exit early, it means no replanning is necessary and can return this
    // candidate to the executor. All results generated during the trial are stored with the
    // candidate so that the executor will be able to reuse them.
    if (!candidate.exitedEarly) {
        auto nss = cq()->nss();
        auto remoteCursors = cq()->getExpCtx()->explain
            ? nullptr
            : search_helpers::getSearchRemoteCursors(cq()->cqPipeline());
        auto remoteExplains = cq()->getExpCtx()->explain
            ? search_helpers::getSearchRemoteExplains(cq()->getExpCtxRaw(), cq()->cqPipeline())
            : nullptr;
        return uassertStatusOK(
            plan_executor_factory::make(opCtx(),
                                        extractCq(),
                                        {makeVector(std::move(candidate)), 0 /*winnerIdx*/},
                                        collections(),
                                        plannerOptions(),
                                        std::move(nss),
                                        extractSbeYieldPolicy(),
                                        std::move(remoteCursors),
                                        std::move(remoteExplains)));
    }

    // If we're here, the trial period took more than 'maxReadsBeforeReplan' physical reads. This
    // plan may not be efficient any longer, so we replan from scratch.
    const auto numReads =
        candidate.data.tracker->getMetric<TrialRunTracker::TrialRunMetric::kNumReads>();
    LOGV2_DEBUG(
        8523803,
        1,
        "Evicting cache entry for a query and replanning it since the number of required reads "
        "mismatch the number of cached reads",
        "maxReadsBeforeReplan"_attr = maxReadsBeforeReplan,
        "decisionReads"_attr = *decisionReads,
        "numReads"_attr = numReads,
        "query"_attr = redact(cq()->toStringShort()),
        "planSummary"_attr = explainer->getPlanSummary());
    return _replan(
        true /*shouldCache*/,
        str::stream()
            << "cached plan was less efficient than expected: expected trial execution to take "
            << decisionReads << " reads but it took at least " << numReads << " reads");
}

sbe::plan_ranker::CandidatePlan CachedPlanner::_collectExecutionStatsForCachedPlan(
    std::unique_ptr<sbe::PlanStage> root,
    stage_builder::PlanStageData data,
    size_t maxTrialPeriodNumReads) {
    const auto maxNumResults{trial_period::getTrialPeriodNumToReturn(*cq())};

    sbe::plan_ranker::CandidatePlan candidate{nullptr /*solution*/,
                                              std::move(root),
                                              sbe::plan_ranker::CandidatePlanData{std::move(data)},
                                              false /* exitedEarly*/,
                                              Status::OK(),
                                              true /*isCachedPlan*/};
    ON_BLOCK_EXIT([rootPtr = candidate.root.get()] { rootPtr->detachFromTrialRunTracker(); });

    // Callback for the tracker when it exceeds any of the tracked metrics. If the tracker exceeds
    // the number of reads before returning 'maxNumResults' number of documents, it means that the
    // cached plan isn't performing as well as it used to and we'll need to replan, so we let the
    // tracker terminate the trial. Otherwise, the cached plan is terminated when the number of the
    // results reach 'maxNumResults'.
    auto onMetricReached = [&candidate](TrialRunTracker::TrialRunMetric metric) {
        switch (metric) {
            case TrialRunTracker::kNumReads:
                return true;  // terminate the trial run
            default:
                MONGO_UNREACHABLE;
        }
    };
    candidate.data.tracker =
        std::make_unique<TrialRunTracker>(std::move(onMetricReached),
                                          size_t{0} /*kNumResults*/,
                                          maxTrialPeriodNumReads /*kNumReads*/);
    candidate.root->attachToTrialRunTracker(candidate.data.tracker.get());

    sbe::TrialRuntimeExecutor{
        opCtx(), collections(), *cq(), sbeYieldPolicy(), AllIndicesRequiredChecker{collections()}}
        .executeCachedCandidateTrial(&candidate, maxNumResults);

    return candidate;
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> CachedPlanner::_replan(
    bool shouldCache, std::string replanReason) {
    // The plan drawn from the cache is being discarded, and should no longer be registered with the
    // yield policy.
    sbeYieldPolicy()->clearRegisteredPlans();

    if (shouldCache) {
        // Deactivate the current cache entry.
        auto&& sbePlanCache = sbe::getPlanCache(opCtx());
        sbePlanCache.deactivate(plan_cache_key_factory::make(
            *cq(), collections(), canonical_query_encoder::Optimizer::kSbeStageBuilders));
    }

    // Use the query planning module to plan the whole query.
    auto solutions = uassertStatusOK(QueryPlanner::plan(*cq(), plannerParams()));
    if (solutions.size() == 1) {
        if (!cq()->cqPipeline().empty()) {
            solutions[0] = QueryPlanner::extendWithAggPipeline(
                *cq(), std::move(solutions[0]), plannerParams().secondaryCollectionsInfo);
        }

        auto [root, data] = stage_builder::buildSlotBasedExecutableTree(
            opCtx(), collections(), *cq(), *solutions[0], sbeYieldPolicy());
        data.replanReason.emplace(replanReason);

        auto explainer = plan_explainer_factory::make(root.get(), &data, solutions[0].get());
        LOGV2_DEBUG(8523804,
                    1,
                    "Replanning of query resulted in a single query solution",
                    "query"_attr = redact(cq()->toStringShort()),
                    "planSummary"_attr = explainer->getPlanSummary(),
                    "shouldCache"_attr = (shouldCache ? "yes" : "no"));
        return prepareSbePlanExecutor(std::move(solutions[0]),
                                      std::make_pair(std::move(root), std::move(data)),
                                      false /*isFromPlanCache*/,
                                      cachedPlanHash(),
                                      nullptr /*classicRuntimePlannerStage*/);
    }
    const auto cachingMode =
        shouldCache ? PlanCachingMode::AlwaysCache : PlanCachingMode::NeverCache;
    MultiPlanner multiPlanner{
        extractPlannerData(), std::move(solutions), cachingMode, replanReason};
    auto exec = multiPlanner.plan();
    LOGV2_DEBUG(8523805,
                1,
                "Query plan after replanning and its cache status",
                "query"_attr = redact(exec->getCanonicalQuery()->toStringShort()),
                "planSummary"_attr = exec->getPlanExplainer().getPlanSummary(),
                "shouldCache"_attr = (shouldCache ? "yes" : "no"));
    return exec;
}

}  // namespace mongo::classic_runtime_planner_for_sbe
