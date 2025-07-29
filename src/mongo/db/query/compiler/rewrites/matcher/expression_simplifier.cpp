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

#include "mongo/db/query/compiler/rewrites/matcher/expression_simplifier.h"

#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/match_expression_walker.h"
#include "mongo/db/query/compiler/rewrites/boolean_simplification/quine_mccluskey.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_restorer.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
using boolean_simplification::BitsetTreeNode;
using boolean_simplification::Maxterm;

ExpressionSimplifierMetrics expressionSimplifierMetrics;

namespace {
struct Context {
    /**
     * An expression cannot contain more than one operator that uses a special index (geo, text).
     */
    bool isExpressionValid() const {
        return numberOfSpecialIndexOperators < 2;
    };

    size_t numberOfSpecialIndexOperators{0};
};

class ValidateVisitor : public MatchExpressionConstVisitor {
public:
    ValidateVisitor(Context& context) : _context{context} {}

    void visit(const AndMatchExpression* expr) final {}
    void visit(const OrMatchExpression* expr) final {}
    void visit(const NorMatchExpression* expr) final {}
    void visit(const NotMatchExpression* expr) final {}
    void visit(const ElemMatchValueMatchExpression* expr) final {}
    void visit(const ElemMatchObjectMatchExpression* expr) final {}

    void visit(const BitsAllClearMatchExpression* expr) final {}
    void visit(const BitsAllSetMatchExpression* expr) final {}
    void visit(const BitsAnyClearMatchExpression* expr) final {}
    void visit(const BitsAnySetMatchExpression* expr) final {}
    void visit(const EqualityMatchExpression* expr) final {}
    void visit(const GTEMatchExpression* expr) final {}
    void visit(const GTMatchExpression* expr) final {}
    void visit(const LTEMatchExpression* expr) final {}
    void visit(const LTMatchExpression* expr) final {}

    void visit(const InMatchExpression* expr) final {}
    void visit(const ModMatchExpression* expr) final {}
    void visit(const RegexMatchExpression* expr) final {}
    void visit(const SizeMatchExpression* expr) final {}
    void visit(const TypeMatchExpression* expr) final {}

    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}

    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {}

    void visit(const GeoMatchExpression* expr) final {
        ++_context.numberOfSpecialIndexOperators;
    }
    void visit(const GeoNearMatchExpression* expr) final {
        ++_context.numberOfSpecialIndexOperators;
    }
    void visit(const InternalBucketGeoWithinMatchExpression* expr) final {
        ++_context.numberOfSpecialIndexOperators;
    }

    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalExprGTMatchExpression* expr) final {}
    void visit(const InternalExprGTEMatchExpression* expr) final {}
    void visit(const InternalExprLTMatchExpression* expr) final {}
    void visit(const InternalExprLTEMatchExpression* expr) final {}

    void visit(const TextMatchExpression* expr) final {
        ++_context.numberOfSpecialIndexOperators;
    }
    void visit(const TextNoOpMatchExpression* expr) final {
        ++_context.numberOfSpecialIndexOperators;
    }

    void visit(const TwoDPtInAnnulusExpression* expr) final {
        ++_context.numberOfSpecialIndexOperators;
    }

    void visit(const WhereMatchExpression* expr) final {}
    void visit(const WhereNoOpMatchExpression* expr) final {}

    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(const InternalSchemaCondMatchExpression* expr) final {}
    void visit(const InternalSchemaEqMatchExpression* expr) final {}
    void visit(const InternalSchemaFmodMatchExpression* expr) final {}
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaObjectMatchExpression* expr) final {}
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {}
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaXorMatchExpression* expr) final {}

    void visit(const InternalEqHashedKey* expr) final {}

private:
    Context& _context;
};

bool isExpressionValid(const MatchExpression* root) {
    Context context{};
    ValidateVisitor visitor{context};
    MatchExpressionWalker walker{&visitor, nullptr, nullptr};
    tree_walker::walk<true, MatchExpression>(root, &walker);
    return context.isExpressionValid();
}

bool containsNegations(const Maxterm& dnf) {
    return std::any_of(dnf.minterms.begin(), dnf.minterms.end(), [](const auto& minterm) {
        return minterm.mask != minterm.predicates;
    });
}

