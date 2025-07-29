/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/db/query/compiler/rewrites/matcher/rewrite_expr.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

namespace {
bool validateFieldPathForExprInRewrite(const ExpressionFieldPath& fieldPathExpr) {
    return !fieldPathExpr.isVariableReference() && !fieldPathExpr.isROOT();
}

template <bool wrapConstInArray>
std::unique_ptr<InMatchExpression> rewriteExprInMatchExpression(
    const ExpressionFieldPath& fieldPathExpr,
    const ExpressionConstant& literalExpr,
    std::vector<BSONObj>& cache) {
    // Build expression BSON.
    auto fieldPath = fieldPathExpr.getFieldPath().tail().fullPath();
    auto bob = BSONObjBuilder{};
    if constexpr (wrapConstInArray) {
        bob << fieldPath << BSON_ARRAY(literalExpr.getValue());
    } else {
        bob << fieldPath << literalExpr.getValue();
    }
    auto inObj = bob.obj();

    // Create $in MatchExpression.
    auto fieldName = inObj.firstElementFieldNameStringData();
    auto inMatch = std::make_unique<InMatchExpression>(fieldName);
    uassertStatusOK(inMatch->setEqualitiesArray(inObj.getField(fieldName).Obj()));

    // Cache the backing BSON for the MatchExpression so it doesn't get destroyed while we still
    // need it.
    cache.push_back(std::move(inObj));

    return inMatch;
};
}  // namespace

using CmpOp = ExpressionCompare::CmpOp;

RewriteExpr::RewriteResult RewriteExpr::rewrite(const boost::intrusive_ptr<Expression>& expression,
                                                const CollatorInterface* collator) {
    LOGV2_DEBUG(20725,
                5,
                "Expression prior to rewrite",
                "expression"_attr = redact(expression->serialize().toString()));

    RewriteExpr rewriteExpr(collator);
    std::unique_ptr<MatchExpression> matchExpression;

    if (auto matchTree = rewriteExpr._rewriteExpression(expression)) {
        matchExpression = std::move(matchTree);
        LOGV2_DEBUG(20726,
                    5,
                    "Post-rewrite MatchExpression",
                    "expression"_attr = redact(matchExpression->debugString()));
        // The Boolean simplifier is disabled since we don't want to simplify sub-expressions, but
        // simplify the whole expression instead.
        matchExpression =
            optimizeMatchExpression(std::move(matchExpression), /* enableSimplification */ false);
        LOGV2_DEBUG(20727,
                    5,
                    "Post-rewrite/post-optimized MatchExpression",
                    "expression"_attr = redact(matchExpression->debugString()));
    }

    return {std::move(matchExpression),
            std::move(rewriteExpr._matchExprElemStorage),
            rewriteExpr._allSubExpressionsRewritten};
}

std::unique_ptr<MatchExpression> RewriteExpr::_rewriteExpression(
    const boost::intrusive_ptr<Expression>& currExprNode) {

    if (auto expr = dynamic_cast<ExpressionAnd*>(currExprNode.get())) {
        return _rewriteAndExpression(expr);
    } else if (auto expr = dynamic_cast<ExpressionOr*>(currExprNode.get())) {
        return _rewriteOrExpression(expr);
    } else if (auto expr = dynamic_cast<ExpressionCompare*>(currExprNode.get())) {
        return _rewriteComparisonExpression(expr);
    } else if (auto expr = dynamic_cast<ExpressionIn*>(currExprNode.get())) {
        return _rewriteInExpression(expr);
    }

    return nullptr;
}

std::unique_ptr<MatchExpression> RewriteExpr::_rewriteAndExpression(
    const boost::intrusive_ptr<ExpressionAnd>& currExprNode) {

    auto andMatch = std::make_unique<AndMatchExpression>();

    for (auto&& child : currExprNode->getOperandList())
        if (auto childMatch = _rewriteExpression(child)) {
            andMatch->add(std::move(childMatch));
        } else {
            _allSubExpressionsRewritten = false;
        }

    if (andMatch->numChildren() > 0)
        return andMatch;

    return nullptr;
}

std::unique_ptr<MatchExpression> RewriteExpr::_rewriteOrExpression(
    const boost::intrusive_ptr<ExpressionOr>& currExprNode) {

    auto orMatch = std::make_unique<OrMatchExpression>();
    for (auto&& child : currExprNode->getOperandList())
        if (auto childExpr = _rewriteExpression(child))
            orMatch->add(std::move(childExpr));
        else {
            // If any child cannot be rewritten to a MatchExpression then we must abandon adding
            // this $or clause.
            _allSubExpressionsRewritten = false;
            return nullptr;
        }

    if (orMatch->numChildren() > 0)
        return orMatch;

    return nullptr;
}

