/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/query_planner_params.h"

namespace mongo {

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
                                              StringData path,
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
