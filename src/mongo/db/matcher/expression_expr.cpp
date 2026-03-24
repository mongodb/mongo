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

#include "mongo/db/matcher/expression_expr.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_internal_eq_hashed_key.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/platform/compiler.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

ExprMatchExpression::ExprMatchExpression(boost::intrusive_ptr<Expression> expr,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         clonable_ptr<ErrorAnnotation> annotation)
    : MatchExpression(MatchType::EXPRESSION, std::move(annotation)),
      _expCtx(expCtx),
      _expression(expr) {
    setPathForEquality(expr);
}

ExprMatchExpression::ExprMatchExpression(BSONElement elem,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         clonable_ptr<ErrorAnnotation> annotation)
    : ExprMatchExpression(Expression::parseOperand(expCtx.get(), elem, expCtx->variablesParseState),
                          expCtx,
                          std::move(annotation)) {}

void ExprMatchExpression::serialize(BSONObjBuilder* out,
                                    const SerializationOptions& opts,
                                    bool includePath) const {
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
    return ValueComparator().evaluate(_expression->serialize() ==
                                      realOther->_expression->serialize());
}

void ExprMatchExpression::_doSetCollator(const CollatorInterface* collator) {
    // This function is used to give match expression nodes which don't keep a pointer to the
    // ExpressionContext access to the ExpressionContext's collator. Since the operation only ever
    // has a single CollatorInterface, and since that collator is kept on the ExpressionContext,
    // the collator pointer that we're propagating throughout the MatchExpression tree must match
    // the one inside the ExpressionContext.
    tassert(11052404,
            "The MatchExpression collator must match the ExpressionContext one",
            collator == _expCtx->getCollator());
    if (_rewriteResult && _rewriteResult->matchExpression()) {
        _rewriteResult->matchExpression()->setCollator(collator);
    }
}


std::unique_ptr<MatchExpression> ExprMatchExpression::clone() const {
    boost::intrusive_ptr<Expression> clonedExpr = _expression->clone();
    auto clone =
        std::make_unique<ExprMatchExpression>(std::move(clonedExpr), _expCtx, _errorAnnotation);
    if (_rewriteResult) {
        clone->_rewriteResult = _rewriteResult->clone();
    }
    if (getTag()) {
        clone->setTag(getTag()->clone());
    }
    if (_path) {
        clone->_path = _path;
    }
    if (_backingBSON) {
        clone->_backingBSON = _backingBSON->getOwned();
    }
    return clone;
}

bool ExprMatchExpression::isTriviallyTrue() const {
    auto exprConst = dynamic_cast<ExpressionConstant*>(_expression.get());
    return exprConst && exprConst->getValue().coerceToBool();
}

bool ExprMatchExpression::isTriviallyFalse() const {
    auto exprConst = dynamic_cast<ExpressionConstant*>(_expression.get());
    return exprConst && !exprConst->getValue().coerceToBool();
}

namespace {
/**
 * Helper function to extract path and constant from equality comparison expression.
 */
std::pair<ExpressionFieldPath*, ExpressionConstant*> extractPathAndConstant(
    boost::intrusive_ptr<ExpressionCompare> eqExpression) {
    const auto& children = eqExpression->getChildren();
    uassert(4125200, "$eq should have 2 children", children.size() == 2ul);

    ExpressionFieldPath* ePath{nullptr};
    ExpressionConstant* eConst{nullptr};
    if ((ePath = dynamic_cast<ExpressionFieldPath*>(children[0].get()))) {
        eConst = dynamic_cast<ExpressionConstant*>(children[1].get());
    } else {
        ePath = dynamic_cast<ExpressionFieldPath*>(children[1].get());
        eConst = dynamic_cast<ExpressionConstant*>(children[0].get());
    }
    return std::make_pair(ePath, eConst);
}
}  // namespace

void ExprMatchExpression::setPathForEquality(boost::intrusive_ptr<Expression> expression) {
    if (auto eq = dynamic_cast<ExpressionCompare*>(expression.get());
        eq && eq->getOp() == ExpressionCompare::EQ) {

        // Extract left-hand side and right-hand side of the Expression.
        auto [ePath, eConst] = extractPathAndConstant(eq);

        if (ePath == nullptr || ePath->isROOT() || ePath->isVariableReference() ||
            eConst == nullptr || eConst->getValue().nullish() || eConst->getValue().isArray()) {
            return;
        }
        _path = ePath->getFieldPath().tail().fullPath();
        _backingBSON = BSON(_path.get() << eConst->getValue());
    }
}

boost::optional<BSONElement> ExprMatchExpression::getData() const {
    if (_backingBSON) {
        return _backingBSON.get().firstElement();
    }
    return boost::none;
}

}  // namespace mongo
