/**
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

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/expression_expr.h"

namespace mongo {
ExprMatchExpression::ExprMatchExpression(boost::intrusive_ptr<Expression> expr,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : MatchExpression(MatchType::EXPRESSION), _expCtx(expCtx), _expression(expr) {}

ExprMatchExpression::ExprMatchExpression(BSONElement elem,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : ExprMatchExpression(Expression::parseOperand(expCtx, elem, expCtx->variablesParseState),
                          expCtx) {}

bool ExprMatchExpression::matches(const MatchableDocument* doc, MatchDetails* details) const {
    if (_rewriteResult && _rewriteResult->matchExpression() &&
        !_rewriteResult->matchExpression()->matches(doc, details)) {
        return false;
    }

    Document document(doc->toBSON());
    auto value = _expression->evaluate(document);
    return value.coerceToBool();
}

void ExprMatchExpression::serialize(BSONObjBuilder* out) const {
    *out << "$expr" << _expression->serialize(false);
}

bool ExprMatchExpression::equivalent(const MatchExpression* other) const {
    if (other->matchType() != matchType()) {
        return false;
    }

    const ExprMatchExpression* realOther = static_cast<const ExprMatchExpression*>(other);

    if (!CollatorInterface::collatorsMatch(_expCtx->getCollator(),
                                           realOther->_expCtx->getCollator())) {
        return false;
    }

    // TODO SERVER-30982: Add mechanism to allow for checking Expression equivalency.
    return ValueComparator().evaluate(_expression->serialize(false) ==
                                      realOther->_expression->serialize(false));
}

void ExprMatchExpression::_doSetCollator(const CollatorInterface* collator) {
    _expCtx->setCollator(collator);

    if (_rewriteResult && _rewriteResult->matchExpression()) {
        _rewriteResult->matchExpression()->setCollator(collator);
    }
}


std::unique_ptr<MatchExpression> ExprMatchExpression::shallowClone() const {
    // TODO SERVER-31003: Replace Expression clone via serialization with Expression::clone().
    BSONObjBuilder bob;
    bob << "" << _expression->serialize(false);
    boost::intrusive_ptr<Expression> clonedExpr =
        Expression::parseOperand(_expCtx, bob.obj().firstElement(), _expCtx->variablesParseState);

    auto clone = stdx::make_unique<ExprMatchExpression>(std::move(clonedExpr), _expCtx);
    if (_rewriteResult) {
        clone->_rewriteResult = _rewriteResult->clone();
    }
    return std::move(clone);
}

MatchExpression::ExpressionOptimizerFunc ExprMatchExpression::getOptimizer() const {
    return [](std::unique_ptr<MatchExpression> expression) {
        auto& exprMatchExpr = static_cast<ExprMatchExpression&>(*expression);

        // If '_expression' can be rewritten to a MatchExpression, we will return a $and node with
        // both the original ExprMatchExpression and the MatchExpression rewrite as children.
        // Exiting early prevents additional calls to optimize from performing additional rewrites
        // and adding duplicate MatchExpression sub-trees to the tree.
        if (exprMatchExpr._rewriteResult) {
            return expression;
        }

        exprMatchExpr._expression = exprMatchExpr._expression->optimize();
        exprMatchExpr._rewriteResult =
            RewriteExpr::rewrite(exprMatchExpr._expression, exprMatchExpr._expCtx->getCollator());

        if (exprMatchExpr._rewriteResult->matchExpression()) {
            auto andMatch = stdx::make_unique<AndMatchExpression>();
            andMatch->add(exprMatchExpr._rewriteResult->releaseMatchExpression().release());
            andMatch->add(expression.release());
            expression = std::move(andMatch);
        }

        return expression;
    };
}
}  // namespace mongo
