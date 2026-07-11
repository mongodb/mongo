// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/cstdint.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/subplan.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/get_executor_helpers.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/wildcard_multikey_paths.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/update/update_driver.h"


namespace mongo {

struct ExpressResult {
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> executor;
    std::unique_ptr<QueryPlannerParams> plannerParams;
};

/*
 * Builds an express executor if the query is eligible. Otherwise returns the planner params created
 * to check express eligibility, for reuse.
 */
ExpressResult tryExpress(OperationContext* opCtx,
                         const MultipleCollectionAccessor& collections,
                         std::unique_ptr<CanonicalQuery>& canonicalQuery,
                         std::size_t plannerOptions,
                         const MakePlannerParamsFn& makePlannerParams);

/*
 * Builds an IdHack planner if eligible, otherwise returns nullptr.
 */
std::unique_ptr<classic_runtime_planner::IdHackPlanner> tryIdHack(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    CanonicalQuery* cq,
    const std::function<PlannerData()>& makePlannerData);

}  // namespace mongo
