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

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <utility>

#include <boost/move/utility_core.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_internal_eq_hashed_key.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/fail_point.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(ExprMatchExpressionMatchesReturnsFalseOnException);

ExprMatchExpression::ExprMatchExpression(boost::intrusive_ptr<Expression> expr,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         clonable_ptr<ErrorAnnotation> annotation)
    : MatchExpression(MatchType::EXPRESSION, std::move(annotation)),
      _expCtx(expCtx),
      _expression(expr) {}

ExprMatchExpression::ExprMatchExpression(BSONElement elem,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         clonable_ptr<ErrorAnnotation> annotation)
    : ExprMatchExpression(Expression::parseOperand(expCtx.get(), elem, expCtx->variablesParseState),
                          expCtx,
                          std::move(annotation)) {}

bool ExprMatchExpression::matches(const MatchableDocument* doc, MatchDetails* details) const {
    if (_rewriteResult && _rewriteResult->matchExpression() &&
        !_rewriteResult->matchExpression()->matches(doc, details)) {
        return false;
    }
    try {
        return evaluateExpression(doc).coerceToBool();
    } catch (const DBException&) {
        if (MONGO_unlikely(ExprMatchExpressionMatchesReturnsFalseOnException.shouldFail())) {
            return false;
        }

        throw;
    }
}

Value ExprMatchExpression::evaluateExpression(const MatchableDocument* doc) const {
    Document document(doc->toBSON());

    // 'Variables' is not thread safe, and ExprMatchExpression may be used in a validator which
    // processes documents from multiple threads simultaneously. Hence we make a copy of the
    // 'Variables' object per-caller.
    Variables variables = _expCtx->variables;
    return _expression->evaluate(document, &variables);
}

void ExprMatchExpression::serialize(BSONObjBuilder* out, const SerializationOptions& opts) const {
    *out << "$expr" << _expression->serialize(opts);
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
    return ValueComparator().evaluate(_expression->serialize(SerializationOptions{}) ==
                                      realOther->_expression->serialize(SerializationOptions{}));
}

void ExprMatchExpression::_doSetCollator(const CollatorInterface* collator) {
    // This function is used to give match expression nodes which don't keep a pointer to the
    // ExpressionContext access to the ExpressionContext's collator. Since the operation only ever
    // has a single CollatorInterface, and since that collator is kept on the ExpressionContext,
    // the collator pointer that we're propagating throughout the MatchExpression tree must match
    // the one inside the ExpressionContext.
    invariant(collator == _expCtx->getCollator());
    if (_rewriteResult && _rewriteResult->matchExpression()) {
        _rewriteResult->matchExpression()->setCollator(collator);
    }
}


std::unique_ptr<MatchExpression> ExprMatchExpression::clone() const {
    // TODO SERVER-31003: Replace Expression clone via serialization with Expression::clone().
    BSONObjBuilder bob;
    bob << "" << _expression->serialize(SerializationOptions{});
    boost::intrusive_ptr<Expression> clonedExpr = Expression::parseOperand(
        _expCtx.get(), bob.obj().firstElement(), _expCtx->variablesParseState);

    auto clone =
        std::make_unique<ExprMatchExpression>(std::move(clonedExpr), _expCtx, _errorAnnotation);
    if (_rewriteResult) {
        clone->_rewriteResult = _rewriteResult->clone();
    }
    return clone;
}

bool ExprMatchExpression::isTriviallyTrue() const {
    auto exprConst = dynamic_cast<ExpressionConstant*>(_expression.get());
    return exprConst && exprConst->getValue().coerceToBool();
}

namespace {

// Return nullptr on failure.
std::unique_ptr<MatchExpression> attemptToRewriteEqHash(ExprMatchExpression& expr) {
    auto childExpr = expr.getExpression();

    // Looking for:
    //                     $eq
    //    $toHashedIndexKey   {$const: NumberLong(?)}
    //           "$a"
    //
    // Where "a" can be any field path and ? can be any number.
    if (auto eq = dynamic_cast<ExpressionCompare*>(childExpr.get());
        eq && eq->getOp() == ExpressionCompare::CmpOp::EQ) {
        const auto& children = eq->getChildren();
        tassert(7281406, "should have 2 $eq children", children.size() == 2ul);

        auto eqFirst = children[0].get();
        auto eqSecond = children[1].get();
        if (auto hashingExpr = dynamic_cast<ExpressionToHashedIndexKey*>(eqFirst)) {
            // Matched $toHashedIndexKey - keep going.
            tassert(7281407,
                    "should have 1 $toHashedIndexKey child",
                    hashingExpr->getChildren().size() == 1ul);
            auto hashChild = hashingExpr->getChildren()[0].get();

            if (auto fieldPath = dynamic_cast<ExpressionFieldPath*>(hashChild);
                fieldPath && !fieldPath->isVariableReference() && !fieldPath->isROOT()) {
                auto path = fieldPath->getFieldPathWithoutCurrentPrefix();

                // Matched "$a" in the example above! Now look for the constant long:
                if (auto constant = dynamic_cast<ExpressionConstant*>(eqSecond);
                    constant && constant->getValue().getType() == BSONType::NumberLong) {
                    long long hashTarget = constant->getValue().getLong();
                    return std::make_unique<InternalEqHashedKey>(path.fullPath(), hashTarget);
                }
            }
        }
    }
    return nullptr;
}
}  // namespace

MatchExpression::ExpressionOptimizerFunc ExprMatchExpression::getOptimizer() const {
    return [](std::unique_ptr<MatchExpression> expression) {
        auto& exprMatchExpr = static_cast<ExprMatchExpression&>(*expression);

        //  $expr expressions can't take advantage of indexes. We attempt to rewrite the expressions
        //  as a conjunction of internal match expressions, so the query planner can use the
        //  internal match expressions to potentially generate an index scan.
        // Exiting early prevents additional calls to optimize from performing additional rewrites
        // and adding duplicate MatchExpression sub-trees to the tree.
        if (exprMatchExpr._rewriteResult) {
            return expression;
        }

        exprMatchExpr._expression = exprMatchExpr._expression->optimize();
        if (auto successfulEqHashRewrite = attemptToRewriteEqHash(exprMatchExpr)) {
            return successfulEqHashRewrite;
        }

        exprMatchExpr._rewriteResult =
            RewriteExpr::rewrite(exprMatchExpr._expression, exprMatchExpr._expCtx->getCollator());

        if (exprMatchExpr._rewriteResult->matchExpression()) {
            // If '_expression' can be rewritten to a MatchExpression, we will return a $and node
            // with both the original ExprMatchExpression and the MatchExpression rewrite as
            // children. The rewritten expression might not be equivalent to the original one so we
            // still have to keep the latter for correctness.
            auto andMatch = std::make_unique<AndMatchExpression>();
            andMatch->add(exprMatchExpr._rewriteResult->releaseMatchExpression());
            andMatch->add(std::move(expression));
            // Re-optimize the new AND in order to make sure that any AND children are absorbed.
            expression = MatchExpression::optimize(std::move(andMatch));
        }

        // Replace trivially true expression with an empty AND since the planner doesn't always
        // check for 'isTriviallyTrue()'.
        if (expression->isTriviallyTrue()) {
            expression = std::make_unique<AndMatchExpression>();
        }

        return expression;
    };
}
}  // namespace mongo
