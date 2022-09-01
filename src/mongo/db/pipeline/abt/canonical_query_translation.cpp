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
    auto abt = generateMatchExpression(canonicalQuery.root(),
                                       true /* allowAggExpression */,
                                       scanProjName,
                                       prefixId.getNextId("match"));

    abt = make<FilterNode>(make<EvalFilter>(std::move(abt), make<Variable>(scanProjName)),
                           std::move(initialNode));

    AlgebrizerContext ctx{prefixId, {scanProjName, std::move(abt)}};

    if (auto sortPattern = canonicalQuery.getSortPattern()) {
        generateCollationNode(ctx, *sortPattern);
    }

    if (auto proj = canonicalQuery.getProj()) {
        translateProjection(ctx, scanProjName, canonicalQuery.getExpCtx(), proj);
    }

    // TODO SERVER-68692: Support limit.
    // TODO SERVER-68693: Support skip.

    return make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{
                              std::move(ctx.getNode()._rootProjection)}},
                          std::move(ctx.getNode()._node));
}

}  // namespace mongo::optimizer
