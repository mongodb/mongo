// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/runtime_planners/planner_types.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec_deferred_engine_choice {

/*
 * Given query information and the PlanRankingResult, chooses which engine to use and lowers the
 * solution to that engine, returning an executor.
 */
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> lowerPlanRankingResult(
    std::unique_ptr<CanonicalQuery> cq,
    PlanRankingResult rankingResult,
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    Pipeline* pipeline);

}  // namespace mongo::exec_deferred_engine_choice
