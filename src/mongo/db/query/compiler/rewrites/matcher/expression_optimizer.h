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
