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
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/fle/range_validator.h"
#include "mongo/db/query/fle/text_search_predicate.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::fle {

class ExpressionRewriter {
public:
    ExpressionRewriter(QueryRewriter* queryRewriter, const ExpressionToRewriteMap& exprRewrites)
        : queryRewriter(queryRewriter), exprRewrites(exprRewrites) {};

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

class EncryptedTextSearchExpressionDetector {
public:
    EncryptedTextSearchExpressionDetector(QueryRewriter& queryRewriter)
        : _queryRewriter(queryRewriter) {}

    std::unique_ptr<Expression> postVisit(Expression* exp) {
        // This detector only cares if we find at least one text search expression in the tree.
        // If we've already found one, we can return early.
        if (_detected) {
            return nullptr;
        }
        if (auto* encTextSearchExpr = dynamic_cast<ExpressionEncTextSearch*>(exp);
            encTextSearchExpr) {
            /**
             * _initializeTextSearchPredicate will return true if we got a valid TextSearchPredicate
             * we can call on. Note: in production, we will get a null TextSearchPredicate if the
             * feature flag was disabled.
             */
            if (_queryRewriter._initializeTextSearchPredicate() &&
                _queryRewriter._getEncryptedTextSearchPredicate().hasValidPayload(
                    *encTextSearchExpr))
                _detected = true;
        }
        return nullptr;
    }

