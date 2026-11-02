// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner_for_sbe/planner_interface.h"
#include "mongo/db/exec/trial_period_utils.h"
#include "mongo/db/query/all_indices_required_checker.h"
#include "mongo/db/query/bind_input_params.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_ranking/plan_selection_strategy.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/query/replanning_required_info.h"
#include "mongo/db/query/sbe_trial_runtime_executor.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"
#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

MONGO_FAIL_POINT_DEFINE(planCacheAlwaysReplanSBE);

namespace mongo::classic_runtime_planner_for_sbe {

namespace {
class ValidCandidatePlanner : public PlannerBase {
public:
    ValidCandidatePlanner(PlannerDataForSBE plannerData, sbe::plan_ranker::CandidatePlan candidate)
        : PlannerBase(std::move(plannerData), PlanSelectionStrategy::kCachedPlan),
          _candidate(std::move(candidate)) {}

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExecutor(
        std::unique_ptr<CanonicalQuery> canonicalQuery) override {
        auto nss = cq()->nss();
        auto remoteCursors = cq()->getExpCtx()->getExplain()
            ? nullptr
            : search_helpers::getSearchRemoteCursors(cq()->cqPipeline());
        auto remoteExplains = cq()->getExpCtx()->getExplain()
            ? search_helpers::getSearchRemoteExplains(cq()->getExpCtxRaw(), cq()->cqPipeline())
            : nullptr;
        return plan_executor_factory::make(opCtx(),
                                           std::move(canonicalQuery),
                                           std::move(_candidate),
                                           collections(),
                                           plannerOptions(),
                                           std::move(nss),
                                           extractSbeYieldPolicy(),
                                           std::move(remoteCursors),
                                           std::move(remoteExplains),
                                           /*cachedPlanHash=*/boost::none,
                                           planSelectionStrategy());
    }


    PlanRankingResult extractPlanRankingResult() override {
        std::vector<std::unique_ptr<QuerySolution>> v;
        v.push_back(std::move(_candidate.solution));
        return PlanRankingResult{
            .solutions = std::move(v),
            .maybeExplainData = PlanExplainerData{.fromPlanCache = true},
            .execState = SavedExecState{SbeExecState{.sbeCandidate = std::move(_candidate),
                                                     .sbeYieldPolicy = extractSbeYieldPolicy()}},
            .plannerParams = extractPlannerParams(),
            .cachedPlanHash = cachedPlanHash(),
            .engineSelection = EngineChoice::kSbe,
            .planSelectionStrategy = planSelectionStrategy()};
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

    sbe::plan_ranker::CandidatePlan candidate{
        .solution = std::move(solution),
        .root = std::move(root),
        .data = sbe::plan_ranker::CandidatePlanData{std::move(data)},
        .status = Status::OK(),
        .fromPlanCache = true};
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
    candidate.data.tracker = std::make_unique<TrialRunTracker>(std::move(onMetricReached),
                                                               maxTrialResultsFromPlanningRoot,
                                                               maxTrialPeriodNumReads,
                                                               boost::none,
                                                               boost::none);
    candidate.root->attachToTrialRunTracker(
        candidate.data.tracker.get(),
        candidate.data.stageData.staticData->runtimePlanningRootNodeId);

    sbe::TrialRuntimeExecutor{plannerData.opCtx,
                              plannerData.collections,
                              *plannerData.cq,
                              plannerData.sbeYieldPolicy.get(),
                              indexExistenceChecker,
                              static_cast<size_t>(plannerData.cq->getExpCtx()
                                                      ->getQueryKnobConfiguration()
                                                      .getSbePlanEvaluationMaxMemoryBytes())}
        .executeCachedCandidateTrial(&candidate, maxTrialResults);

    return candidate;
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
    tassert(11903601,
            "Expected classic plan cache entry to have decision reads",
            decisionReads.has_value());

    size_t maxReadsBeforeReplan = internalQueryCacheEvictionRatio.load() * *decisionReads;

    // When the failpoint is active, force the cached plan to always be replanned by setting the
    // threshold to zero.
    if (MONGO_unlikely(planCacheAlwaysReplanSBE.shouldFail())) {
        maxReadsBeforeReplan = 0;
    }
    auto candidate = collectExecutionStatsForCachedPlan(plannerData,
                                                        std::move(solution),
                                                        indexExistenceChecker,
                                                        std::move(sbePlan),
                                                        std::move(planStageData),
                                                        maxReadsBeforeReplan);

    auto getPlanSummary = [&]() {
        return plan_cache_util::buildDebugInfo(plannerData.cq->nss(), candidate.solution.get())
            .planSummary;
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

        // TODO SERVER-119036 update comment and simplify this function.
        // If we're using the deferred get_executor, we throw an exception which is caught be a
        // higher level replanning try/catch, and will reuse the top-level planning path. If
        // we're using the regular get_executor, this counter is incremented in `replan`.
        incrementReplanCounterCb();
        uasserted(ReplanningRequiredInfo(plan_cache_util::CacheMode::NeverCache,
                                         *plannerData.cachedPlanSolutionHash),
                  std::move(replanReason));
    } else if (candidate.stopCondition == MultiPlannerStopCondition::kExhaustedBudget) {
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

        // TODO SERVER-119036 update comment and simplify this function.
        incrementReplanCounterCb();
        uasserted(ReplanningRequiredInfo(plan_cache_util::CacheMode::AlwaysCache,
                                         *plannerData.cachedPlanSolutionHash),
                  std::move(replanReason));
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
        auto& collection = plannerData.collections.getMainCollectionAcquisition();
        auto classicCacheKey =
            plan_cache_key_factory::make<PlanCacheKey>(*plannerData.cq, collection);
        auto classicPlanCache =
            CollectionQueryInfo::get(collection.getCollectionPtr()).getPlanCache();
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
