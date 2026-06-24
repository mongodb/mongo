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

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/path_resolver.h"

namespace mongo::join_ordering {

StatusWith<std::unique_ptr<CanonicalQuery>> createCanonicalQueryFromSingleMatchExpression(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    NamespaceString nss,
    std::unique_ptr<MatchExpression> expr);

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

}  // namespace mongo::join_ordering
