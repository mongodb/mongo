// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/compiler/rewrites/boolean_simplification/bitset_algebra.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_bitset_tree_converter.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * Restore MatchExpression tree from a bitset tree and a list of expressions representing bits in
 * the the bitset tree: i-th expression in the expressions lists represents i-th bit in the bitset
 * tree.
 */
std::unique_ptr<MatchExpression> restoreMatchExpression(
    const boolean_simplification::BitsetTreeNode& bitsetTree,
    const BitsetTreeTransformResult::ExpressionList& expressions);
}  // namespace mongo
