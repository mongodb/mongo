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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_plan_ranker.h"

namespace mongo::sbe {
/**
 * This struct holds a vector with all candidate plans evaluated by this RuntimePlanner, and an
 * index pointing to the winning plan within this vector.
 */
struct CandidatePlans {
    std::vector<plan_ranker::CandidatePlan> plans;
    size_t winnerIdx;

    auto& winner() {
        invariant(winnerIdx < plans.size());
        return plans[winnerIdx];
    }
};

/**
 * An interface to be implemented by all classes which can evaluate the cost of a PlanStage tree in
 * order to pick the the best plan amongst those specified in 'roots' vector. Evaluation is done in
 * runtime by collecting execution stats for each of the plans, and the best candidate plan is
 * chosen according to certain criteria.
 */
class RuntimePlanner {
public:
    virtual ~RuntimePlanner() = default;

    virtual CandidatePlans plan(
        std::vector<std::unique_ptr<QuerySolution>> solutions,
        std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots) = 0;
};

/**
 * A base class for runtime planner which provides a method to perform a trial run for the candidate
 * plan by executing each plan in a round-robin fashion and collecting execution stats. Each
 * specific implementation can use the collected stats to select the best plan amongst the
 * candidates.
 */
class BaseRuntimePlanner : public RuntimePlanner {
public:
    BaseRuntimePlanner(OperationContext* opCtx,
                       const CollectionPtr& collection,
                       const CanonicalQuery& cq,
                       PlanYieldPolicySBE* yieldPolicy)
        : _opCtx(opCtx), _collection(collection), _cq(cq), _yieldPolicy(yieldPolicy) {
        invariant(_opCtx);
    }

protected:
    /**
     * Prepares the given plan stage tree for execution, attaches it to the operation context and
     * returns two slot accessors for the result and recordId slots, and a boolean value indicating
     * if the plan has exited early from the trial period.
     */
    std::tuple<sbe::value::SlotAccessor*, sbe::value::SlotAccessor*, bool> prepareExecutionPlan(
        PlanStage* root, stage_builder::PlanStageData* data) const;

    /**
     * Executes each plan in a round-robin fashion to collect execution stats. Stops when:
     *    * Any plan hits EOF.
     *    * Or returns a pre-defined number of results.
     *    * Or all candidate plans fail or exit early by throwing a special signaling exception.
     *
     * All documents returned by each plan are enqueued into the 'CandidatePlan->results' queue.
     *
     * Upon completion returns a vector of candidate plans. Execution stats can be obtained for each
     * of the candidate plans by calling 'CandidatePlan->root->getStats()'.
     *
     * After the trial period ends, all plans remain open.
     */
    std::vector<plan_ranker::CandidatePlan> collectExecutionStats(
        std::vector<std::unique_ptr<QuerySolution>> solutions,
        std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots);

    OperationContext* const _opCtx;
    const CollectionPtr& _collection;
    const CanonicalQuery& _cq;
    PlanYieldPolicySBE* const _yieldPolicy;
};
}  // namespace mongo::sbe
