// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/matcher/expression_where_noop.h"

namespace mongo {

namespace exec::matcher {

void MatchExpressionEvaluator::visit(const WhereMatchExpression* expr) {
    _result = WhereMatchExpressionBase::evaluateWherePredicate(expr, _doc->toBSON());
}

void MatchExpressionEvaluator::visit(const WhereNoOpMatchExpression* expr) {
    MONGO_UNREACHABLE_TASSERT(9713603);
}

void MatchesSingleElementEvaluator::visit(const WhereMatchExpression* expr) {
    // This expression should only be used to match full documents
    MONGO_UNREACHABLE_TASSERT(10071000);
}

void MatchesSingleElementEvaluator::visit(const WhereNoOpMatchExpression* expr) {
    MONGO_UNREACHABLE_TASSERT(10071001);
}

}  // namespace exec::matcher
}  // namespace mongo
