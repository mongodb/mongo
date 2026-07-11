// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/runtime_planners/planner_types.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/get_executor_helpers.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>


namespace mongo::exec_deferred_engine_choice {

/*
 * Given query information, picks which plan to use to execute the query and summarizes the result
 * in `PlanRankingResult`. May use different planning approaches such as multiplanning, subplanning,
 * or cost-based ranking.
 */
PlanRankingResult planRanking(OperationContext* opCtx,
                              const MultipleCollectionAccessor& collections,
                              std::unique_ptr<CanonicalQuery>& canonicalQuery,
                              PlanYieldPolicy::YieldPolicy yieldPolicy,
                              std::size_t plannerOptions,
                              Pipeline* pipeline,
                              const MakePlannerParamsFn& makeQueryPlannerParams);

}  // namespace mongo::exec_deferred_engine_choice
