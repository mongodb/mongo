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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <deque>
#include <string>
#include <variant>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/exec/histogram_server_status_metric.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/trial_period_utils.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/query/plan_cache_debug_info.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/plan_ranker_util.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/sbe_multi_planner.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/stage_builder_util.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_proxy.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"
#include "mongo/util/tick_source.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::sbe {
namespace {
/**
 * An element in this histogram is the number of plans in the candidate set of an invocation (of the
 * SBE multiplanner).
 */
auto& sbeNumPlansHistogram =
    *MetricBuilder<HistogramServerStatusMetric>{"query.multiPlanner.histograms.sbeNumPlans"}.bind(
        HistogramServerStatusMetric::pow(5, 2, 2));

/**
 * Aggregation of the total number of invocations (of the SBE multiplanner).
 */
auto& sbeCount = *MetricBuilder<Counter64>{"query.multiPlanner.sbeCount"};

/**
 * Aggregation of the total number of microseconds spent (in SBE multiplanner).
 */
auto& sbeMicrosTotal = *MetricBuilder<Counter64>{"query.multiPlanner.sbeMicros"};

/**
 * Aggregation of the total number of reads done (in SBE multiplanner).
 */
auto& sbeNumReadsTotal = *MetricBuilder<Counter64>{"query.multiPlanner.sbeNumReads"};

/**
 * An element in this histogram is the number of microseconds spent in an invocation (of the SBE
 * multiplanner).
 */
auto& sbeMicrosHistogram =
    *MetricBuilder<HistogramServerStatusMetric>{"query.multiPlanner.histograms.sbeMicros"}.bind(
        HistogramServerStatusMetric::pow(11, 1024, 4));

/**
 * An element in this histogram is the number of reads performance during an invocation (of the SBE
 * multiplanner).
 */
auto& sbeNumReadsHistogram =
    *MetricBuilder<HistogramServerStatusMetric>{"query.multiPlanner.histograms.sbeNumReads"}.bind(
        HistogramServerStatusMetric::pow(9, 128, 2));
}  // namespace

CandidatePlans MultiPlanner::plan(
    const QueryPlannerParams& plannerParams,
    std::vector<std::unique_ptr<QuerySolution>> solutions,
    std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots) {
    auto candidates = collectExecutionStats(
        std::move(solutions),
        std::move(roots),
        trial_period::getTrialPeriodMaxWorks(_opCtx,
                                             _collections.getMainCollection(),
                                             internalQueryPlanEvaluationWorksSbe.load(),
                                             internalQueryPlanEvaluationCollFractionSbe.load()));
    auto decision = uassertStatusOK(mongo::plan_ranker::pickBestPlan<PlanStageStats>(candidates));
    return finalizeExecutionPlans(plannerParams, std::move(decision), std::move(candidates));
}

bool MultiPlanner::CandidateCmp::operator()(const plan_ranker::CandidatePlan* lhs,
                                            const plan_ranker::CandidatePlan* rhs) const {
    size_t lhsReads = lhs->data.tracker->getMetric<TrialRunTracker::TrialRunMetric::kNumReads>();
    size_t rhsReads = rhs->data.tracker->getMetric<TrialRunTracker::TrialRunMetric::kNumReads>();
    auto lhsProductivity = plan_ranker::calculateProductivity(lhs->results.size(), lhsReads);
    auto rhsProductivity = plan_ranker::calculateProductivity(rhs->results.size(), rhsReads);
    return lhsProductivity < rhsProductivity;
}

