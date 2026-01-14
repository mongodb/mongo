/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/path_resolver.h"
#include "mongo/util/modules.h"

namespace mongo::join_ordering {

/**
 * Represents the result of splitting a MatchExpression from a $match in the subpipeline of a
 * $lookup into parts representing equijoin predicates and residual single table predicates.
 */
struct SplitPredicatesResult {
    // List of equijoin predicates. Each element will be an agg expression of the form:
    // {$eq: ['$fieldPathInForeignNSS', '$$varCorrespondingToPathInLocalNSS']}
    // Note that the order of operands is not guaranteed and there may be semantically duplicate
    // entries.
    std::vector<boost::intrusive_ptr<const Expression>> joinPredicates;
    // Residual predicate on the single table which does not contain any correlated predicates. In
    // other terms, this expression is able to be pushed down to the find layer.
    std::unique_ptr<MatchExpression> singleTablePredicates;
};

/**
 * Given a MatchExpression and set of variables corresponding to the 'let' statement of a $lookup,
 * attempt to split the expression into equijoin predicates and single table predicates. The
 * equijoin predicates are suitable to insert into the JoinGraph while the single table predicates
 * are intended to be pushed into the find layer for the foreign namespace. If this function fails
 * to split the expression, it returns boost::none. The main reason this would occur is the
 * expression contains variables referencing the local namespace which are not equijoin predicates.
 * This function does not modify its inputs.
 */
boost::optional<SplitPredicatesResult> splitJoinAndSingleCollectionPredicates(
    const MatchExpression* matchExpr, const std::vector<LetVariable>& variables);


struct ExprPredicatesResult {
    bool expressionIsFullyAbsorbed;

    // Extracted join predicates.
    std::vector<JoinPredicate> predicates;
};

/**
 * Extract join equality predicates from the given MatchExpression 'expr'.
 * A predicate can be qualified as a proper join equality predicate only if satisfies the following
 * conditions: it must be a $expr equality predicate which contains two field paths belonging to
 * different collections.
 * The 'expr' can be fully absorbed into join graph if it contains only proper join equality
 * predicates and $and's.
 * The function returns 'ExprPredicatesResult' which contains a list of join predicates and a
 * boolean value identifying whether the 'expr' was fully absorbed.
 */
ExprPredicatesResult extractExprPredicates(PathResolver& pathResolver, const MatchExpression* expr);
}  // namespace mongo::join_ordering
