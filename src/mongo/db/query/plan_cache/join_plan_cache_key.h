// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_cache/join_plan_cache.h"

#include <vector>

namespace mongo {

/**
 * Builds a JoinPlanCacheKey from a fully-constructed join graph.
 *
 * Each node's contribution is a PlanCacheKeyInfo (match expression shape +
 * indexability discriminators for that node's collection), which ensures that
 * the key changes if the collection's index set changes eligibility for the query.
 *
 * 'resolvedPaths' maps PathId values in JoinEdge predicates to their concrete
 * FieldPath strings, this is produced by the PathResolver during join graph construction.
 *
 * 'collections' is used to look up the CollectionPtr for each node's collection so
 * that indexability discriminators can be computed.
 */
JoinPlanCacheKey makeJoinPlanCacheKey(const join_ordering::JoinGraph& graph,
                                      const std::vector<join_ordering::ResolvedPath>& resolvedPaths,
                                      const MultipleCollectionAccessor& collections);

}  // namespace mongo