MultiPlanner::PlanQ MultiPlanner::preparePlans(
    const std::vector<size_t>& planIndexes,
    const size_t trackerResultsBudget,
    std::vector<std::unique_ptr<QuerySolution>>& solutions,
    std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>>& roots) {
    PlanQ planq;
    for (auto planIndex : planIndexes) {
        auto&& [root, stageData] = roots[planIndex];
        // Make a copy of the original plan. This pristine copy will be inserted into the plan
        // cache if this candidate becomes the winner.
        auto origPlan = std::make_pair<std::unique_ptr<PlanStage>, plan_ranker::CandidatePlanData>(
            root->clone(), plan_ranker::CandidatePlanData{stageData});

        // Attach a unique TrialRunTracker to the plan, which is configured to use at most
        // '_maxNumReads' reads.
        auto tracker = std::make_unique<TrialRunTracker>(trackerResultsBudget, _maxNumReads);
        root->attachToTrialRunTracker(tracker.get());

        plan_ranker::CandidatePlanData data = {std::move(stageData), std::move(tracker)};
        _candidates.push_back({std::move(solutions[planIndex]),
                               std::move(root),
                               std::move(data),
                               false /* exitedEarly */,
                               Status::OK()});
        auto* candidatePtr = &_candidates.back();
        // Store the original plan in the CandidatePlan.
        candidatePtr->clonedPlan.emplace(std::move(origPlan));
        _trialRuntimeExecutor.prepareCandidate(candidatePtr, false /*preparingFromCache*/);
        if (fetchOneDocument(candidatePtr)) {
            planq.push(candidatePtr);
        }
    }
    return planq;
}

void MultiPlanner::trialPlans(PlanQ planq) {
    while (!planq.empty()) {
        plan_ranker::CandidatePlan* bestCandidate = planq.top();
        planq.pop();
        bestCandidate->data.tracker->updateMaxMetric<TrialRunTracker::TrialRunMetric::kNumReads>(
            _maxNumReads);
        if (fetchOneDocument(bestCandidate)) {
            planq.push(bestCandidate);
        }
    }
}

bool MultiPlanner::fetchOneDocument(plan_ranker::CandidatePlan* candidate) {
    if (!_trialRuntimeExecutor.fetchNextDocument(candidate, _maxNumResults)) {
        candidate->root->detachFromTrialRunTracker();
        if (candidate->status.isOK()) {
            auto numReads =
                candidate->data.tracker->getMetric<TrialRunTracker::TrialRunMetric::kNumReads>();
            // In the case of number of read of the plan is 0, we don't want to set _maxNumReads to
            // 0 for the following plans, because that will effectively disable the max read bound.
            // Instead we set a hard limit of 1 _maxNumReads here.
            _maxNumReads = std::max(static_cast<size_t>(1), std::min(_maxNumReads, numReads));
        }
        return false;
    }
    return true;
}

