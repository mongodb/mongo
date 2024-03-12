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
#include <utility>
#include <vector>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/all_indices_required_checker.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_plan_ranker.h"
#include "mongo/db/query/sbe_stage_builder_plan_data.h"
#include "mongo/db/query/sbe_trial_runtime_executor.h"
#include "mongo/util/assert_util_core.h"

namespace mongo::sbe {
/**
 * This struct holds a vector with all candidate plans evaluated by this RuntimePlanner, and an
 * index pointing to the winning plan within this vector.
 */
struct CandidatePlans {
    auto& winner() {
        invariant(winnerIdx < plans.size());
        return plans[winnerIdx];
    }

    std::vector<plan_ranker::CandidatePlan> plans;
    size_t winnerIdx;
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
        const QueryPlannerParams& plannerParams,
        std::vector<std::unique_ptr<QuerySolution>> solutions,
        std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots) = 0;
};

/**
 * A base class for runtime planner for common data members and constructor.
 */
class BaseRuntimePlanner : public RuntimePlanner {
public:
    BaseRuntimePlanner(OperationContext* opCtx,
                       const MultipleCollectionAccessor& collections,
                       CanonicalQuery& cq,
                       PlanYieldPolicySBE* yieldPolicy)
        : _opCtx(opCtx),
          _collections(collections),
          _cq(cq),
          _yieldPolicy(yieldPolicy),
          _indexExistenceChecker(collections),
          _trialRuntimeExecutor{_opCtx, _collections, _cq, _yieldPolicy, _indexExistenceChecker} {
        invariant(_opCtx);
    }

protected:
    OperationContext* const _opCtx;
    const MultipleCollectionAccessor& _collections;
    CanonicalQuery& _cq;
    PlanYieldPolicySBE* const _yieldPolicy;
    const AllIndicesRequiredChecker _indexExistenceChecker;
    TrialRuntimeExecutor _trialRuntimeExecutor;

    std::vector<plan_ranker::CandidatePlan> _candidates;
};
}  // namespace mongo::sbe
