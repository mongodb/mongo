/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "query_rewriter.h"

#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/fle/range_validator.h"

namespace mongo::fle {

class ExpressionRewriter {
public:
    ExpressionRewriter(QueryRewriter* queryRewriter, const ExpressionToRewriteMap& exprRewrites)
        : queryRewriter(queryRewriter), exprRewrites(exprRewrites){};

    std::unique_ptr<Expression> postVisit(Expression* exp) {
        if (auto rewriteEntry = exprRewrites.find(typeid(*exp));
            rewriteEntry != exprRewrites.end()) {
            for (auto& rewrite : rewriteEntry->second) {
                auto expr = rewrite(queryRewriter, exp);
                if (expr != nullptr) {
                    didRewrite = true;
                    return expr;
                }
            }
        }
        return nullptr;
    }

    QueryRewriter* queryRewriter;
    const ExpressionToRewriteMap& exprRewrites;
    bool didRewrite = false;
};

std::unique_ptr<Expression> QueryRewriter::rewriteExpression(Expression* expression) {
    tassert(6334104, "Expected an expression to rewrite but found none", expression);

    ExpressionRewriter expressionRewriter{this, this->_exprRewrites};
    auto res = expression_walker::walk<Expression>(expression, &expressionRewriter);
    _rewroteLastExpression = expressionRewriter.didRewrite;
    return res;
}

boost::optional<BSONObj> QueryRewriter::rewriteMatchExpression(const BSONObj& filter) {
    auto expr = uassertStatusOK(MatchExpressionParser::parse(filter, _expCtx));
    validateRanges(*expr.get());

    _rewroteLastExpression = false;
    if (auto res = _rewrite(expr.get())) {
        // The rewrite resulted in top-level changes. Serialize the new expression.
        return res->serialize().getOwned();
    } else if (_rewroteLastExpression) {
        // The rewrite had no top-level changes, but nested expressions were rewritten. Serialize
        // the parsed expression, which has in-place changes.
        return expr->serialize().getOwned();
    }

    // No rewrites were done.
    return boost::none;
}

std::unique_ptr<MatchExpression> QueryRewriter::_rewrite(MatchExpression* expr) {
    switch (expr->matchType()) {
        case MatchExpression::AND:
        case MatchExpression::OR:
        case MatchExpression::NOT:
        case MatchExpression::NOR: {
            for (size_t i = 0; i < expr->numChildren(); i++) {
                auto child = expr->getChild(i);
                if (auto newChild = _rewrite(child)) {
                    expr->resetChild(i, newChild.release());
                }
            }
            return nullptr;
        }
        case MatchExpression::EXPRESSION: {
            // Save the current value of _rewroteLastExpression, since rewriteExpression() may
            // reset it to false and we may have already done a match expression rewrite.
            auto didRewrite = _rewroteLastExpression;
            auto rewritten =
                rewriteExpression(static_cast<ExprMatchExpression*>(expr)->getExpression().get());
            _rewroteLastExpression |= didRewrite;
            if (rewritten) {
                return std::make_unique<ExprMatchExpression>(rewritten.release(),
                                                             getExpressionContext());
            }
            return nullptr;
        }
        default: {
            if (auto rewriteEntry = _matchRewrites.find(expr->matchType());
                rewriteEntry != _matchRewrites.end()) {
                for (auto& rewrite : rewriteEntry->second) {
                    auto rewritten = rewrite(this, expr);
                    // Only one rewrite can be applied to an expression, so return as soon as a
                    // rewrite returns something other than nullptr.
                    if (rewritten != nullptr) {
                        _rewroteLastExpression = true;
                        return rewritten;
                    }
                }
            }
            return nullptr;
        }
    }
}
}  // namespace mongo::fle
