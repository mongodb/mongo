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

#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner_for_sbe/planner_interface.h"
#include "mongo/db/exec/trial_period_utils.h"
#include "mongo/db/query/all_indices_required_checker.h"
#include "mongo/db/query/bind_input_params.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/sbe_trial_runtime_executor.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"
#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::classic_runtime_planner_for_sbe {

namespace {
class ValidCandidatePlanner : public PlannerBase {
public:
    ValidCandidatePlanner(PlannerDataForSBE plannerData, sbe::plan_ranker::CandidatePlan candidate)
        : PlannerBase(std::move(plannerData)), _candidate(std::move(candidate)) {}

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExecutor(
        std::unique_ptr<CanonicalQuery> canonicalQuery) override {
        auto nss = cq()->nss();
        auto remoteCursors = cq()->getExpCtx()->getExplain()
            ? nullptr
            : search_helpers::getSearchRemoteCursors(cq()->cqPipeline());
        auto remoteExplains = cq()->getExpCtx()->getExplain()
            ? search_helpers::getSearchRemoteExplains(cq()->getExpCtxRaw(), cq()->cqPipeline())
            : nullptr;
        return uassertStatusOK(plan_executor_factory::make(opCtx(),
                                                           std::move(canonicalQuery),
                                                           std::move(_candidate),
                                                           collections(),
                                                           plannerOptions(),
                                                           std::move(nss),
                                                           extractSbeYieldPolicy(),
                                                           std::move(remoteCursors),
                                                           std::move(remoteExplains)));
    }

private:
    sbe::plan_ranker::CandidatePlan _candidate;
};

/**
 * Recover $where expression JS function predicate from the SBE runtime environemnt, if
 * necessary, so we could successfully replan the query. The primary match expression was
 * modified during the input parameters bind-in process while we were collecting execution
 * stats above.
 */
void recoverWhereExpression(CanonicalQuery* canonicalQuery,
                            sbe::plan_ranker::CandidatePlan candidate) {
    if (canonicalQuery->getExpCtxRaw()->getHasWhereClause()) {
        input_params::recoverWhereExprPredicate(canonicalQuery->getPrimaryMatchExpression(),
                                                candidate.data.stageData);
    }
}

/**
 * Executes the "trial" portion of a single plan until it
 *   - reaches EOF,
 *   - reaches the 'maxNumResults' limit,
 *   - early exits via the TrialRunTracker, or
 *   - returns a failure Status.
 *
 * All documents returned by the plan are enqueued into the 'CandidatePlan->results' queue.
 */
sbe::plan_ranker::CandidatePlan collectExecutionStatsForCachedPlan(
    const PlannerDataForSBE& plannerData,
    std::unique_ptr<QuerySolution> solution,
    const AllIndicesRequiredChecker& indexExistenceChecker,
    std::unique_ptr<sbe::PlanStage> root,
    stage_builder::PlanStageData data,
    size_t maxTrialPeriodNumReads) {
    // If we have an aggregation pipeline, we should track results via kNumResults metric,
    // instead of returned documents, because aggregation stages like $unwind or $group can change
    // the amount of returned documents arbitrarily.
    const size_t maxNumResults = trial_period::getTrialPeriodNumToReturn(*plannerData.cq);
    const size_t maxTrialResults =
        plannerData.cq->cqPipeline().empty() ? maxNumResults : std::numeric_limits<size_t>::max();
    const size_t maxTrialResultsFromPlanningRoot =
        plannerData.cq->cqPipeline().empty() ? 0 : maxNumResults;

    sbe::plan_ranker::CandidatePlan candidate{std::move(solution),
                                              std::move(root),
                                              sbe::plan_ranker::CandidatePlanData{std::move(data)},
                                              false /* exitedEarly*/,
                                              Status::OK(),
                                              true /*isCachedPlan*/};
    ON_BLOCK_EXIT([rootPtr = candidate.root.get()] { rootPtr->detachFromTrialRunTracker(); });

    // Callback for the tracker when it exceeds any of the tracked metrics. If the tracker exceeds
    // the number of reads before finishing the trial run, it means that the
    // cached plan isn't performing as well as it used to and we'll need to replan, so we let the
    // tracker terminate the trial. If the tracker reaches 'maxTrialResultsFromPlanningRoot'
    // planning results, the trial run finishes and the plan is used to complete the query.
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
    candidate.data.tracker = std::make_unique<TrialRunTracker>(
        std::move(onMetricReached), maxTrialResultsFromPlanningRoot, maxTrialPeriodNumReads);
    candidate.root->attachToTrialRunTracker(
        candidate.data.tracker.get(),
        candidate.data.stageData.staticData->runtimePlanningRootNodeId);

    sbe::TrialRuntimeExecutor{
        plannerData.opCtx,
        plannerData.collections,
        *plannerData.cq,
        plannerData.sbeYieldPolicy.get(),
        indexExistenceChecker,
        static_cast<size_t>(internalQuerySBEPlanEvaluationMaxMemoryBytes.load())}
        .executeCachedCandidateTrial(&candidate, maxTrialResults);

    return candidate;
}

// TODO SERVER-87466 Trigger replanning by throwing an exception, instead of creating another
// planner.
std::unique_ptr<PlannerInterface> replan(
    PlannerDataForSBE plannerData,
    const AllIndicesRequiredChecker& indexExistenceChecker,
    std::string replanReason,
    bool shouldCache,
    const std::function<void()>& incrementReplanCounterCb,
    const std::function<void()>& incrementReplannedPlanIsCachedPlanCounterCb) {
    incrementReplanCounterCb();

    // The trial run might have allowed DDL commands to be executed during yields. Check if the
    // provided planner parameters still match the current view of the index catalog.
    indexExistenceChecker.check(plannerData.opCtx, plannerData.collections);

    // The plan drawn from the cache is being discarded, and should no longer be
    // registered with the yield policy.
    plannerData.sbeYieldPolicy->clearRegisteredPlans();

    // Use the query planning module to plan the whole query.
    auto solutions =
        uassertStatusOK(QueryPlanner::plan(*plannerData.cq, *plannerData.plannerParams));

    // There's a single solution, there's a special planner for just this case.
    if (solutions.size() == 1) {
        LOGV2_DEBUG(8523804,
                    1,
                    "Replanning of query resulted in a single query solution",
                    "query"_attr = redact(plannerData.cq->toStringShort()),
                    "shouldCache"_attr = (shouldCache ? "yes" : "no"));
        return std::make_unique<SingleSolutionPassthroughPlanner>(
            std::move(plannerData), std::move(solutions[0]), std::move(replanReason));
    }

    // Multiple solutions. Resort to multiplanning.
    LOGV2_DEBUG(8523805,
                1,
                "Query plan after replanning and its cache status",
                "query"_attr = redact(plannerData.cq->toStringShort()),
                "shouldCache"_attr = (shouldCache ? "yes" : "no"));
    return std::make_unique<MultiPlanner>(std::move(plannerData),
                                          std::move(solutions),
                                          shouldCache,
                                          incrementReplannedPlanIsCachedPlanCounterCb,
                                          std::move(replanReason));
}

std::unique_ptr<PlannerInterface> attemptToUsePlan(
    PlannerDataForSBE plannerData,
    boost::optional<size_t> decisionReads,
    std::unique_ptr<sbe::PlanStage> sbePlan,
    stage_builder::PlanStageData planStageData,
    std::unique_ptr<QuerySolution> solution,
    const AllIndicesRequiredChecker& indexExistenceChecker,
    const std::function<void(const PlannerData&)>& deactivateCb,
    const std::function<void()>& incrementReplanCounterCb,
    const std::function<void()>& incrementReplannedPlanIsCachedPlanCounterCb) {
    const bool isPinnedCacheEntry = !decisionReads.has_value();
    if (isPinnedCacheEntry) {
        auto sbePlanAndData = std::make_pair(std::move(sbePlan), std::move(planStageData));
        return std::make_unique<SingleSolutionPassthroughPlanner>(std::move(plannerData),
                                                                  std::move(sbePlanAndData));
    }

    const size_t maxReadsBeforeReplan = internalQueryCacheEvictionRatio.load() * *decisionReads;
    auto candidate = collectExecutionStatsForCachedPlan(plannerData,
                                                        std::move(solution),
                                                        indexExistenceChecker,
                                                        std::move(sbePlan),
                                                        std::move(planStageData),
                                                        maxReadsBeforeReplan);

    auto getPlanSummary = [&]() {
        return plan_cache_util::buildDebugInfo(candidate.solution.get()).planSummary;
    };

    if (!candidate.status.isOK()) {
        // On failure, fall back to replanning the whole query. We neither evict the existing cache
        // entry, nor cache the result of replanning.
        LOGV2_DEBUG(8523802,
                    1,
                    "Execution of cached plan failed, falling back to replan",
                    "query"_attr = redact(plannerData.cq->toStringShort()),
                    "planSummary"_attr = getPlanSummary(),
                    "error"_attr = candidate.status.toString());
        std::string replanReason = str::stream() << "cached plan returned: " << candidate.status;
        recoverWhereExpression(plannerData.cq, std::move(candidate));
        return replan(std::move(plannerData),
                      indexExistenceChecker,
                      std::move(replanReason),
                      /* shouldCache */ false,
                      incrementReplanCounterCb,
                      incrementReplannedPlanIsCachedPlanCounterCb);
    }

    if (candidate.exitedEarly) {
        // The trial period took more than 'maxReadsBeforeReplan' physical reads. This plan may not
        // be efficient any longer, so we replan from scratch.
        auto numReads =
            candidate.data.tracker->getMetric<TrialRunTracker::TrialRunMetric::kNumReads>();
        LOGV2_DEBUG(
            8523803,
            1,
            "Evicting cache entry for a query and replanning it since the number of required reads "
            "mismatch the number of cached reads",
            "maxReadsBeforeReplan"_attr = maxReadsBeforeReplan,
            "decisionReads"_attr = decisionReads,
            "numReads"_attr = numReads,
            "query"_attr = redact(plannerData.cq->toStringShort()),
            "planSummary"_attr = getPlanSummary());

        deactivateCb(plannerData);

        std::string replanReason = str::stream()
            << "cached plan was less efficient than expected: expected trial execution to take "
            << decisionReads << " reads but it took at least " << numReads << " reads";
        recoverWhereExpression(plannerData.cq, std::move(candidate));
        return replan(std::move(plannerData),
                      indexExistenceChecker,
                      std::move(replanReason),
                      /* shouldCache */ true,
                      incrementReplanCounterCb,
                      incrementReplannedPlanIsCachedPlanCounterCb);
    }

    // If the trial run did not exit early, it means no replanning is necessary and can return this
    // candidate to the executor. All results generated during the trial are stored with the
    // candidate so that the executor will be able to reuse them.
    return std::make_unique<ValidCandidatePlanner>(std::move(plannerData), std::move(candidate));
}
}  // namespace

