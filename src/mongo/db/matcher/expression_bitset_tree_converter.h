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
#include "mongo/db/query/boolean_simplification/bitset_tree.h"

namespace mongo {
/**
 * ExpressionBitInfo class stores the original predicate from the MatchExpression in its
 * 'expression' member.
 */
struct ExpressionBitInfo {
    explicit ExpressionBitInfo(std::unique_ptr<MatchExpression> expression)
        : expression(std::move(expression)) {}
    std::unique_ptr<MatchExpression> expression;
};

/**
 * Transform the given MatchExpression tree into a Bitset tree. Returns the bitset tree and a
 * vector of ExpressionBitInfo representing bits in the bitset tree. Every bitset in the BitsetTree
 * has the same number of bits and the size of the vector equals to the number of bits of the
 * bitsets. If the MatchExpression tree contains more than 'maximumNumberOfUniquePredicates' unique
 * predicates the function ends earlier and returns 'none'.
 */
boost::optional<std::pair<boolean_simplification::BitsetTreeNode, std::vector<ExpressionBitInfo>>>
transformToBitsetTree(const MatchExpression* root, size_t maximumNumberOfUniquePredicates);
}  // namespace mongo