std::unique_ptr<MatchExpression> RewriteExpr::_rewriteComparisonExpression(
    const boost::intrusive_ptr<ExpressionCompare>& expr) {

    if (!_canRewriteComparison(expr)) {
        return nullptr;
    }

    const auto& operandList = expr->getOperandList();
    invariant(operandList.size() == 2);

    ExpressionFieldPath* lhs{nullptr};
    ExpressionConstant* rhs{nullptr};
    CmpOp cmpOperator = expr->getOp();

    // Extract left-hand side and right-hand side MatchExpression components.
    if ((lhs = dynamic_cast<ExpressionFieldPath*>(operandList[0].get()))) {
        rhs = dynamic_cast<ExpressionConstant*>(operandList[1].get());
        invariant(rhs);
    } else {
        lhs = dynamic_cast<ExpressionFieldPath*>(operandList[1].get());
        rhs = dynamic_cast<ExpressionConstant*>(operandList[0].get());
        invariant(lhs && rhs);

        // The MatchExpression is normalized so that the field path expression is on the left. For
        // cases like {$gt: [1, "$x"]} where the order of the child expressions matter, we also
        // change the comparison operator.
        switch (cmpOperator) {
            case ExpressionCompare::GT: {
                cmpOperator = ExpressionCompare::LT;
                break;
            }
            case ExpressionCompare::GTE: {
                cmpOperator = ExpressionCompare::LTE;
                break;
            }
            case ExpressionCompare::LT: {
                cmpOperator = ExpressionCompare::GT;
                break;
            }
            case ExpressionCompare::LTE: {
                cmpOperator = ExpressionCompare::GTE;
                break;
            }
            case ExpressionCompare::EQ:
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(3994306);
        }
    }

    // Build argument for ComparisonMatchExpression.
    const auto fieldPath = lhs->getFieldPath().tail();
    BSONObjBuilder bob;
    bob << fieldPath.fullPath() << rhs->getValue();
    auto cmpObj = bob.obj();
    _matchExprElemStorage.push_back(cmpObj);

    return _buildComparisonMatchExpression(cmpOperator, cmpObj.firstElement());
}

std::unique_ptr<MatchExpression> RewriteExpr::_buildComparisonMatchExpression(
    ExpressionCompare::CmpOp comparisonOp, BSONElement fieldAndValue) {
    tassert(3994301,
            "comparisonOp must be one of the following: $eq, $gt, $gte, $lt, $lte",
            comparisonOp == ExpressionCompare::EQ || comparisonOp == ExpressionCompare::GT ||
                comparisonOp == ExpressionCompare::GTE || comparisonOp == ExpressionCompare::LT ||
                comparisonOp == ExpressionCompare::LTE);

    std::unique_ptr<MatchExpression> matchExpr;

    switch (comparisonOp) {
        case ExpressionCompare::EQ: {
            matchExpr = std::make_unique<InternalExprEqMatchExpression>(
                fieldAndValue.fieldNameStringData(), fieldAndValue);
            break;
        }
        case ExpressionCompare::GT: {
            matchExpr = std::make_unique<InternalExprGTMatchExpression>(
                fieldAndValue.fieldNameStringData(), fieldAndValue);
            break;
        }
        case ExpressionCompare::GTE: {
            matchExpr = std::make_unique<InternalExprGTEMatchExpression>(
                fieldAndValue.fieldNameStringData(), fieldAndValue);
            break;
        }
        case ExpressionCompare::LT: {
            matchExpr = std::make_unique<InternalExprLTMatchExpression>(
                fieldAndValue.fieldNameStringData(), fieldAndValue);
            break;
        }
        case ExpressionCompare::LTE: {
            matchExpr = std::make_unique<InternalExprLTEMatchExpression>(
                fieldAndValue.fieldNameStringData(), fieldAndValue);
            break;
        }
        default:
            MONGO_UNREACHABLE_TASSERT(3994307);
    }
    matchExpr->setCollator(_collator);

    return matchExpr;
}

bool RewriteExpr::_canRewriteComparison(
    const boost::intrusive_ptr<ExpressionCompare>& expression) const {

    // Currently we only rewrite $eq, $gt, $gte, $lt and $lte expressions.
    auto op = expression->getOp();
    if (op != ExpressionCompare::EQ && op != ExpressionCompare::GT &&
        op != ExpressionCompare::GTE && op != ExpressionCompare::LT &&
        op != ExpressionCompare::LTE) {
        return false;
    }

    const auto& operandList = expression->getOperandList();
    bool hasFieldPath = false;

    for (const auto& operand : operandList) {
        if (auto exprFieldPath = dynamic_cast<ExpressionFieldPath*>(operand.get())) {
            if (exprFieldPath->isVariableReference() || exprFieldPath->isROOT()) {
                // Rather than a local document field path, this field path refers to either a
                // variable or the full document itself. Neither of which can be expressed in the
                // match language.
                return false;
            }

            if (hasFieldPath) {
                // Match does not allow for more than one local document field path.
                return false;
            }

            hasFieldPath = true;
        } else if (auto exprConst = dynamic_cast<ExpressionConstant*>(operand.get())) {
            switch (exprConst->getValue().getType()) {
                case BSONType::array:
                case BSONType::eoo:
                case BSONType::undefined:
                    return false;
                default:
                    break;
            }
        } else {
            return false;
        }
    }

    return hasFieldPath;
}

