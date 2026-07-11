// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

namespace projection_executor {
class ProjectionExecutor;
}  // namespace projection_executor


/**
 * Check if an index is suitable for use in a plan using a DISTINCT_SCAN stage (the "fast
 * distinct hack" node, which can be used only if there is an empty query predicate).
 *
 * Criteria for suitable index is that the index should be of type BTREE or HASHED and the index
 * cannot be a partial index.
 *
 * Sparse indexes are not suitable when strictDistinctOnly is true, since in that case we want to
 * treat missing fields as null rather than ignore them.
 *
 * Multikey indices are not suitable for DistinctNode when the projection is on an array
 * element. Arrays are flattened in a multikey index which makes it impossible for the distinct
 * scan stage (plan stage generated from DistinctNode) to select the requested element by array
 * index.
 *
 * Multikey indices cannot be used for the fast distinct hack if the field is dotted. Currently
 * the solution generated for the distinct hack includes a projection stage and the projection
 * stage cannot be covered with a dotted field.
 *
 * For wildcards indicies (when 'wildcardProj' is specified), the projection needs to cover the
 * field over which we are distinct-ing.
 */
bool isIndexSuitableForDistinct(const BSONObj& keyPattern,
                                bool multikey,
                                const MultikeyPaths& multikeyPaths,
                                bool sparse,
                                projection_executor::ProjectionExecutor* wildcardProj,
                                std::string_view field,
                                const BSONObj& filter,
                                bool flipDistinctScanDirection,
                                bool strictDistinctOnly,
                                const OrderedPathSet& projectionFields = {},
                                bool hasSort = true);

/**
 * Return whether or not any component of the path 'path' is multikey given an index key pattern
 * and multikeypaths. If no multikey metdata is available for the index, and the index is marked
 * multikey, conservatively assumes that a component of 'path' _is_ multikey. The 'isMultikey'
 * property of an index is false for indexes that definitely have no multikey paths. If the query
 * has no specified sort, we must also consider whether any of the projection fields are multikey
 * when determining eligibility for DISTINCT_SCAN. We pass the hasSort parameter to confirm that we
 * only check for projection multikeyness if no sort order has been specified. If a sort does exist,
 * plan enumeration will properly handle checking the (in)validity of a covered IXSCAN or
 * DISTINCT_SCAN in this case. Without a sort, these checks are bypassed so we check them here.
 */
bool isAnyComponentOfPathOrProjectionMultikey(const BSONObj& indexKeyPattern,
                                              bool isMultikey,
                                              const MultikeyPaths& indexMultikeyInfo,
                                              std::string_view path,
                                              const OrderedPathSet& projFields = {},
                                              bool hasSort = true);

/**
 * If possible, turn the provided QuerySolution into a QuerySolution that uses a DistinctNode
 * to provide results for the distinct command. If the QuerySolution is already using a
 * DistinctNode, finalize it by pushing ShardingFilter and FetchNodes into the distinct scan. Plans
 * involving multiple scans will not be mutated.
 *
 * When 'plannerParams' does not specify 'STRICT_DISTINCT_ONLY', any resulting QuerySolution will
 * limit the number of documents that need to be examined to compute the results of a distinct
 * command, but it may not guarantee that there are no duplicate values for the distinct field.
 *
 * If the provided solution could be mutated successfully, returns true, otherwise returns
 * false. This conversion is known as the 'distinct hack'.
 */
bool finalizeDistinctScan(const CanonicalQuery& canonicalQuery,
                          const QueryPlannerParams& plannerParams,
                          QuerySolution* soln,
                          const std::string& field,
                          bool flipDistinctScanDirection = false);

/**
 * Attempt to create a query solution with DISTINCT_SCAN based on an index which could
 * provide the distinct semantics on the key from the 'canonicalDistinct`.
 */
std::unique_ptr<QuerySolution> constructCoveredDistinctScan(
    const CanonicalQuery& canonicalQuery,
    const QueryPlannerParams& plannerParams,
    const CanonicalDistinct& canonicalDistinct);

/**
 * If the canonical query doesn't have a filter and a sort, the query planner won't try to build an
 * index scan, so we will try to create a DISTINCT_SCAN manually.
 *
 * Otherwise, if the distinct multiplanner is disabled, we will return the first query solution that
 * can be transformed to an DISTINCT_SCAN from the candidates returned by the query planner.
 */
std::unique_ptr<QuerySolution> createDistinctScanSolution(const CanonicalQuery& canonicalQuery,
                                                          const QueryPlannerParams& plannerParams,
                                                          bool flipDistinctScanDirection);

}  // namespace mongo