std::vector<plan_ranker::CandidatePlan> MultiPlanner::collectExecutionStats(
    std::vector<std::unique_ptr<QuerySolution>> solutions,
    std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots,
    size_t maxTrialPeriodNumReads) {
    invariant(solutions.size() == roots.size());

    _maxNumResults = trial_period::getTrialPeriodNumToReturn(_cq);
    _maxNumReads = maxTrialPeriodNumReads;

    auto tickSource = _opCtx->getServiceContext()->getTickSource();
    auto startTicks = tickSource->getTicks();
    sbeNumPlansHistogram.increment(solutions.size());
    sbeCount.increment();

    // Determine which plans are blocking and which are non blocking. The non blocking plans will
    // be run first in order to provide an upper bound on the number of reads allowed for the
    // blocking plans.
    std::vector<size_t> nonBlockingPlanIndexes;
    std::vector<size_t> blockingPlanIndexes;
    for (size_t index = 0; index < solutions.size(); ++index) {
        if (solutions[index]->hasBlockingStage) {
            blockingPlanIndexes.push_back(index);
        } else {
            nonBlockingPlanIndexes.push_back(index);
        }
    }

    // If all the plans are blocking, then the trial period risks going on for too long. Because the
    // plans are blocking, they may not provide '_maxNumResults' within the allotted budget of
    // reads. We could end up in a situation where each plan's trial period runs for a long time,
    // substantially slowing down the multi-planning process. For this reason, when all the plans
    // are blocking, we pass '_maxNumResults' to the trial run tracker. This causes the sort stage
    // to exit early as soon as it sees '_maxNumResults' _input_ values, which keeps the trial
    // period shorter.
    //
    // On the other hand, if we have a mix of blocking and non-blocking plans, we don't want the
    // sort stage to exit early based on the number of input rows it observes. This could cause the
    // trial period for the blocking plans to run for a much shorter timeframe than the non-blocking
    // plans. This leads to an apples-to-oranges comparison between the blocking and non-blocking
    // plans which could artificially favor the blocking plans.
    const size_t trackerResultsBudget = nonBlockingPlanIndexes.empty() ? _maxNumResults : 0;

    // Reserve space for the candidates to avoid reallocations and have stable pointers to vector's
    // elements.
    _candidates.reserve(solutions.size());
    // Run the non-blocking plans first.
    trialPlans(preparePlans(nonBlockingPlanIndexes, trackerResultsBudget, solutions, roots));
    // Run the blocking plans.
    trialPlans(preparePlans(blockingPlanIndexes, trackerResultsBudget, solutions, roots));

    size_t totalNumReads = 0;
    for (const auto& candidate : _candidates) {
        totalNumReads +=
            candidate.data.tracker->getMetric<TrialRunTracker::TrialRunMetric::kNumReads>();
    }
    sbeNumReadsHistogram.increment(totalNumReads);
    sbeNumReadsTotal.increment(totalNumReads);

    auto durationMicros = durationCount<Microseconds>(
        tickSource->ticksTo<Microseconds>(tickSource->getTicks() - startTicks));
    sbeMicrosHistogram.increment(durationMicros);
    sbeMicrosTotal.increment(durationMicros);

    return std::move(_candidates);
}

