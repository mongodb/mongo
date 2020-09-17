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

#pragma once

#include "mongo/db/query/sbe_plan_ranker.h"
#include "mongo/db/query/sbe_runtime_planner.h"

namespace mongo::sbe {
/**
 * Runs a trial period in order to evaluate the cost of a cached plan. If the cost is unexpectedly
 * high, the plan cache entry is deactivated and we use multi-planning to select an entirely new
 * winning plan. This process is called "replanning".
 *
 * TODO: refresh the list of indexes in 'queryParams' during replanning.
 */
class CachedSolutionPlanner final : public BaseRuntimePlanner {
public:
    CachedSolutionPlanner(OperationContext* opCtx,
                          const CollectionPtr& collection,
                          const CanonicalQuery& cq,
                          const QueryPlannerParams& queryParams,
                          size_t decisionReads,
                          PlanYieldPolicySBE* yieldPolicy)
        : BaseRuntimePlanner{opCtx, collection, cq, yieldPolicy},
          _queryParams{queryParams},
          _decisionReads{decisionReads} {}

    plan_ranker::CandidatePlan plan(
        std::vector<std::unique_ptr<QuerySolution>> solutions,
        std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots)
        final;

private:
    /**
     * Finalizes the winning plan before passing it to the caller as a result of the planning.
     */
    plan_ranker::CandidatePlan finalizeExecutionPlan(std::unique_ptr<sbe::PlanStageStats> stats,
                                                     plan_ranker::CandidatePlan candidate) const;

    /**
     * Uses the QueryPlanner and the MultiPlanner to re-generate candidate plans for this
     * query and select a new winner.
     *
     * Falls back to a new plan if the performance was worse than anticipated during the trial
     * period.
     *
     * The plan cache is modified only if 'shouldCache' is true.
     */
    plan_ranker::CandidatePlan replan(bool shouldCache) const;

    // Query parameters used to create a query solution when the plan was first created. Used during
    // replanning.
    const QueryPlannerParams _queryParams;

    // The number of physical reads taken to decide on a winning plan when the plan was first
    // cached.
    const size_t _decisionReads;
};
}  // namespace mongo::sbe
