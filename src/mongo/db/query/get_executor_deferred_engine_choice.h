// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_distinct.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/get_executor_helpers.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/write_ops/canonical_update.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>

namespace mongo::exec_deferred_engine_choice {

class Collection;
class CollectionPtr;
class CountRequest;

/**
 * Creates an executor for the given query. Similar to getExecutorFind(), but makes the engine
 * choice after a QuerySolution is chosen. By delaying the choice, we allow for more precise engine
 * decisions. Eventually this will replace get_executor.h, which will be deleted.
 * TODO SERVER-119036 delete old get_executor
 */
[[MONGO_MOD_PUBLIC]] StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
getExecutorFindDeferredEngineChoice(OperationContext* opCtx,
                                    const MultipleCollectionAccessor& collections,
                                    std::unique_ptr<CanonicalQuery> canonicalQuery,
                                    PlanYieldPolicy::YieldPolicy yieldPolicy,
                                    const MakePlannerParamsFn& makeQueryPlannerParams,
                                    size_t plannerOptions = QueryPlannerParams::DEFAULT,
                                    Pipeline* pipeline = nullptr);

}  // namespace mongo::exec_deferred_engine_choice