/**
 * Create the MatchExpression equivalent of the $expr $in.
 * Cases:
 *   - {$expr: {$in: ["$lhsFieldPath", rhsConstArray]}} --> {"lhsFieldPath": {$in:
 * rhsConstArray}}
 *   - {$expr: {$in:[lhsConst, "$rhsFieldPath"]}} --> {"rhsFieldPath": {$in: [lhsConst]}}
 * This is the first level of filtering that can take advantage of indexes.
 * It may return a superset of results because MatchExpressions have implicit array
 * traversal semantics that are not present in agg. The original predicate is maintained in
 * the second level of filtering for correctness.
 */
std::unique_ptr<MatchExpression> RewriteExpr::_rewriteInExpression(
    const boost::intrusive_ptr<ExpressionIn>& expr) {

    const auto& operandList = expr->getOperandList();
    invariant(operandList.size() == 2);

    auto lhs = operandList[0].get();
    auto rhs = operandList[1].get();

    // Validate lhs, which must be a field path.
    auto lhsFieldPath = dynamic_cast<ExpressionFieldPath*>(lhs);
    if (!lhsFieldPath) {
        if (internalQueryExtraPredicateForReversedIn.load() ||
            expr->getExpressionContext()->isFleQuery()) {
            /**
             * If we have a FLE query, we are guaranteed to have an expression that may generate a
             * $in for its FLE tag disjunction. In this case we want the rewrite to take place even
             * in the absence of the internalQueryExtraPredicateForReversedIn query knob. Not
             * performing the rewrite for FLE queries will prevent them from being able to take
             *advantage of the index on the '__safeContent__' array, and has signficant performance
             * implications.
             */
            if (auto* lhsConst = dynamic_cast<ExpressionConstant*>(lhs); lhsConst) {
                auto* rhsFieldPath = dynamic_cast<ExpressionFieldPath*>(rhs);
                if (rhsFieldPath && validateFieldPathForExprInRewrite(*rhsFieldPath)) {
                    if (lhsConst->getValue().getType() == BSONType::regEx) {
                        // Would trigger BadValue: Cannot insert regex into InListData.
                        return nullptr;
                    }
                    // We don't need to place additional restrictions on 'rhs' here, because $in
                    // would error out for a non-array value of 'rhs'- similarly, 'lhs' will become
                    // an array, so we don't need th same checks as in the case below.
                    return rewriteExprInMatchExpression<true /* wrapConstInArray */>(
                        *rhsFieldPath, *lhsConst, _matchExprElemStorage);
                }
            }
        }
        return nullptr;
    } else if (!validateFieldPathForExprInRewrite(*lhsFieldPath)) {
        // Rather than a local document field path, this field path refers to either a variable or
        // the full document itself. Neither of which can be expressed in the match language.
        return nullptr;
    }

    // Validate rhs, which must be a static constant array.
    auto rhsConst = dynamic_cast<ExpressionConstant*>(rhs);
    if (!rhsConst) {
        return nullptr;
    } else {
        const auto& rhsVal = rhsConst->getValue();
        if (rhsVal.getType() != BSONType::array) {
            return nullptr;
        }

        // If any of the following types are present in the $in array, the expression is ineligible
        // for the rewrite because the semantics are different between MatchExpression and agg.
        //
        // - Array: MatchExpressions have implicit array traversal semantics, while agg doesn't.
        // - Null: MatchExpressions will also match on missing values, while agg only matches on an
        // explicit "null".
        // - Regex: MatchExpressions will evaluate the regex, while agg only matches the exact regex
        // (i.e. a field with value /clothing/, but not "clothings").
        for (const auto& el : rhsVal.getArray()) {
            switch (el.getType()) {
                case BSONType::array:
                case BSONType::null:
                case BSONType::undefined:
                case BSONType::regEx:
                    return nullptr;
                default:
                    break;
            }
        }
    }

    return rewriteExprInMatchExpression<false /* wrapConstInArray */>(
        *lhsFieldPath, *rhsConst, _matchExprElemStorage);
}
}  // namespace mongo
