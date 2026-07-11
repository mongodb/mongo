// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/exec/runtime_planners/planner_interface.h"
#include "mongo/db/exec/runtime_planners/planner_types.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/canonical_distinct.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/engine_selection.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/replanning_required_info.h"
#include "mongo/db/query/write_ops/canonical_update.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>


namespace mongo {

/**
 * Struct to hold information about a query plan's cache info.
 */
struct PlanCacheInfo {
    boost::optional<uint32_t> planCacheKey;
    boost::optional<uint32_t> planCacheShapeHash;
};

class ClassicRuntimePlannerResult {
public:
    PlanCacheInfo& planCacheInfo() {
        return _cacheInfo;
    }

    void setCachedPlanHash(boost::optional<size_t> cachedPlanHash) {
        // SbeWithClassicRuntimePlanningPrepareExecutionHelper passes cached plan hash to the
        // runtime planner.
    }

    std::unique_ptr<classic_runtime_planner::ClassicPlannerInterface> runtimePlanner;

private:
    PlanCacheInfo _cacheInfo;
};

/**
 * Fills in the given information on the CurOp::OpDebug object, if it has not already been filled in
 * by an outer pipeline.
 */
void setOpDebugPlanCacheInfo(OperationContext* opCtx, const PlanCacheInfo& cacheInfo);

/**
 * Increments the query.planning.invocationCount server status metric.
 */
void incrementPlannerInvocationCount();

/**
 * Increments the query.subPlanner.classicChoseWinningPlan server status metric.
 */
void incrementClassicSubplannerChoseWinningPlan();

using MakePlannerParamsFn = std::function<std::unique_ptr<QueryPlannerParams>(
    const CanonicalQuery&, size_t, boost::optional<QueryPlannerParams::ReplanningData>)>;
using MakePlannerFn =
    std::function<std::unique_ptr<PlannerInterface>(std::unique_ptr<QueryPlannerParams>)>;
/*
 * Calls `makePlanner` with five retries in case exceptions are thrown. Uses the given plannerParams
 * at first to avoid additional calls to `makeQueryPlannerParams`.
 */
std::unique_ptr<PlannerInterface> retryMakePlanner(
    std::unique_ptr<QueryPlannerParams> plannerParams,
    const MakePlannerParamsFn& makeQueryPlannerParams,
    const MakePlannerFn& makePlanner,
    CanonicalQuery* canonicalQuery,
    std::size_t plannerOptions,
    Pipeline* pipeline);

/**
 * Returns true if the single solution from the ranker should be run through multiplanning.
 * This can happen when CBR picked a winner that needs works measured for plan cache insertion,
 * or when force plan cache or force multiplanner is enabled.
 */
bool shouldMultiPlanForSingleSolution(const PlanRankingResult& rankerResult,
                                      const CanonicalQuery* cq);

/**
 * Captures the cardinality estimation method (if it exists) from the winning plan's root node
 * and stores it in CurOp for query stats collection.
 */
void captureCardinalityEstimationMethodForQueryStats(
    OperationContext* opCtx,
    const boost::optional<PlanExplainerData>& maybeExplainData,
    const QuerySolution* solution);

}  // namespace mongo
