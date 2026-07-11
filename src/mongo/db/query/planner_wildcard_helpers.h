// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/interval_evaluation_tree.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace mongo {
namespace wildcard_planning {

using BoundsTightness = IndexBoundsBuilder::BoundsTightness;

/**
 * Specifies the maximum depth of nested array indices through which a query may traverse before a
 * $** index declines to answer it, due to the exponential complexity of the bounds required.
 */
static constexpr size_t kWildcardMaxArrayIndexTraversalDepth = 8u;

/**
 * Given a single wildcard index, and a set of fields which are being queried, create a 'mock'
 * IndexEntry for each of the query fields and add them into the provided vector.
 */
void expandWildcardIndexEntry(const IndexEntry& wildcardIndex,
                              const std::set<std::string>& fields,
                              std::vector<IndexEntry>* out);

/**
 * Always returns false: both non-generic CWI entries (concrete wildcard path, tight bounds)
 * and the generic entry ("$_path", prefix scan) can satisfy queries with a FETCH stage
 * handling residual predicates. Asserts that any non-generic entry reaching this point was
 * assigned a wildcard predicate, as guaranteed by
 * stripInvalidAssignmentsToCompoundWildcardIndexes.
 */
bool canOnlyAnswerWildcardPrefixQuery(
    const std::vector<std::unique_ptr<QuerySolutionNode>>& ixscanNodes);

/**
 * During planning, the expanded $** IndexEntry's keyPattern and bounds are in the format with
 * expanded field path, like {..., 'path': 1, ...}. Once planning is complete, it is necessary to
 * call this method in order to prepare the IndexEntry and bounds for execution. This function
 * performs the following actions:
 * - Converts the keyPattern to the {..., $_path: 1, "path": 1, ...} format expected by the $**
 *   index.
 * - Adds a new entry '$_path' to the bounds vector, and computes the necessary intervals on it.
 * - Adds a new, empty entry to 'multikeyPaths' for '$_path'.
 * - Updates shouldDedup for index scan node.
 */
void finalizeWildcardIndexScanConfiguration(
    IndexScanNode* scan, std::vector<interval_evaluation_tree::Builder>* ietBuilders);

/**
 * This helper generates index intervals for the "$_path" field to scan all keys indexing a
 * document. The index intervals will be ['[MinKey, MinKey]', '["", {})]' ]. The "MinKey" key value
 * is for documents missing the wildcard field.
 */
std::vector<Interval> makeAllValuesForPath();
}  // namespace wildcard_planning
}  // namespace mongo