boost::optional<Maxterm> quineMcCluskey(const BitsetTreeNode& tree,
                                        const ExpressionSimplifierSettings& settings) {
    auto maxterm = boolean_simplification::convertToDNF(tree, settings.maximumNumberOfMinterms);
    if (!maxterm) {
        LOGV2_DEBUG(
            8113912, 2, "The number of minterms has exceeded the 'maximumNumberOfMinterms' limit");
        return boost::none;
    }

    // The simplifications using Absorption law.
    maxterm->removeRedundancies();

    LOGV2_DEBUG(7767001,
                5,
                "MatchExpression in DNF representation",
                "maxterm"_attr = redact(maxterm->toString()));

    // The simplifications using the Quine-McCluskey algorithm (x&y | x&~y = x). The QMC works only
    // for expressions with negations.
    if (settings.applyQuineMcCluskey && containsNegations(*maxterm)) {
        maxterm = boolean_simplification::quineMcCluskey(std::move(*maxterm));
    }

    return maxterm;
}

boost::optional<BitsetTreeNode> handleRootedAndCase(Maxterm dnfExpression) {
    auto [commonPredicates, maxterm] =
        boolean_simplification::extractCommonPredicates(std::move(dnfExpression));

    if (commonPredicates.isConjunctionAlwaysTrue()) {
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

boost::optional<BitsetTreeNode> simplifyBitsetTree(BitsetTreeNode&& tree,
                                                   const ExpressionSimplifierSettings& settings) {
    // Nothing to simplify since the the expression has only one conjunctive or disjunctive term.
    if (tree.internalChildren.empty()) {
        // Since the expression restorer does not accept any BitsetTree nodes with negations,
        // particularly in the presence of the $nor operator, we apply De Morgan's Law which
        // effectively push down the negations down the tree to the leaves.
        tree.applyDeMorgan();
        return std::move(tree);
    }

    auto maxterm = quineMcCluskey(tree, settings);
    if (!maxterm) {
        return boost::none;
    }

    if (tree.type == BitsetTreeNode::And && settings.doNotOpenContainedOrs) {
        return handleRootedAndCase(std::move(*maxterm));
    }

    return boolean_simplification::convertToBitsetTree(*maxterm);
}
}  // namespace

boost::optional<std::unique_ptr<MatchExpression>> simplifyMatchExpression(
    const MatchExpression* root, const ExpressionSimplifierSettings& settings) {
    LOGV2_DEBUG(7767000,
                5,
                "Converting MatchExpression to corresponding DNF",
                "expression"_attr = redact(root->debugString()));

    auto bitsetAndExpressions =
        transformToBitsetTree(root, settings.maximumNumberOfUniquePredicates);

    if (MONGO_unlikely(!bitsetAndExpressions.has_value())) {
        LOGV2_DEBUG(8113911,
                    2,
                    "The query contains schema expressions or maximum number of unique predicates "
                    "in DNF transformer exceeded");
        expressionSimplifierMetrics.abortedTooLargeCount.incrementRelaxed();
        return boost::none;
    }

    auto bitsetTreeResult =
        simplifyBitsetTree(std::move(bitsetAndExpressions->bitsetTree), settings);
    if (MONGO_unlikely(!bitsetTreeResult.has_value())) {
        expressionSimplifierMetrics.abortedTooLargeCount.incrementRelaxed();
        return boost::none;
    }

    if (bitsetAndExpressions->expressionSize * settings.maxSizeFactor <=
        bitsetTreeResult->calculateSize()) {
        LOGV2_DEBUG(8113910, 2, "The number of predicates has exceeded the 'maxSizeFactor' limit");
        expressionSimplifierMetrics.notSimplifiedCount.incrementRelaxed();
        return boost::none;
    }

    auto simplifiedExpression =
        restoreMatchExpression(*bitsetTreeResult, bitsetAndExpressions->expressions);

    // Check here if we still have a valid query, e.g. no more than one special index operator (geo,
    // text).
    if (!isExpressionValid(simplifiedExpression.get())) {
        LOGV2_DEBUG(
            8163001,
            5,
            "Failed to simplify the expression since the simplified expression is not valid.",
            "simplifiedExpression"_attr = redact(root->debugString()));
        return boost::none;
    }

    // The simplification ran until the end, so it completed with  a simplified formula.
    expressionSimplifierMetrics.simplifiedCount.incrementRelaxed();

    LOGV2_DEBUG(7767002,
                5,
                "Simplified MatchExpression",
                "expression"_attr = redact(simplifiedExpression->debugString()));
    return simplifiedExpression;
}
}  // namespace mongo
