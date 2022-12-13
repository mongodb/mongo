/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/abt/canonical_query_translation.h"

#include "mongo/db/pipeline/abt/algebrizer_context.h"
#include "mongo/db/pipeline/abt/collation_translation.h"
#include "mongo/db/pipeline/abt/match_expression_visitor.h"
#include "mongo/db/pipeline/abt/transformer_visitor.h"

namespace mongo::optimizer {

ABT translateCanonicalQueryToABT(const Metadata& metadata,
                                 const CanonicalQuery& canonicalQuery,
                                 ProjectionName scanProjName,
                                 ABT initialNode,
                                 PrefixId& prefixId) {
    auto matchExpr = generateMatchExpression(
        canonicalQuery.root(), true /* allowAggExpression */, scanProjName, prefixId);

    // Decompose conjunction in the filter into a serial chain of FilterNodes.
    const auto& composition = collectComposedBounded(matchExpr, kMaxPathConjunctionDecomposition);
    for (const auto& path : composition) {
        initialNode = make<FilterNode>(make<EvalFilter>(path, make<Variable>(scanProjName)),
                                       std::move(initialNode));
    }

    AlgebrizerContext ctx{prefixId, {scanProjName, std::move(initialNode)}};

    if (auto sortPattern = canonicalQuery.getSortPattern()) {
        generateCollationNode(ctx, *sortPattern);
    }

    if (auto proj = canonicalQuery.getProj()) {
        translateProjection(ctx, scanProjName, canonicalQuery.getExpCtx(), proj);
    }

    auto skipAmount = canonicalQuery.getFindCommandRequest().getSkip();
    auto limitAmount = canonicalQuery.getFindCommandRequest().getLimit();

    if (limitAmount || skipAmount) {
        ctx.setNode<LimitSkipNode>(
            std::move(ctx.getNode()._rootProjection),
            properties::LimitSkipRequirement(
                limitAmount.value_or(properties::LimitSkipRequirement::kMaxVal),
                skipAmount.value_or(0)),
            std::move(ctx.getNode()._node));
    }

    return make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{
                              std::move(ctx.getNode()._rootProjection)}},
                          std::move(ctx.getNode()._node));
}

}  // namespace mongo::optimizer
