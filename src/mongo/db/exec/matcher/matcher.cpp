/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/matcher/matcher.h"

#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_internal_eq_hashed_key.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/util/fail_point.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(ExprMatchExpressionMatchesReturnsFalseOnException);

namespace exec::matcher {

Value evaluateExpression(const ExprMatchExpression* expr, const MatchableDocument* doc) {
    Document document(doc->toBSON());

    // 'Variables' is not thread safe, and ExprMatchExpression may be used in a validator which
    // processes documents from multiple threads simultaneously. Hence we make a copy of the
    // 'Variables' object per-caller.
    Variables variables = expr->getExpressionContext()->variables;
    return expr->getExpression()->evaluate(document, &variables);
}

void MatchExpressionEvaluator::visit(const ExprMatchExpression* expr) {
    if (expr->getRewriteResult() && expr->getRewriteResult()->matchExpression() &&
        !matches(expr->getRewriteResult()->matchExpression(), _doc, _details)) {
        _result = false;
        return;
    }
    try {
        _result = evaluateExpression(expr, _doc).coerceToBool();
    } catch (const DBException&) {
        if (MONGO_unlikely(ExprMatchExpressionMatchesReturnsFalseOnException.shouldFail())) {
            _result = false;
            return;
        }

        throw;
    }
}

void MatchExpressionEvaluator::visit(const InternalEqHashedKey* expr) {
    // Sadly, we need to match EOO elements to null index keys, as a special case.
    if (!bson::extractElementAtDottedPath(_doc->toBSON(), expr->path())) {
        _result = BSONElementHasher::hash64(BSON("" << BSONNULL).firstElement(),
                                            BSONElementHasher::DEFAULT_HASH_SEED) ==
            expr->getData().numberLong();
        return;
    }

    // Otherwise, we let this traversal work for us.
    visitPathExpression(expr);
}

void MatchExpressionEvaluator::visit(const InternalSchemaCondMatchExpression* expr) {
    _result = matches(expr->condition(), _doc, _details)
        ? matches(expr->thenBranch(), _doc, _details)
        : matches(expr->elseBranch(), _doc, _details);
}

void MatchExpressionEvaluator::visit(const InternalSchemaMaxPropertiesMatchExpression* expr) {
    BSONObj obj = _doc->toBSON();
    _result = (obj.nFields() <= expr->numProperties());
}

void MatchExpressionEvaluator::visit(const InternalSchemaMinPropertiesMatchExpression* expr) {
    BSONObj obj = _doc->toBSON();
    _result = (obj.nFields() >= expr->numProperties());
}

void MatchExpressionEvaluator::visit(const InternalSchemaXorMatchExpression* expr) {
    MatchExpressionEvaluator childVisitor(_doc, nullptr);
    bool found = false;
    for (auto&& child : expr->getChildren()) {
        child->acceptVisitor(&childVisitor);
        if (childVisitor.getResult()) {
            if (found) {
                _result = false;
                return;
            }
            found = true;
        }
    }
    _result = found;
}

void MatchExpressionEvaluator::visit(const NotMatchExpression* expr) {
    _result = !matches(expr->getChild(0), _doc, nullptr);
}

void MatchExpressionEvaluator::visitPathExpression(const PathMatchExpression* expr) {
    tassert(9714100, "Access to incomplete PathMatchExpression.", expr->elementPath());
    MatchableDocument::IteratorHolder cursor(_doc, &*expr->elementPath());
    while (cursor->more()) {
        ElementIterator::Context e = cursor->next();
        if (!exec::matcher::matchesSingleElement(expr, e.element(), _details)) {
            continue;
        }
        if (_details && _details->needRecord() && !e.arrayOffset().eoo()) {
            _details->setElemMatchKey(e.arrayOffset().fieldName());
        }
        _result = true;
        return;
    }
    _result = false;
}

/**
 * The input BSONObj matches if:
 *  - each field that matches a regular expression in 'expr->getPatternProperties()' also matches
 *    the corresponding match expression; and
 *  - any field not contained in 'expr->getProperties()' nor matching a pattern in
 *    'expr->getPatternProperties()' matches the 'expr->getOtherwise()' match expression.
 */
bool matchesBSONObj(const InternalSchemaAllowedPropertiesMatchExpression* expr,
                    const BSONObj& obj) {
    for (auto&& property : obj) {
        bool checkOtherwise = true;
        for (auto&& constraint : expr->getPatternProperties()) {
            if (constraint.first.regex->matchView(property.fieldName())) {
                checkOtherwise = false;
                if (!matchesBSONElement(constraint.second->getFilter(), property)) {
                    return false;
                }
            }
        }

        if (checkOtherwise &&
            expr->getProperties().find(property.fieldNameStringData()) !=
                expr->getProperties().end()) {
            checkOtherwise = false;
        }

        if (checkOtherwise && !matchesBSONElement(expr->getOtherwise()->getFilter(), property)) {
            return false;
        }
    }
    return true;
}

void MatchExpressionEvaluator::visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) {
    _result = matchesBSONObj(expr, _doc->toBSON());
}

}  // namespace exec::matcher
}  // namespace mongo
