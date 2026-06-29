/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
