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

    return {std::move(matchExpression),
            std::move(rewriteExpr._matchExprStringStorage),
            std::move(rewriteExpr._matchExprElemStorage)};
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

std::unique_ptr<MatchExpression> RewriteExpr::_rewriteInExpression(
    const boost::intrusive_ptr<ExpressionIn>& currExprNode) {

    if (!_isValidMatchIn(currExprNode)) {
        return nullptr;
    }

    const auto& operandList = currExprNode->getOperandList();
    ExpressionFieldPath* lhsExpression = dynamic_cast<ExpressionFieldPath*>(operandList[0].get());
    BSONArrayBuilder arrBuilder;

    if (ExpressionArray* rhsArray = dynamic_cast<ExpressionArray*>(operandList[1].get())) {
        for (auto&& item : rhsArray->getOperandList()) {
            auto exprConst = dynamic_cast<ExpressionConstant*>(item.get());
            arrBuilder << exprConst->getValue();
        }
    } else {
        auto exprConst = dynamic_cast<ExpressionConstant*>(operandList[1].get());
        invariant(exprConst);

        auto valueArr = exprConst->getValue();
        invariant(valueArr.isArray());

        for (auto val : valueArr.getArray()) {
            arrBuilder << val;
        }
    }

    _matchExprStringStorage.push_back(lhsExpression->getFieldPath().tail().fullPath());
    const auto& fieldPath = _matchExprStringStorage.back();

    BSONObj inValues = arrBuilder.obj();
    _matchExprElemStorage.push_back(inValues);
    std::vector<BSONElement> elementList;
    inValues.elems(elementList);

    auto matchInExpr = stdx::make_unique<InMatchExpression>(fieldPath);
    matchInExpr->setCollator(_collator);
    uassertStatusOK(matchInExpr->setEqualities(elementList));

    return std::move(matchInExpr);
}

std::unique_ptr<MatchExpression> RewriteExpr::_rewriteComparisonExpression(
    const boost::intrusive_ptr<ExpressionCompare>& currExprNode) {

    if (!_isValidMatchComparison(currExprNode)) {
        return nullptr;
    }

    return _buildComparisonMatchExpression(currExprNode);
}

std::unique_ptr<MatchExpression> RewriteExpr::_buildComparisonMatchExpression(
    const boost::intrusive_ptr<ExpressionCompare>& expr) {
    const auto& operandList = expr->getOperandList();
    invariant(operandList.size() == 2);

    ExpressionFieldPath* lhs{nullptr};
    ExpressionConstant* rhs{nullptr};
    CmpOp cmpOperator = expr->getOp();

    // Build left-hand and right-hand MatchExpression components and modify the operator if
    // required.
    if ((lhs = dynamic_cast<ExpressionFieldPath*>(operandList[0].get()))) {
        rhs = dynamic_cast<ExpressionConstant*>(operandList[1].get());
        invariant(rhs);
    } else {
        lhs = dynamic_cast<ExpressionFieldPath*>(operandList[1].get());
        rhs = dynamic_cast<ExpressionConstant*>(operandList[0].get());
        invariant(lhs && rhs);

        // When converting an Expression that has a field path on the RHS we will need to move to
        // the LHS. This may require an inversion of the operator used as well.
        // For example:  {$gt: [3, '$foo']} => {foo: {$lt: 3}}
        switch (cmpOperator) {
            case ExpressionCompare::CmpOp::GT:
                cmpOperator = CmpOp::LT;
                break;
            case ExpressionCompare::CmpOp::GTE:
                cmpOperator = CmpOp::LTE;
                break;
            case ExpressionCompare::CmpOp::LT:
                cmpOperator = CmpOp::GT;
                break;
            case ExpressionCompare::CmpOp::LTE:
                cmpOperator = CmpOp::GTE;
                break;
            default:  // No need to convert EQ or NE. CMP is not valid for rewrite.
                break;
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

    std::unique_ptr<ComparisonMatchExpression> compMatchExpr;
    std::unique_ptr<NotMatchExpression> notMatchExpr;

    switch (comparisonOp) {
        case ExpressionCompare::EQ: {
            compMatchExpr = stdx::make_unique<EqualityMatchExpression>(fieldAndValue.fieldName(),
                                                                       fieldAndValue);
            break;
        }
        case ExpressionCompare::NE: {
            compMatchExpr = stdx::make_unique<EqualityMatchExpression>(fieldAndValue.fieldName(),
                                                                       fieldAndValue);
            notMatchExpr = stdx::make_unique<NotMatchExpression>(compMatchExpr.release());
            break;
        }
        case ExpressionCompare::GT: {
            compMatchExpr =
                stdx::make_unique<GTMatchExpression>(fieldAndValue.fieldName(), fieldAndValue);
            break;
        }
        case ExpressionCompare::GTE: {
            compMatchExpr =
                stdx::make_unique<GTEMatchExpression>(fieldAndValue.fieldName(), fieldAndValue);
            break;
        }
        case ExpressionCompare::LT: {
            compMatchExpr =
                stdx::make_unique<LTMatchExpression>(fieldAndValue.fieldName(), fieldAndValue);
            break;
        }
        case ExpressionCompare::LTE: {
            compMatchExpr =
                stdx::make_unique<LTEMatchExpression>(fieldAndValue.fieldName(), fieldAndValue);
            break;
        }
        default:
            MONGO_UNREACHABLE;
    }

    if (notMatchExpr) {
        notMatchExpr->setCollator(_collator);
        return std::move(notMatchExpr);
    }

    compMatchExpr->setCollator(_collator);
    return std::move(compMatchExpr);
}

bool RewriteExpr::_isValidFieldPath(const FieldPath& fieldPath) const {
    // We can only rewrite field paths that contain ROOT plus a field path of length 1. Expression
    // and MatchExpression treat dotted paths in a different manner and will not produce the same
    // results.
    return fieldPath.getPathLength() == 2;
}

bool RewriteExpr::_isValidMatchIn(const boost::intrusive_ptr<ExpressionIn>& expr) const {
    const auto& operandList = expr->getOperandList();

    auto fieldPathExpr = dynamic_cast<ExpressionFieldPath*>(operandList[0].get());
    if (!fieldPathExpr || !fieldPathExpr->isRootFieldPath()) {
        // A left-hand-side local document field path is required to translate to match.
        return false;
    }

    if (!_isValidFieldPath(fieldPathExpr->getFieldPath())) {
        return false;
    }

    if (ExpressionArray* rhsArray = dynamic_cast<ExpressionArray*>(operandList[1].get())) {
        for (auto&& item : rhsArray->getOperandList()) {
            if (!dynamic_cast<ExpressionConstant*>(item.get())) {
                // All array values must be constant.
                return false;
            }
        }

        return true;
    }

    if (ExpressionConstant* constArr = dynamic_cast<ExpressionConstant*>(operandList[1].get())) {
        return constArr->getValue().isArray();
    }

    return false;
}

bool RewriteExpr::_isValidMatchComparison(
    const boost::intrusive_ptr<ExpressionCompare>& expression) const {
    if (expression->getOp() == ExpressionCompare::CMP) {
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

            if (!_isValidFieldPath(exprFieldPath->getFieldPath())) {
                return false;
            }

            hasFieldPath = true;
        } else if (!dynamic_cast<ExpressionConstant*>(operand.get())) {
            return false;
        }
    }

    return hasFieldPath;
}
}  // namespace mongo