PlannerGeneratorFromClassicCacheEntry::PlannerGeneratorFromClassicCacheEntry(
    PlannerDataForSBE plannerDataArg,
    std::unique_ptr<QuerySolution> solution,
    boost::optional<size_t> decisionReads)
    : _plannerData(std::move(plannerDataArg)),
      _solution(std::move(solution)),
      _decisionReads(decisionReads) {
    LOGV2_DEBUG(8523445,
                5,
                "Building SBE plan from the classic plan cache",
                "decisionReads"_attr = decisionReads);

    // After retrieving a classic cache entry, we have a QSN tree for the winning plan. Now we
    // lower it to SBE.
    std::tie(_sbePlan, _planStageData) =
        stage_builder::buildSlotBasedExecutableTree(_plannerData.opCtx,
                                                    _plannerData.collections,
                                                    *_plannerData.cq,
                                                    *_solution,
                                                    _plannerData.sbeYieldPolicy.get());
}

std::unique_ptr<PlannerInterface> PlannerGeneratorFromClassicCacheEntry::makePlanner() {
    // Note that planStageData.debugInfo is not set here. This will be set when an explainer is
    // created.
    AllIndicesRequiredChecker indexExistenceChecker{_plannerData.collections};
    auto deactivateEntry = [](const PlannerData& plannerData) {
        auto& collection = plannerData.collections.getMainCollection();
        auto classicCacheKey =
            plan_cache_key_factory::make<PlanCacheKey>(*plannerData.cq, collection);
        auto classicPlanCache = CollectionQueryInfo::get(collection).getPlanCache();
        size_t evictedCount = classicPlanCache->deactivate(classicCacheKey);
        planCacheCounters.incrementClassicCachedPlansEvictedCounter(evictedCount);
    };

    return attemptToUsePlan(
        std::move(_plannerData),
        _decisionReads,
        std::move(_sbePlan),
        std::move(*_planStageData),
        std::move(_solution),
        indexExistenceChecker,
        deactivateEntry,
        []() { planCacheCounters.incrementClassicReplannedCounter(); },
        []() { planCacheCounters.incrementClassicReplannedPlanIsCachedPlanCounter(); });
}

