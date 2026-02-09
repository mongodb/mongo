/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/classic/multi_plan.h"
#include "mongo/db/exec/runtime_planners/exec_deferred_engine_choice_runtime_planner/planner_interface.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/engine_selection.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_ranking/plan_ranker.h"
#include "mongo/db/query/plan_yield_policy_impl.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"

#include <absl/functional/bind_front.h>

namespace mongo::exec_deferred_engine_choice {

MultiPlanner::MultiPlanner(PlannerData plannerData,
                           std::vector<std::unique_ptr<QuerySolution>> solutions)
    : DeferredEngineChoicePlannerInterface(std::move(plannerData)) {
    _multiplanStage = std::make_unique<MultiPlanStage>(
        cq()->getExpCtxRaw(),
        collections().getMainCollectionPtrOrAcquisition(),
        cq(),
        absl::bind_front(&MultiPlanner::_cacheDependingOnPlan, this),
        boost::none /*replanReason*/);
    for (auto&& solution : solutions) {
        solution->indexFilterApplied = plannerParams()->indexFiltersApplied;
        auto executableTree = buildExecutableTree(*solution);
        _multiplanStage->addPlan(std::move(solution), std::move(executableTree), ws());
    }
}

const MultiPlanStats* MultiPlanner::getSpecificStats() const {
    return static_cast<const MultiPlanStats*>(_multiplanStage->getSpecificStats());
}

// TODO SERVER-117636: Split out caching mechanism from multiplanning.
void MultiPlanner::_cacheDependingOnPlan(const CanonicalQuery& queryToCache,
                                         MultiPlanStage& mps,
                                         std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
                                         std::vector<plan_ranker::CandidatePlan>& candidates) {}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> MultiPlanner::makeExecutor(
    std::unique_ptr<CanonicalQuery> canonicalQuery, Pipeline* pipeline) {
    auto trialPeriodYieldPolicy = makeClassicYieldPolicy(
        opCtx(), cq()->nss(), static_cast<PlanStage*>(_multiplanStage.get()), yieldPolicy());
    uassertStatusOK(_multiplanStage->runTrials(trialPeriodYieldPolicy.get()));
    uassertStatusOK(_multiplanStage->pickBestPlan());

    auto querySolution = _multiplanStage->extractBestSolution();
    auto engine = chooseEngine(
        opCtx(),
        collections(),
        canonicalQuery.get(),
        pipeline,
        canonicalQuery->getExpCtx()->getNeedsMerge(),
        std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForPushDownStagesDecision{
            .opCtx = opCtx(),
            .canonicalQuery = *cq(),
            .collections = collections(),
            .plannerOptions = plannerOptions(),
        }),
        querySolution.get());
    const bool useSbe = engine == EngineChoice::kSbe;

    // TODO SERVER-119040: Pass explain information to executor.
    return executorFromSolution(useSbe,
                                std::move(canonicalQuery),
                                std::move(querySolution),
                                std::move(_multiplanStage),
                                pipeline);
}
}  // namespace mongo::exec_deferred_engine_choice
