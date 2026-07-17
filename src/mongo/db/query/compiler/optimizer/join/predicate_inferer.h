// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/path_resolver.h"

namespace mongo::join_ordering {
/**
 * Find and add implicit (transitive) edges within the graph.
 * `maxNodes` is the maximum number of nodes allowed in a connected component to be used for
 * implicit edge finding.
 *
 * Example: two edges A.a = B.b and B.b = C.c form an implicit edge A.a = C.c.
 *
 * We then re-use the disjoint set we created to add these implicit edges, to infer and propagate
 * single table predicates across equivalence classes, if any exist.
 *
 * Example: Taking the equivalence class above, {A.a, B.b, C.c} and the STP B.b = 5, we can
 * infer A.a = 5 and C.c = 5 and thus propagate those new access paths to nodes A and C. However, we
 * need to hang onto the original access paths before replacing them or that backing BSON will be
 * destroyed (see  propagateSingleTablePredicate() for more details). Thus we return the original
 * backing BSONObj to eventually be attached to the AggJoinModel instance.
 *
 */
StatusWith<std::vector<BSONObj>> addImplicitEdgesAndInferPredicates(
    MutableJoinGraph& graph,
    const std::vector<ResolvedPath>& resolvedPaths,
    size_t maxNodes,
    const boost::intrusive_ptr<ExpressionContext>& expCtx);

/**
 * Builds a new CanonicalQuery over 'nss' whose filter is 'expr', reusing the projection from
 * 'cqOld'. Used to snapshot a node's filter as parsed, before predicate inference mutates the
 * node's access path by ANDing in inferred single-table predicates.
 */
StatusWith<std::unique_ptr<CanonicalQuery>> cloneCQWithUpdatedFilter(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    NamespaceString nss,
    std::unique_ptr<MatchExpression> expr,
    const CanonicalQuery& cqOld);

}  // namespace mongo::join_ordering