std::unique_ptr<PlannerInterface> PlannerGeneratorFromSbeCacheEntry::makePlanner() {
    AllIndicesRequiredChecker indexExistenceChecker{_plannerData.collections};
    const boost::optional<size_t> decisionReads = _cachedPlanHolder->decisionReads();
    auto sbePlan = std::move(_cachedPlanHolder->cachedPlan->root);
    auto planStageData = std::move(_cachedPlanHolder->cachedPlan->planStageData);
    planStageData.debugInfo = _cachedPlanHolder->debugInfo;

    LOGV2_DEBUG(8523404,
                5,
                "Recovering SBE plan from the SBE plan cache",
                "decisionReads"_attr = decisionReads);

    const auto& foreignHashJoinCollections = planStageData.staticData->foreignHashJoinCollections;
    if (!_plannerData.cq->cqPipeline().empty() && !foreignHashJoinCollections.empty()) {
        // We'd like to check if there is any foreign collection in the hash_lookup stage
        // that is no longer eligible for using a hash_lookup plan. In this case we
        // invalidate the cache and immediately replan without ever running a trial period.
        const auto& secondaryCollectionsInfo = _plannerData.plannerParams->secondaryCollectionsInfo;

        for (const auto& foreignCollection : foreignHashJoinCollections) {
            const auto collectionInfo = secondaryCollectionsInfo.find(foreignCollection);
            tassert(8832902,
                    "Foreign collection must be present in the collections info",
                    collectionInfo != secondaryCollectionsInfo.end());
            tassert(8832901, "Foreign collection must exist", collectionInfo->second.exists);

            if (!QueryPlannerAnalysis::isEligibleForHashJoin(collectionInfo->second)) {
                return replan(
                    std::move(_plannerData),
                    indexExistenceChecker,
                    str::stream() << "Foreign collection "
                                  << foreignCollection.toStringForErrorMsg()
                                  << " is not eligible for hash join anymore",
                    /* shouldCache */ true,
                    []() { planCacheCounters.incrementSbeReplannedCounter(); },
                    []() { planCacheCounters.incrementSbeReplannedPlanIsCachedPlanCounter(); });
            }
        }
    }

    auto deactivateEntry = [](const PlannerData& plannerData) {
        // Deactivate the current cache entry.
        auto& sbePlanCache = sbe::getPlanCache(plannerData.opCtx);
        size_t evictedCount = sbePlanCache.deactivate(
            plan_cache_key_factory::make(*plannerData.cq, plannerData.collections));
        planCacheCounters.incrementSbeCachedPlansEvictedCounter(evictedCount);
    };

    return attemptToUsePlan(
        std::move(_plannerData),
        decisionReads,
        std::move(sbePlan),
        std::move(planStageData),
        nullptr, /* solution */
        indexExistenceChecker,
        deactivateEntry,
        []() { planCacheCounters.incrementSbeReplannedCounter(); },
        []() { planCacheCounters.incrementSbeReplannedPlanIsCachedPlanCounter(); });
}

std::unique_ptr<PlannerInterface> makePlannerForSbeCacheEntry(
    PlannerDataForSBE plannerData,
    std::unique_ptr<sbe::CachedPlanHolder> cachedPlanHolder,
    const size_t cachedSolutionHash) {
    plannerData.cachedPlanSolutionHash = cachedSolutionHash;
    PlannerGeneratorFromSbeCacheEntry generator(std::move(plannerData),
                                                std::move(cachedPlanHolder));
    return generator.makePlanner();
}

std::unique_ptr<PlannerInterface> makePlannerForClassicCacheEntry(
    PlannerDataForSBE plannerData,
    std::unique_ptr<QuerySolution> solution,
    const size_t cachedSolutionHash,
    boost::optional<size_t> decisionReads) {
    plannerData.cachedPlanSolutionHash = cachedSolutionHash;
    PlannerGeneratorFromClassicCacheEntry generator(
        std::move(plannerData), std::move(solution), decisionReads);
    return generator.makePlanner();
}
}  // namespace mongo::classic_runtime_planner_for_sbe
