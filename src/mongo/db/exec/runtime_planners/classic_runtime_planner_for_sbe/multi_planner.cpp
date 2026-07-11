// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/curop.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner_for_sbe/planner_interface.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_ranking/plan_ranker_method.h"
#include "mongo/db/query/plan_yield_policy_impl.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"
#include "mongo/logv2/log.h"

#include <absl/functional/bind_front.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::classic_runtime_planner_for_sbe {

MultiPlanner::MultiPlanner(PlannerDataForSBE plannerData,
                           std::vector<std::unique_ptr<QuerySolution>> candidatePlans,
                           bool shouldWriteToPlanCache,
                           const std::function<void()>& incrementReplannedPlanIsCachedPlanCounterCb,
                           boost::optional<std::string> replanReason)
    : PlannerBase(std::move(plannerData)),
      _shouldWriteToPlanCache(shouldWriteToPlanCache),
      _incrementReplannedPlanIsCachedPlanCounterCb(incrementReplannedPlanIsCachedPlanCounterCb),
      _replanReason(replanReason) {
    LOGV2_DEBUG(
        6215001, 5, "Using classic multi-planner for SBE", "replanReason"_attr = _replanReason);

    // Set up the callback that 'MultiPlanStage' will call to write to the SBE plan cache.
    auto planCacheWriter = absl::bind_front(&MultiPlanner::_buildSbePlanAndMaybeCache, this);

    _multiPlanStage =
        std::make_unique<MultiPlanStage>(cq()->getExpCtxRaw(),
                                         collections().getMainCollectionPtrOrAcquisition(),
                                         cq(),
                                         std::move(planCacheWriter),
                                         replanReason);
    for (auto&& solution : candidatePlans) {
        auto nextPlanRoot = stage_builder::buildClassicExecutableTree(
            opCtx(), collections().getMainCollectionPtrOrAcquisition(), *cq(), *solution, ws());
        _multiPlanStage->addPlan(std::move(solution), std::move(nextPlanRoot), ws());
    }
    auto trialPeriodYieldPolicy = makeClassicYieldPolicy(
        opCtx(), cq()->nss(), static_cast<PlanStage*>(_multiPlanStage.get()), yieldPolicy());
    uassertStatusOK(_multiPlanStage->runTrials(trialPeriodYieldPolicy.get()));
    uassertStatusOK(_multiPlanStage->pickBestPlan());
    CurOp::get(opCtx())->debug().planRankerMethod = PlanRankerMethod::kMultiPlanner;
}

const MultiPlanStats* MultiPlanner::getSpecificStats() const {
    return static_cast<const MultiPlanStats*>(_multiPlanStage->getSpecificStats());
}

std::unique_ptr<QuerySolution> MultiPlanner::extractQuerySolution() {
    return _multiPlanStage->extractBestSolution();
};

Status MultiPlanner::runTrials(trial_period::TrialPhaseConfig trialConfig) {
    auto trialPeriodYieldPolicy = makeClassicYieldPolicy(
        opCtx(), cq()->nss(), static_cast<PlanStage*>(_multiPlanStage.get()), yieldPolicy());
    return _multiPlanStage->runTrials(trialPeriodYieldPolicy.get(), trialConfig);
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> MultiPlanner::makeExecutor(
    std::unique_ptr<CanonicalQuery> canonicalQuery) {
    // We should have already constructed the full SBE plan in order to write it to the SBE plan
    // cache.
    invariant(_sbePlanAndData);

    if (_shouldUseEofOptimization()) {
        auto nss = cq()->nss();
        // Return a classic plan executor which will unspool the data buffered during
        // multi-planning.
        return plan_executor_factory::make(std::move(canonicalQuery),
                                           extractWs(),
                                           std::move(_multiPlanStage),
                                           collections().getMainCollectionAcquisition(),
                                           yieldPolicy(),
                                           plannerOptions(),
                                           std::move(nss),
                                           nullptr /* querySolution */,
                                           cachedPlanHash());
    }

    // The winning plan did not reach EOF during the trial period, or we were otherwise unable
    // to use the EOF optimization. Construct an SBE plan executor.
    //
    // When the EOF optimization is not in use, we should own the 'QuerySolution'. In contrast,
    // when the EOF optimization is used, MultiPlanStage retains ownership of the winning
    // solution.
    invariant(_winningSolution);
    return prepareSbePlanExecutor(std::move(canonicalQuery),
                                  std::move(_winningSolution),
                                  std::move(*_sbePlanAndData),
                                  false /*isFromPlanCache*/,
                                  cachedPlanHash(),
                                  std::move(_multiPlanStage));
}

bool MultiPlanner::_shouldUseEofOptimization() const {
    return _multiPlanStage->bestSolutionEof() &&
        // We show SBE plan in explain.
        !cq()->getExpCtxRaw()->getExplain() &&
        // We can't use EOF optimization if pipeline is present. Because we need to execute the
        // pipeline part in SBE, we have to rebuild and rerun the whole query.
        cq()->cqPipeline().empty() &&
        // We want more coverage for SBE in debug builds.
        !kDebugBuild;
}

void MultiPlanner::_buildSbePlanAndMaybeCache(
    const CanonicalQuery& queryToCache,
    MultiPlanStage& mps,
    std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
    std::vector<plan_ranker::CandidatePlan>& candidates) {
    invariant(queryToCache.isSbeCompatible());
    // Note: 'queryToCache' is _not_ necessarily the same as the canonical query available in
    // cq(). This is because subplanning will plan (and cache) each branch of the top-level OR
    // in a query separately, and invoke this callback with each branch as if it were an
    // independent top-level query.

    boost::optional<PlanCacheDecisionMetrics> planCacheDecisionMetrics;
    if (_shouldWriteToPlanCache) {
        auto stats = _multiPlanStage->getStats();
        planCacheDecisionMetrics.emplace(
            plan_cache_util::computeNumReadsFromStats(*stats, *ranking),
            plan_cache_util::computeNumWorksFromStats(*ranking));
    }

    // If classic plan cache is enabled, write to it. We need to do this before we extend the
    // QSN tree with the agg pipeline, since the agg portion does not get cached in classic.
    if (_shouldWriteToPlanCache) {
        plan_cache_util::updateClassicPlanCacheFromClassicCandidates(
            opCtx(),
            collections().getMainCollectionAcquisition(),
            queryToCache,
            *planCacheDecisionMetrics,
            std::move(ranking),
            candidates);
    }

    // Pointer to the query solution which should be used to construct the SBE plan cache entry.
    const QuerySolution* solnToCache = _multiPlanStage->bestSolution();

    if (!_shouldUseEofOptimization()) {
        // Extend the winning solution with the agg pipeline. If using the EOF optimisation,
        // plan_executor_factory::make(...) may later require the best solution to still be
        // owned by
        // `_multiPlanStage` (not yet extracted).
        _winningSolution = extendSolutionWithPipeline(_multiPlanStage->extractBestSolution());
        solnToCache = _winningSolution.get();
    }

    tassert(10221200, "solnToCache must be non-null.", solnToCache);

    if (cachedPlanSolutionHash() && *cachedPlanSolutionHash() == solnToCache->hash()) {
        _incrementReplannedPlanIsCachedPlanCounterCb();
    }

    _sbePlanAndData = prepareSbePlanAndData(*solnToCache, std::move(_replanReason));
}

}  // namespace mongo::classic_runtime_planner_for_sbe
