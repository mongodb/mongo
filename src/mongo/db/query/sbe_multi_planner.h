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

#include <cstddef>
#include <memory>
#include <queue>
#include <utility>
#include <vector>

#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/plan_ranking_decision.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_plan_ranker.h"
#include "mongo/db/query/sbe_runtime_planner.h"
#include "mongo/db/query/sbe_stage_builder_plan_data.h"

namespace mongo::sbe {
/**
 * Collects execution stats for all candidate plans, ranks them and picks the best.
 *
 * TODO: add support for backup plan
 */
class MultiPlanner final : public BaseRuntimePlanner {
public:
    MultiPlanner(OperationContext* opCtx,
                 const MultipleCollectionAccessor& collections,
                 CanonicalQuery& cq,
                 PlanCachingMode cachingMode,
                 PlanYieldPolicySBE* yieldPolicy)
        : BaseRuntimePlanner{opCtx, collections, cq, yieldPolicy},
          _cachingMode{cachingMode},
          _maxNumResults{0},
          _maxNumReads{0} {}

    CandidatePlans plan(
        const QueryPlannerParams& plannerParams,
        std::vector<std::unique_ptr<QuerySolution>> solutions,
        std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots)
        final;

private:
    struct CandidateCmp {
        bool operator()(const plan_ranker::CandidatePlan* lhs,
                        const plan_ranker::CandidatePlan* rhs) const;
    };

    using PlanQ = std::priority_queue<plan_ranker::CandidatePlan*,
                                      std::vector<plan_ranker::CandidatePlan*>,
                                      CandidateCmp>;

    /**
     * Moves candidates into the internal candidates vector, sets up a PlanQ entry for each,
     * prepares each candidate's execution tree and tries to fetch the first document to initialize
     * plan productivity stats.
     */
    PlanQ preparePlans(
        const std::vector<size_t>& planIndexes,
        size_t trackerResultsBudget,
        std::vector<std::unique_ptr<QuerySolution>>& solutions,
        std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>>& roots);

    /**
     * Runs the plans in the queue by fetching one document at a time. Each time the most
     * productive plan is executed.
     */
    void trialPlans(PlanQ planq);

    /**
     * Tries to fetch a single document from the plan. Returns true if the trial run should continue
     * for the given candidate and false if the trial should end.
     */
    bool fetchOneDocument(plan_ranker::CandidatePlan* candidate);

    /**
     * Executes each plan in to collect execution stats. Stops when all the plans have either:
     *    * Hit EOF.
     *    * Returned a pre-defined number of results.
     *    * Failed
     *    * Exited early by throwing a special signaling exception.
     *
     * Each plan is executed at least once. After that plans are executed by fetching one document
     * at a time. Every time the most productive plan is executed.
     *
     * All documents returned by each plan are enqueued into the 'CandidatePlan->results' queue.
     *
     * Upon completion, returns a vector of candidate plans. Execution stats can be obtained for
     * each of the candidate plans by calling 'CandidatePlan->root->getStats()'.
     *
     * After the trial period ends, plans that ran out of memory or reached EOF are closed.
     * All other plans are open, but 'exitedEarly' plans are in an invalid state. Such plans must be
     * re-created using the cloned copy before execution of the plan.
     */
    std::vector<plan_ranker::CandidatePlan> collectExecutionStats(
        std::vector<std::unique_ptr<QuerySolution>> solutions,
        std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots,
        size_t maxTrialPeriodNumReads);
    /**
     * Returns the best candidate plan selected according to the plan ranking 'decision'.
     *
     * Calls 'close' method on all other candidate plans and updates the plan cache entry,
     * if possible.
     */
    CandidatePlans finalizeExecutionPlans(
        const QueryPlannerParams& plannerParams,
        std::unique_ptr<mongo::plan_ranker::PlanRankingDecision> decision,
        std::vector<plan_ranker::CandidatePlan> candidates) const;

    // Describes the cases in which we should write an entry for the winning plan to the plan cache.
    const PlanCachingMode _cachingMode;
    size_t _maxNumResults;
    size_t _maxNumReads;
};
}  // namespace mongo::sbe
