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

#include "mongo/db/exec/runtime_planners/classic_runtime_planner_for_sbe/planner_interface.h"
#include "mongo/db/query/plan_executor_factory.h"
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

    auto trialPeriodYieldPolicy =
        makeClassicYieldPolicy(opCtx(),
                               cq()->nss(),
                               static_cast<PlanStage*>(_multiPlanStage.get()),
                               yieldPolicy(),
                               collections().getMainCollectionPtrOrAcquisition());
    uassertStatusOK(_multiPlanStage->pickBestPlan(trialPeriodYieldPolicy.get()));
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
        return uassertStatusOK(
            plan_executor_factory::make(std::move(canonicalQuery),
                                        extractWs(),
                                        std::move(_multiPlanStage),
                                        collections().getMainCollectionPtrOrAcquisition(),
                                        yieldPolicy(),
                                        plannerOptions(),
                                        std::move(nss),
                                        nullptr /* querySolution */,
                                        cachedPlanHash()));
    }

    // The winning plan did not reach EOF during the trial period, or we were otherwise unable to
    // use the EOF optimization. Construct an SBE plan executor.
    //
    // When the EOF optimization is not in use, we should own the 'QuerySolution'. In contrast, when
    // the EOF optimization is used, MultiPlanStage retains ownership of the winning solution.
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
    // Note: 'queryToCache' is _not_ necessarily the same as the canonical query available in cq().
    // This is because subplanning will plan (and cache) each branch of the top-level OR in a query
    // separately, and invoke this callback with each branch as if it were an independent top-level
    // query.

    boost::optional<NumReads> numReads;
    if (_shouldWriteToPlanCache) {
        auto stats = _multiPlanStage->getStats();
        numReads = plan_cache_util::computeNumReadsFromStats(*stats, *ranking);
    }

    // If classic plan cache is enabled, write to it. We need to do this before we extend the QSN
    // tree with the agg pipeline, since the agg portion does not get cached in classic.
    if (_shouldWriteToPlanCache && !useSbePlanCache()) {
        plan_cache_util::updateClassicPlanCacheFromClassicCandidatesForSbeExecution(
            opCtx(),
            collections().getMainCollection(),
            queryToCache,
            *numReads,
            std::move(ranking),
            candidates);
    }

    // Pointer to the query solution which should be used to construct the SBE plan cache entry.
    const QuerySolution* solnToCache = _multiPlanStage->bestSolution();

    if (!_shouldUseEofOptimization()) {
        // Extend the winning solution with the agg pipeline. If using the EOF optimisation,
        // plan_executor_factory::make(...) may later require the best solution to still be owned by
        // `_multiPlanStage` (not yet extracted).
        _winningSolution = extendSolutionWithPipeline(_multiPlanStage->extractBestSolution());
        solnToCache = _winningSolution.get();
    }

    tassert(10221200, "solnToCache must be non-null.", solnToCache);

    if (cachedPlanSolutionHash() && *cachedPlanSolutionHash() == solnToCache->hash()) {
        _incrementReplannedPlanIsCachedPlanCounterCb();
    }

    _sbePlanAndData = prepareSbePlanAndData(*solnToCache, std::move(_replanReason));
    if (_shouldWriteToPlanCache && useSbePlanCache()) {
        plan_cache_util::updateSbePlanCacheWithNumReads(
            opCtx(), collections(), queryToCache, *numReads, *_sbePlanAndData, solnToCache);
    }
}

}  // namespace mongo::classic_runtime_planner_for_sbe
