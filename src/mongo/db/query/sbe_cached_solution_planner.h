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

#include "mongo/db/query/all_indices_required_checker.h"
#include "mongo/db/query/query_planner_params.h"
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
                          const MultipleCollectionAccessor& collections,
                          const CanonicalQuery& cq,
                          const QueryPlannerParams& queryParams,
                          boost::optional<size_t> decisionReads,
                          PlanYieldPolicySBE* yieldPolicy)
        : BaseRuntimePlanner{opCtx, collections, cq, queryParams, yieldPolicy},
          _decisionReads{decisionReads} {}

    CandidatePlans plan(
        std::vector<std::unique_ptr<QuerySolution>> solutions,
        std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots)
        final;

private:
    /**
     * Executes the "trial" portion of a single plan until it
     *   - reaches EOF,
     *   - reaches the 'maxNumResults' limit,
     *   - early exits via the TrialRunTracker, or
     *   - returns a failure Status.
     *
     * All documents returned by the plan are enqueued into the 'CandidatePlan->results' queue.
     *
     * When the trial period ends, this function checks the stats to determine if the number of
     * reads during the trial meets the criteria for replanning, in which case it sets the
     * 'needsReplanning' flag of the resulting CandidatePlan to true.
     *
     * The execution plan for the resulting CandidatePlan remains open, but if the 'exitedEarly'
     * flag is set, the plan is in an invalid state and must be closed and reopened before it can be
     * executed.
     */
    plan_ranker::CandidatePlan collectExecutionStatsForCachedPlan(
        std::unique_ptr<QuerySolution> solution,
        std::unique_ptr<PlanStage> root,
        stage_builder::PlanStageData data,
        size_t maxTrialPeriodNumReads);

    /**
     * Uses the QueryPlanner and the MultiPlanner to re-generate candidate plans for this
     * query and select a new winner.
     *
     * Falls back to a new plan if the performance was worse than anticipated during the trial
     * period.
     *
     * The plan cache is modified only if 'shouldCache' is true. The 'reason' string is used to
     * indicate the reason for replanning, which can be included, for example, into plan stats
     * summary.
     */
    CandidatePlans replan(bool shouldCache, std::string reason) const;

    // The number of physical reads taken to decide on a winning plan when the plan was first
    // cached. boost::none in case planing will not be based on the trial run logic.
    const boost::optional<size_t> _decisionReads;
};
}  // namespace mongo::sbe