    bool wasDetected() const {
        return _detected;
    }

private:
    QueryRewriter& _queryRewriter;
    bool _detected = false;
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

namespace {
/**
 * This function replaces an aggregation Expression $and/$or with a MatchExpression equivalent
 * $and/$or with the $expr pushed down to the sub-expressions. This is a safe conversion, because
 * $and/$or semantics are the same in find and agg. This function returns a nullptr when the node
 * being visited is not an $and/$or expression, indicating no rewrite for the node took place.
 *
 * For example:
 * 1) $expr: {$and: [{$encStrStartsWith: {...}}]} -> $and: [{$expr: {$encStrStartsWith: {}}}]
 * 2) $expr: {$or: [{$encStrStartsWith: {...}}]} -> $or: [{$expr: {$encStrStartsWith: {}}}]
 * 3) $expr: {$and: [{$and: {$encStrStartsWith: {...}}}]}
 *          -> {$and: [{$and: [{$expr:{$encStrStartsWith: {...}}}]}]}
 * 4) $expr: {$and: [{$or: {$encStrStartsWith: {...}}}]}
 *          -> {$and: [{$or: [{$expr:{$encStrStartsWith: {...}}}]}]}
 */
std::unique_ptr<MatchExpression> rewriteAggAndOrAsMatch(Expression* expr,
                                                        ExpressionContext* expCtx) {
    auto rewriteAndCollectChildren = [](Expression* expr, ExpressionContext* expCtx) {
        std::vector<std::unique_ptr<MatchExpression>> childExprVec;
        for (auto& child : expr->getChildren()) {
            auto rewritten = rewriteAggAndOrAsMatch(child.get(), expCtx);
            if (rewritten) {
                childExprVec.emplace_back(std::move(rewritten));
            } else {
                childExprVec.emplace_back(std::make_unique<ExprMatchExpression>(child, expCtx));
            }
        }
        return childExprVec;
    };
    // We should be able to stop rewriting $and/$or if we don't detect any encrypted text search
    // below their tree, but that requires walking the expression tree at every step of the
    // recursion and may negate any benefit we get from terminating early.
    if (dynamic_cast<ExpressionAnd*>(expr)) {
        /**
         * We have a $expr: {$and : [{}, {}]}, let's convert it into $and: [{expr: {}}, {expr: {}}].
         * Since we don't know how far down the FLE rewrite happens, we need to replace all the
         * nested $and/$or into match equivalents as well.
         */
        return std::make_unique<AndMatchExpression>(rewriteAndCollectChildren(expr, expCtx));
    } else if (dynamic_cast<ExpressionOr*>(expr)) {
        return std::make_unique<OrMatchExpression>(rewriteAndCollectChildren(expr, expCtx));
    }
    return nullptr;
}

bool isAndOrAggExpression(const Expression* expr) {
    return dynamic_cast<const ExpressionAnd*>(expr) || dynamic_cast<const ExpressionOr*>(expr);
}

bool hasEncryptedTextSearchExpr(QueryRewriter& queryRewriter, Expression* expression) {
    tassert(10817903, "Expected an expression to rewrite but found none", expression);

    EncryptedTextSearchExpressionDetector detector{queryRewriter};
    auto res = expression_walker::walk<Expression>(expression, &detector);
    return detector.wasDetected();
}
}  // namespace

std::unique_ptr<MatchExpression> QueryRewriter::_rewriteExprMatchExpression(
    ExprMatchExpression& exprMatchExpression) {
    auto* aggExpr = exprMatchExpression.getExpression().get();
    /**
     * If we have a $expr with an encrypted text search expression in its subtree, we
     * require special handling to make sure we can generate a rewrite with tag disjunctions
     * that result in an index scan.
     */
    if (hasEncryptedTextSearchExpr(*this, aggExpr)) {
        // 1) Base case: we have a $expr with a single encrypted text search expression.
        if (const auto* exprTextSearch = dynamic_cast<const ExpressionEncTextSearch*>(aggExpr);
            exprTextSearch) {
            /**
             * At this point, we know that have a $expr within a match expression, which means we
             * are free to try and replace that $expr with a match equivalent if possible.
             *
             * Typically, we would rely on the rewrite logic to invoke the EncryptedPredicate
             * rewrite code on this expression, which would provide us with an aggregate expression
             * representing the tag disjunction of the encrypted text predicate:
             * i.e {$expr: {$or:[{$in:[...]}, {$in:[...]},...]}}
             *
             * However, this disjunction expression relies on the query optimizer rewrites to
             * rewrite the disjunctionin a way that will allow the index scan to be used. When the
             * number of tags is sufficiently high, the residual filter from the QO rewrite does not
             * perform well.
             *
             * To overcome this, we try to rewrite the tag disjunction for the encrypted text
             * predicate as a match expression. We could try to do the same for encrypted range and
             * equality predicates, but it would be more complex and intrusive, so we limit the
             * change to text predicates for now.
             *
             * Note, that rewriteToTagDisjunctionAsMatch will return a nullptr if we failed to
             * generate the disjunction, which may happen if we exceed the tag limit during tag
             * generation.
             */
            auto rewrittenMatchExpression =
                _getEncryptedTextSearchPredicate().rewriteToTagDisjunctionAsMatch(*exprTextSearch);
            /**
             * Ensure we detect the rewrite for this aggregate expression. This is required in case
             * our $expr was within a user provided query with the match expression:
             * {$match: {$and/$or: [{$expr:{$encStrContains:{...}}}]}
             */
            if (rewrittenMatchExpression) {
                _rewroteLastExpression = true;
            }
            return rewrittenMatchExpression;
        }

        // 2: We have an encrypted text search expression within an aggregate $and/$or expression
        // tree.
        if (isAndOrAggExpression(aggExpr)) {
            /**
             * In order to generate an index scan for encrypted text search expressions within an
             * aggregate $and/$or, we first need to convert the $and/$or logical operators to a
             * match $and/$or with the $expr pushed to the sub-expressions.
             */
            auto andOrAsMatchExpression = rewriteAggAndOrAsMatch(aggExpr, getExpressionContext());
            // We should always be able to convert the aggregate $and/$or to a match equivalent, but
            // if we fail to do so, we don't want to error out. Instead, we will default to the
            // aggregate rewrite behaviour.
            if (andOrAsMatchExpression) {
                // Save the current value of _rewroteLastExpression, since we are resetting the
                // value to false here in order to check if any nested expressions are rewritten in
                // our recursive call to _rewrite().
                auto didRewriteBeforeLogicalAggAsMatch = _rewroteLastExpression;
                _rewroteLastExpression = false;
                auto rewrittenAndOrAsMatch = _rewrite(andOrAsMatchExpression.get());
                const auto andOrMatchHadNestedRewrite = _rewroteLastExpression;
                _rewroteLastExpression |= didRewriteBeforeLogicalAggAsMatch;

                // We should never receive a new pointer from _rewrite, because $and/$or match
                // expressions can only rewrite their children, they don't generate a new top level
                // expression.
                tassert(10817904,
                        "Detected unexpected top level $and/$or rewrite",
                        (rewrittenAndOrAsMatch.get() == nullptr));

                // Our match $and/$or generated nested rewrites, let's return it here.
                if (andOrMatchHadNestedRewrite) {
                    return andOrAsMatchExpression;
                }
                // If we got here, it means none of the sub-expressions in the $and/$or generated
                // FLE rewrites into tag disjunctions. In this case, we should return a nullptr, so
                // we use the original $and/$or.
                return nullptr;
            }
        }
    }
    /**
     * Rewrite the $expr sub-expression using the default rewrite logic.
     * If we got here, it means we have one of the following scenarios:
     * 1) We had a $expr that did not have any encrypted text search expressions in the subtree.
     * 2) We had a $expr that has an encrypted text search below a node that can't be converted
     *    i.e $not: [{ $encStrStartsWith: {...}}]
     * 3) We had a $expr with an $and/$or that contains an encrypted text search expression in the
     *    subtree, but the $and/$or failed to convert to a match equivalent with $expr pushed down.
     *    that failed to convert to a match equivalent via rewriteAggAndOrAsMatch().
     *    This is very unlikely, but we fall through to this code so that we still generate the FLE
     *    rewrites in the aggregation language (i.e default behaviour).
     */
    // Save the current value of _rewroteLastExpression, since rewriteExpression() may
    // reset it to false and we may have already done a match expression rewrite.
    auto didRewrite = _rewroteLastExpression;
    auto rewritten = rewriteExpression(aggExpr);
    _rewroteLastExpression |= didRewrite;
    if (rewritten) {
        return std::make_unique<ExprMatchExpression>(rewritten.release(), getExpressionContext());
    }
    return nullptr;
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
            return _rewriteExprMatchExpression(*static_cast<ExprMatchExpression*>(expr));
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

const TextSearchPredicate& QueryRewriter::_getEncryptedTextSearchPredicate() const {
    tassert(10817902,
            "Invalid call to _getEncryptedTextSearchPredicate()",
            _hasValidTextSearchPredicate());
    return *_textSearchPredicate;
}

bool QueryRewriter::_initializeTextSearchPredicate() {
    // If we already had a valid text search predicate, don't re-initialize it.
    if (_hasValidTextSearchPredicate()) {
        return true;
    }
    _textSearchPredicate = _initializeTextSearchPredicateInternal();
    return _hasValidTextSearchPredicate();
}

std::unique_ptr<TextSearchPredicate> QueryRewriter::_initializeTextSearchPredicateInternal() const {
    // We must use the feature flag as part of our criteria here to prevent processing encrypted
    // text search expressions when the feature flag is disabled. The feature flag is used to
    // guard in this way in the aggRewriteMap registration.
    if (gFeatureFlagQETextSearchPreview.canBeEnabled()) {
        return std::make_unique<TextSearchPredicate>(this);
    }
    return nullptr;
}

bool QueryRewriter::_hasValidTextSearchPredicate() const {
    return _textSearchPredicate.get() != nullptr;
}

}  // namespace mongo::fle
