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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/rewrite_expr.h"

#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/logv2/log.h"

namespace mongo {

using CmpOp = ExpressionCompare::CmpOp;

RewriteExpr::RewriteResult RewriteExpr::rewrite(const boost::intrusive_ptr<Expression>& expression,
                                                const CollatorInterface* collator) {
    LOGV2_DEBUG(
        20725, 5, "Expression prior to rewrite", "expression"_attr = expression->serialize(false));

    RewriteExpr rewriteExpr(collator);
    std::unique_ptr<MatchExpression> matchExpression;

    if (auto matchTree = rewriteExpr._rewriteExpression(expression)) {
        matchExpression = std::move(matchTree);
        LOGV2_DEBUG(20726,
                    5,
                    "Post-rewrite MatchExpression",
                    "expression"_attr = matchExpression->debugString());
        matchExpression = MatchExpression::optimize(std::move(matchExpression));
        LOGV2_DEBUG(20727,
                    5,
                    "Post-rewrite/post-optimized MatchExpression",
                    "expression"_attr = matchExpression->debugString());
    }

    return {std::move(matchExpression), std::move(rewriteExpr._matchExprElemStorage)};
}

std::unique_ptr<MatchExpression> RewriteExpr::_rewriteExpression(
    const boost::intrusive_ptr<Expression>& currExprNode) {

    if (auto expr = dynamic_cast<ExpressionAnd*>(currExprNode.get())) {
        return _rewriteAndExpression(expr);
    } else if (auto expr = dynamic_cast<ExpressionOr*>(currExprNode.get())) {
        return _rewriteOrExpression(expr);
    } else if (auto expr = dynamic_cast<ExpressionCompare*>(currExprNode.get())) {
        return _rewriteComparisonExpression(expr);
    }

    return nullptr;
}

std::unique_ptr<MatchExpression> RewriteExpr::_rewriteAndExpression(
    const boost::intrusive_ptr<ExpressionAnd>& currExprNode) {

    auto andMatch = std::make_unique<AndMatchExpression>();

    for (auto&& child : currExprNode->getOperandList())
        if (auto childMatch = _rewriteExpression(child))
            andMatch->add(std::move(childMatch));

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
        else
            // If any child cannot be rewritten to a MatchExpression then we must abandon adding
            // this $or clause.
            return nullptr;

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
            matchExpr = std::make_unique<InternalExprEqMatchExpression>(fieldAndValue.fieldName(),
                                                                        fieldAndValue);
            break;
        }
        case ExpressionCompare::GT: {
            matchExpr = std::make_unique<InternalExprGTMatchExpression>(fieldAndValue.fieldName(),
                                                                        fieldAndValue);
            break;
        }
        case ExpressionCompare::GTE: {
            matchExpr = std::make_unique<InternalExprGTEMatchExpression>(fieldAndValue.fieldName(),
                                                                         fieldAndValue);
            break;
        }
        case ExpressionCompare::LT: {
            matchExpr = std::make_unique<InternalExprLTMatchExpression>(fieldAndValue.fieldName(),
                                                                        fieldAndValue);
            break;
        }
        case ExpressionCompare::LTE: {
            matchExpr = std::make_unique<InternalExprLTEMatchExpression>(fieldAndValue.fieldName(),
                                                                         fieldAndValue);
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

    for (auto operand : operandList) {
        if (auto exprFieldPath = dynamic_cast<ExpressionFieldPath*>(operand.get())) {
            if (exprFieldPath->isVariableReference()) {
                // This field path refers to a variable rather than a local document field path.
                return false;
            }

            if (hasFieldPath) {
                // Match does not allow for more than one local document field path.
                return false;
            }

            hasFieldPath = true;
        } else if (auto exprConst = dynamic_cast<ExpressionConstant*>(operand.get())) {
            switch (exprConst->getValue().getType()) {
                case BSONType::Array:
                case BSONType::EOO:
                case BSONType::Undefined:
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
}  // namespace mongo
