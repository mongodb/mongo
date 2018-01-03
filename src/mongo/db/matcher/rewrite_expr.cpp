/*-
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/rewrite_expr.h"

#include "mongo/db/matcher/expression_internal_expr_eq.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/util/log.h"

namespace mongo {

using CmpOp = ExpressionCompare::CmpOp;

RewriteExpr::RewriteResult RewriteExpr::rewrite(const boost::intrusive_ptr<Expression>& expression,
                                                const CollatorInterface* collator) {
    LOG(5) << "Expression prior to rewrite: " << expression->serialize(false);

    RewriteExpr rewriteExpr(collator);
    std::unique_ptr<MatchExpression> matchExpression;

    if (auto matchTree = rewriteExpr._rewriteExpression(expression)) {
        matchExpression = std::move(matchTree);
        LOG(5) << "Post-rewrite MatchExpression: " << matchExpression->toString();
        matchExpression = MatchExpression::optimize(std::move(matchExpression));
        LOG(5) << "Post-rewrite/post-optimized MatchExpression: " << matchExpression->toString();
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

    auto andMatch = stdx::make_unique<AndMatchExpression>();

    for (auto&& child : currExprNode->getOperandList()) {
        if (auto childMatch = _rewriteExpression(child)) {
            andMatch->add(childMatch.release());
        }
    }

    if (andMatch->numChildren() > 0) {
        return std::move(andMatch);
    }

    return nullptr;
}

std::unique_ptr<MatchExpression> RewriteExpr::_rewriteOrExpression(
    const boost::intrusive_ptr<ExpressionOr>& currExprNode) {

    auto orMatch = stdx::make_unique<OrMatchExpression>();
    for (auto&& child : currExprNode->getOperandList()) {
        if (auto childExpr = _rewriteExpression(child)) {
            orMatch->add(childExpr.release());
        } else {
            // If any child cannot be rewritten to a MatchExpression then we must abandon adding
            // this $or clause.
            return nullptr;
        }
    }

    if (orMatch->numChildren() > 0) {
        return std::move(orMatch);
    }

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
    invariant(comparisonOp == ExpressionCompare::EQ);

    auto eqMatchExpr = stdx::make_unique<InternalExprEqMatchExpression>();
    invariantOK(eqMatchExpr->init(fieldAndValue.fieldName(), fieldAndValue));
    eqMatchExpr->setCollator(_collator);

    return std::move(eqMatchExpr);
}

bool RewriteExpr::_canRewriteComparison(
    const boost::intrusive_ptr<ExpressionCompare>& expression) const {

    // Currently we only rewrite $eq expressions.
    if (expression->getOp() != ExpressionCompare::EQ) {
        return false;
    }

    const auto& operandList = expression->getOperandList();
    bool hasFieldPath = false;

    for (auto operand : operandList) {
        if (auto exprFieldPath = dynamic_cast<ExpressionFieldPath*>(operand.get())) {
            if (!exprFieldPath->isRootFieldPath()) {
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
