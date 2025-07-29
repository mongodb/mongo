/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/query/compiler/rewrites/boolean_simplification/bitset_tree.h"

namespace mongo {
/**
 * ExpressionBitInfo class stores the original predicate from the MatchExpression in its
 * 'expression' member.
 */
struct ExpressionBitInfo {
    explicit ExpressionBitInfo(const MatchExpression* expr) : expression(expr) {}
    const MatchExpression* expression;
};

/**
 * Contains result of 'transformToBitsetTree' function.
 */
struct BitsetTreeTransformResult {
    using ExpressionList = absl::InlinedVector<ExpressionBitInfo, 4>;

    // The root node of the bitset tree.
    boolean_simplification::BitsetTreeNode bitsetTree;

    // A vector of ExpressionBitInfo represented by bits in the bitset tree. The size of the vector
    // equals to the number of bits of the BitSet tree.
    ExpressionList expressions;

    // The number of nodes of the original MatchExpression tree.
    size_t expressionSize;
};

/**
 * Transform the given MatchExpression tree into a Bitset tree. If the MatchExpression tree contains
 * schema expressions or more than 'maximumNumberOfUniquePredicates' unique predicates the function
 * ends earlier and returns 'none'.
 */
boost::optional<BitsetTreeTransformResult> transformToBitsetTree(
    const MatchExpression* root, size_t maximumNumberOfUniquePredicates);
}  // namespace mongo
