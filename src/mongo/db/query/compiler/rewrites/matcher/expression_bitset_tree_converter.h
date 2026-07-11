// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/compiler/rewrites/boolean_simplification/bitset_tree.h"
#include "mongo/util/modules.h"

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