CandidatePlans MultiPlanner::finalizeExecutionPlans(
    const QueryPlannerParams& plannerParams,
    std::unique_ptr<mongo::plan_ranker::PlanRankingDecision> decision,
    std::vector<plan_ranker::CandidatePlan> candidates) const {
    invariant(decision);

    // Make sure we have at least one plan which hasn't failed.
    uassert(4822873,
            "all candidate plans failed during multi planning",
            std::count_if(candidates.begin(), candidates.end(), [](auto&& candidate) {
                return candidate.status.isOK();
            }) > 0);

    auto&& stats = decision->getStats<sbe::PlanStageStats>();

    const auto winnerIdx = decision->candidateOrder[0];
    tassert(5323801,
            str::stream() << "winner index is out of candidate plans bounds: " << winnerIdx << ", "
                          << candidates.size(),
            winnerIdx < candidates.size());
    tassert(5323802,
            str::stream() << "winner index is out of candidate plan stats bounds: " << winnerIdx
                          << ", " << stats.candidatePlanStats.size(),
            winnerIdx < stats.candidatePlanStats.size());

    auto& winner = candidates[winnerIdx];
    tassert(5323803,
            str::stream() << "winning candidate returned an error: " << winner.status,
            winner.status.isOK());

    LOGV2_DEBUG(4822875,
                5,
                "Winning solution",
                "bestSolution"_attr = redact(winner.solution->toString()),
                "bestSolutionHash"_attr = winner.solution->hash());

    auto explainer = plan_explainer_factory::make(
        winner.root.get(), &winner.data.stageData, winner.solution.get());
    LOGV2_DEBUG(4822876, 2, "Winning plan", "planSummary"_attr = explainer->getPlanSummary());

    // Close all candidate plans but the winner.
    for (size_t ix = 1; ix < decision->candidateOrder.size(); ++ix) {
        const auto planIdx = decision->candidateOrder[ix];
        invariant(planIdx < candidates.size());
        candidates[planIdx].root->close();
    }

    // An SBE tree that exited early by throwing an exception cannot be reused by design. To work
    // around this limitation, we clone the tree from the original tree. If there is a pipeline in
    // "_cq" the winning candidate will be extended by building a new SBE tree below, so we don't
    // need to clone a new copy here if the winner exited early.
    if (winner.exitedEarly && _cq.cqPipeline().empty()) {
        // Remove all the registered plans from _yieldPolicy's list of trees.
        _yieldPolicy->clearRegisteredPlans();

        tassert(6142204,
                "The winning CandidatePlan should contain the original plan",
                winner.clonedPlan);
        // Clone a new copy of the original plan to use for execution so that the 'clonedPlan' in
        // 'winner' can be inserted into the plan cache while in a clean state.
        winner.data.stageData = stage_builder::PlanStageData(winner.clonedPlan->second.stageData);
        // When we clone the tree below, the new tree's stats will be zeroed out. If this is an
        // explain operation, save the stats from the old tree before we discard it.
        if (_cq.getExplain()) {
            winner.data.stageData.savedStatsOnEarlyExit =
                winner.root->getStats(true /* includeDebugInfo */);
        }
        winner.root = winner.clonedPlan->first->clone();

        stage_builder::prepareSlotBasedExecutableTree(_opCtx,
                                                      winner.root.get(),
                                                      &winner.data.stageData,
                                                      _cq,
                                                      _collections,
                                                      _yieldPolicy,
                                                      false /* preparingFromCache */);
        // Clear the results queue.
        winner.results = {};
        winner.root->open(false /* reOpen*/);
    }

    // Extend the winning candidate with the agg pipeline and rebuild the execution tree. Because
    // the trial was done with find-only part of the query, we cannot reuse the results. The
    // non-winning plans are only used in 'explain()' so, to save on unnecessary work, we extend
    // them only if this is an 'explain()' request.
    if (!_cq.cqPipeline().empty()) {
        winner.root->close();
        _yieldPolicy->clearRegisteredPlans();
        auto solution = QueryPlanner::extendWithAggPipeline(
            _cq, std::move(winner.solution), plannerParams.secondaryCollectionsInfo);
        auto [rootStage, data] = stage_builder::buildSlotBasedExecutableTree(
            _opCtx, _collections, _cq, *solution, _yieldPolicy);
        // The winner might have been replanned. So, pass through the replanning reason to the new
        // plan.
        data.replanReason = std::move(winner.data.stageData.replanReason);

        // We need to clone the plan here for the plan cache to use. The clone will be stored in the
        // cache prior to preparation, whereas the original copy of the tree will be prepared and
        // used to execute this query.
        auto clonedPlan = std::make_pair(rootStage->clone(), plan_ranker::CandidatePlanData{data});
        stage_builder::prepareSlotBasedExecutableTree(_opCtx,
                                                      rootStage.get(),
                                                      &data,
                                                      _cq,
                                                      _collections,
                                                      _yieldPolicy,
                                                      false /* preparingFromCache */);
        candidates[winnerIdx] =
            sbe::plan_ranker::CandidatePlan{std::move(solution),
                                            std::move(rootStage),
                                            plan_ranker::CandidatePlanData{std::move(data)}};
        candidates[winnerIdx].clonedPlan.emplace(std::move(clonedPlan));
        candidates[winnerIdx].root->open(false);

        if (_cq.getExplain()) {
            for (size_t i = 0; i < candidates.size(); ++i) {
                if (i == winnerIdx)
                    continue;  // have already done the winner

                auto solution = QueryPlanner::extendWithAggPipeline(
                    _cq, std::move(candidates[i].solution), plannerParams.secondaryCollectionsInfo);
                auto&& [rootStage, data] = stage_builder::buildSlotBasedExecutableTree(
                    _opCtx, _collections, _cq, *solution, _yieldPolicy);
                candidates[i] = sbe::plan_ranker::CandidatePlan{
                    std::move(solution), std::move(rootStage), std::move(data)};
            }
        }
    }

    // Writes a cache entry for the winning plan to the plan cache if possible.
    plan_cache_util::updateSbePlanCacheFromSbeCandidates(
        _opCtx, _collections, _cachingMode, _cq, std::move(decision), candidates);

    return {std::move(candidates), winnerIdx};
}
}  // namespace mongo::sbe
