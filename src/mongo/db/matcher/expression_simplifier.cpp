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

#include "mongo/db/matcher/expression_simplifier.h"
#include "mongo/db/matcher/expression_restorer.h"
#include "mongo/db/query/boolean_simplification/quine_mccluskey.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
using boolean_simplification::BitsetTreeNode;
using boolean_simplification::Maxterm;
using boolean_simplification::Minterm;

namespace {
Maxterm quineMcCluskey(const BitsetTreeNode& tree, const ExpressionSimlifierSettings& settings) {
    auto maxterm = boolean_simplification::convertToDNF(tree);
    maxterm.removeRedundancies();

    LOGV2_DEBUG(
        7767001, 5, "MatchExpression in DNF representation", "maxterm"_attr = maxterm.toString());

    if (settings.applyQuineMcCluskey) {
        maxterm = boolean_simplification::quineMcCluskey(std::move(maxterm));
    }

    return maxterm;
}

boost::optional<BitsetTreeNode> handleRootedAndCase(Maxterm dnfExpression) {
    auto [commonPredicates, maxterm] =
        boolean_simplification::extractCommonPredicates(std::move(dnfExpression));

    if (commonPredicates.isAlwaysTrue()) {
        return {boolean_simplification::convertToBitsetTree(maxterm)};
    }

    BitsetTreeNode topBitsetTree =
        boolean_simplification::convertToBitsetTree(Maxterm{commonPredicates});
    if (!maxterm.isAlwaysTrue()) {
        BitsetTreeNode bitsetTree = boolean_simplification::convertToBitsetTree(maxterm);
        topBitsetTree.internalChildren.emplace_back(std::move(bitsetTree));
    }

    return {std::move(topBitsetTree)};
}

boost::optional<BitsetTreeNode> simplifyBitsetTree(const BitsetTreeNode& tree,
                                                   const ExpressionSimlifierSettings& settings) {
    auto maxterm = quineMcCluskey(tree, settings);

    // 1 root OR term + number child conjuctive terms.
    const size_t numberOfTerms = 1 + maxterm.minterms.size();
    if (tree.calculateNumberOfTerms() * settings.maxNumberOfTermsFactor < numberOfTerms) {
        LOGV2_DEBUG(
            8113910, 2, "The number of predicates has exceeded the 'maxNumberOfTermsFactor' limit");
        return boost::none;
    }

    if (tree.type == BitsetTreeNode::And && settings.doNotOpenContainedOrs) {
        return handleRootedAndCase(std::move(maxterm));
    }

    return {boolean_simplification::convertToBitsetTree(maxterm)};
}
}  // namespace

const ExpressionSimlifierSettings ExpressionSimlifierSettings::kPermissive{
    std::numeric_limits<size_t>::max(), 1e6, false, true};

boost::optional<std::unique_ptr<MatchExpression>> simplifyMatchExpression(
    const MatchExpression* root, const ExpressionSimlifierSettings& settings) {
    LOGV2_DEBUG(7767000,
                5,
                "Converting MatchExpression to corresponding DNF",
                "expression"_attr = root->debugString());

    auto result = transformToBitsetTree(root, settings.maximumNumberOfUniquePredicates);

    if (MONGO_unlikely(!result.has_value())) {
        LOGV2_DEBUG(8113911, 2, "Maximum number of unique predicates in DNF transformer exceeded");
        return boost::none;
    }

    auto bitsetAndExpressions = std::move(*result);

    auto bitsetTreeResult = simplifyBitsetTree(bitsetAndExpressions.first, settings);
    if (MONGO_unlikely(!bitsetTreeResult.has_value())) {
        return boost::none;
    }

    return restoreMatchExpression(*bitsetTreeResult, bitsetAndExpressions.second);
}
}  // namespace mongo
