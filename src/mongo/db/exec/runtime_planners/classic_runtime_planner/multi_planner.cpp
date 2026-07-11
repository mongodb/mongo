// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/multi_plan.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/query/plan_yield_policy_impl.h"
#include "mongo/util/assert_util.h"

namespace mongo::classic_runtime_planner {

MultiPlanner::MultiPlanner(PlannerData plannerData,
                           std::vector<std::unique_ptr<QuerySolution>> solutions,
                           PlanExplainerData maybeExplainData,
                           bool addingCBRChosenPlanToPlanCache)
    : ClassicPlannerInterface(std::move(plannerData), std::move(maybeExplainData)) {
    plan_cache_util::CacheMode shouldCache = plannerParams().replanningData.has_value()
        ? plannerParams().replanningData->shouldCache
        : plan_cache_util::CacheMode::AlwaysCache;
    auto stage = std::make_unique<MultiPlanStage>(
        cq()->getExpCtxRaw(),
        collections().getMainCollectionPtrOrAcquisition(),
        cq(),
        plan_cache_util::ConditionalClassicPlanCacheWriter{
            shouldCache, opCtx(), collections().getMainCollectionPtrOrAcquisition()},
        boost::none /* replanReason */,
        addingCBRChosenPlanToPlanCache);
    for (auto&& solution : solutions) {
        solution->indexFilterApplied = plannerParams().indexFiltersApplied;
        auto executableTree = buildExecutableTree(*solution);
        stage->addPlan(std::move(solution), std::move(executableTree), ws());
    }
    setRoot(std::move(stage));
    // Need to do this after the move to make the static analyzer happy. The pointer is
    // stored as a PlanStage pointer, so we need to reinterpret cast during retrieval.
    _multiplanStage = reinterpret_cast<MultiPlanStage*>(getRoot());
}

Status MultiPlanner::doPlan(PlanYieldPolicy* planYieldPolicy) {
    tassert(11451403, "MultiPlanner::doPlan() called in invalid state", _state == kNotInitialized);
    auto status = _multiplanStage->runTrials(planYieldPolicy);
    if (!status.isOK()) {
        return status;
    }
    status = _multiplanStage->pickBestPlan();
    _state = kInitialized;
    return status;
}

const MultiPlanStats* MultiPlanner::getSpecificStats() const {
    return static_cast<const MultiPlanStats*>(_multiplanStage->getSpecificStats());
}

Status MultiPlanner::runTrials(trial_period::TrialPhaseConfig trialConfig) {
    tassert(
        11451402, "MultiPlanner::runTrials() called in invalid state", _state == kNotInitialized);
    auto trialPeriodYieldPolicy = makeClassicYieldPolicy(
        opCtx(), cq()->nss(), static_cast<PlanStage*>(_multiplanStage), yieldPolicy());
    return _multiplanStage->runTrials(trialPeriodYieldPolicy.get(), trialConfig);
}

Status MultiPlanner::pickBestPlan() {
    auto status = _multiplanStage->pickBestPlan();
    _state = kInitialized;
    return status;
}

std::unique_ptr<QuerySolution> MultiPlanner::extractQuerySolution() {
    // The query solutions are owned by the 'MultiPlan' stage.
    return nullptr;
}

const QuerySolution* MultiPlanner::querySolution() const {
    return _multiplanStage->bestSolution();
}

PlanExplainerData MultiPlanner::extractExplainData() {
    auto explainData = _multiplanStage->extractPlanExplainerData();
    explainData.planStageQsnMap = std::move(_planStageQsnMap);
    return explainData;
}

void MultiPlanner::abandonTrialsExceptHashes(const boost::container::flat_set<size_t>& hashes) {
    _multiplanStage->abandonTrialsExceptHashes(hashes);
}

void MultiPlanner::markCBRChoseWinner() {
    _multiplanStage->markCBRChoseWinner();
}

void MultiPlanner::emitAccumulatedStats() {
    _multiplanStage->emitAccumulatedStats();
}

}  // namespace mongo::classic_runtime_planner
