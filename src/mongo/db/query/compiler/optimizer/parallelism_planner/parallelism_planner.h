// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/optimizer/parallelism_planner/parallelism_planner_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/util/modules.h"


namespace mongo::parallelism_planner {

/**
 * Builds the parallel plan for 'solution' and attaches it with QuerySolution::setParallelRoot(),
 * then returns the same solution. The serial plan stays the solution's root, and the solution is
 * returned unchanged if the query is not eligible for parallelization.
 *
 * Every one of these must hold for a parallel plan to be considered:
 *   - enableParallelQuery is true and maxDegreeOfParallelism is greater than 1,
 *   - the query asks for no particular scan order with a $natural hint or sort, and no query
 *     settings index hints were applied. (TODO SERVER-139355 Consider allowing certain query
 * settings)
 *   - every node of 'solution' reports isParallelEligible(), and
 *   - the main collection holds at least internalQueryParallelQueryMinCollectionSizeBytes of data.
 *
 * 'querySettingsApplied' is QueryPlannerParams::querySettingsApplied, i.e. whether query settings
 * index hints were applied to the query.
 *
 * Additional restrictions apply depending on the specific query shape and access path.
 *
 * When a gate fails, the reason is recorded within the returned QuerySolution.
 */
std::unique_ptr<QuerySolution> addParallelPlan(const CanonicalQuery& cq,
                                               const MultipleCollectionAccessor& collections,
                                               bool querySettingsApplied,
                                               std::unique_ptr<QuerySolution>&& solution);

}  // namespace mongo::parallelism_planner
