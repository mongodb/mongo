// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * Make simplifying changes to the structure of a MatchExpression tree without altering its
 * semantics. This function may return:
 *   - a pointer to the original, unmodified MatchExpression,
 *   - a pointer to the original MatchExpression that has been mutated, or
 *   - a pointer to a new MatchExpression.
 *
 * The value of 'expression' must not be nullptr.
 * 'enableSimplification' parameter controls Boolean Expression Simplifier.
 */
std::unique_ptr<MatchExpression> optimizeMatchExpression(
    std::unique_ptr<MatchExpression> expression, bool enableSimplification = true);

/**
 * Traverses expression tree post-order. Sorts children at each non-leaf node by (MatchType,
 * path(), children, number of children).
 *
 * The value of 'tree' must not be nullptr.
 */
void sortMatchExpressionTree(MatchExpression* tree);

/**
 * Convenience method which normalizes a MatchExpression tree by optimizing and then sorting it.
 *
 * The value of 'tree' must not be nullptr.
 * 'enableSimplification' parameter controls Boolean Expression Simplifier.
 */
std::unique_ptr<MatchExpression> normalizeMatchExpression(std::unique_ptr<MatchExpression> tree,
                                                          bool enableSimplification = true);
}  // namespace mongo
