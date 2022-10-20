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

#include "range_predicate.h"

#include <iterator>

#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_tags.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/fle/encrypted_predicate.h"

namespace mongo::fle {

REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_WITH_FLAG(BETWEEN,
                                                     RangePredicate,
                                                     gFeatureFlagFLE2Range);

REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_WITH_FLAG(GT, RangePredicate, gFeatureFlagFLE2Range);
REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_WITH_FLAG(GTE, RangePredicate, gFeatureFlagFLE2Range);
REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_WITH_FLAG(LT, RangePredicate, gFeatureFlagFLE2Range);
REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_WITH_FLAG(LTE, RangePredicate, gFeatureFlagFLE2Range);

REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE_WITH_FLAG(ExpressionBetween,
                                                   RangePredicate,
                                                   gFeatureFlagFLE2Range);

std::vector<PrfBlock> RangePredicate::generateTags(BSONValue payload) const {
    auto parsedPayload = parseFindPayload<ParsedFindRangePayload>(payload);
    std::vector<PrfBlock> tags;
    tassert(7030500, "Must generate tags from a non-stub payload.", !parsedPayload.isStub());
    for (auto& edge : parsedPayload.edges.value()) {
        auto tagsForEdge = readTags(*_rewriter->getEscReader(),
                                    *_rewriter->getEccReader(),
                                    edge.esc,
                                    edge.ecc,
                                    edge.edc,
                                    parsedPayload.maxCounter);
        tags.insert(tags.end(),
                    std::make_move_iterator(tagsForEdge.begin()),
                    std::make_move_iterator(tagsForEdge.end()));
    }
    return tags;
}

std::unique_ptr<MatchExpression> RangePredicate::rewriteToTagDisjunction(
    MatchExpression* expr) const {
    if (auto compExpr = dynamic_cast<ComparisonMatchExpression*>(expr)) {
        auto payload = compExpr->getData();
        if (!isPayload(payload)) {
            return nullptr;
        }
        // If this is a stub expression, replace expression with $alwaysTrue.
        if (isStub(payload)) {
            return std::make_unique<AlwaysTrueMatchExpression>();
        }
        return makeTagDisjunction(toBSONArray(generateTags(payload)));
    }

    tassert(6720900,
            "Range rewrite should only be called with $between operator.",
            expr->matchType() == MatchExpression::BETWEEN);
    auto betExpr = static_cast<BetweenMatchExpression*>(expr);
    auto payload = betExpr->rhs();

    if (!isPayload(payload)) {
        return nullptr;
    }
    return makeTagDisjunction(toBSONArray(generateTags(payload)));
}

std::pair<boost::intrusive_ptr<Expression>, Value> validateBetween(Expression* expr) {
    auto betweenExpr = dynamic_cast<ExpressionBetween*>(expr);
    tassert(6720901, "Range rewrite should only be called with $between operator.", betweenExpr);
    auto children = betweenExpr->getChildren();
    uassert(6720902, "$between should have two children.", children.size() == 2);

    auto fieldpath = dynamic_cast<ExpressionFieldPath*>(children[0].get());
    uassert(6720903, "first argument should be a fieldpath", fieldpath);
    auto secondArg = dynamic_cast<ExpressionConstant*>(children[1].get());
    uassert(6720904, "second argument should be a constant", secondArg);
    auto payload = secondArg->getValue();
    return {children[0], payload};
}

std::unique_ptr<Expression> RangePredicate::rewriteToTagDisjunction(Expression* expr) const {
    auto [_, payload] = validateBetween(expr);
    if (!isPayload(payload)) {
        return nullptr;
    }
    auto tags = toValues(generateTags(std::ref(payload)));

    return makeTagDisjunction(_rewriter->getExpressionContext(), std::move(tags));
}

std::unique_ptr<ExpressionInternalFLEBetween> RangePredicate::fleBetweenFromPayload(
    StringData path, ParsedFindRangePayload payload) const {
    auto* expCtx = _rewriter->getExpressionContext();
    return fleBetweenFromPayload(ExpressionFieldPath::createPathFromString(
                                     expCtx, path.toString(), expCtx->variablesParseState),
                                 payload);
}

std::unique_ptr<ExpressionInternalFLEBetween> RangePredicate::fleBetweenFromPayload(
    boost::intrusive_ptr<Expression> fieldpath, ParsedFindRangePayload payload) const {
    tassert(7030501,
            "$internalFleBetween can only be generated from a non-stub payload.",
            !payload.isStub());
    auto cm = payload.maxCounter;
    ServerDataEncryptionLevel1Token serverToken = std::move(payload.serverToken);
    std::vector<ConstDataRange> edcTokens;
    std::transform(std::make_move_iterator(payload.edges.value().begin()),
                   std::make_move_iterator(payload.edges.value().end()),
                   std::back_inserter(edcTokens),
                   [](FLEFindEdgeTokenSet&& edge) { return edge.edc.toCDR(); });

    auto* expCtx = _rewriter->getExpressionContext();
    return std::make_unique<ExpressionInternalFLEBetween>(
        expCtx, fieldpath, serverToken.toCDR(), cm, std::move(edcTokens));
}

std::unique_ptr<MatchExpression> RangePredicate::rewriteToRuntimeComparison(
    MatchExpression* expr) const {
    BSONElement ffp;
    if (auto compExpr = dynamic_cast<ComparisonMatchExpression*>(expr)) {
        auto payload = compExpr->getData();
        if (!isPayload(payload)) {
            return nullptr;
        }
        // If this is a stub expression, replace expression with $alwaysTrue.
        if (isStub(payload)) {
            return std::make_unique<AlwaysTrueMatchExpression>();
        }
        ffp = payload;
    } else {
        auto between = static_cast<BetweenMatchExpression*>(expr);
        ffp = between->rhs();
    }

    if (!isPayload(ffp)) {
        return nullptr;
    }
    auto payload = parseFindPayload<ParsedFindRangePayload>(ffp);
    auto internalFleBetween = fleBetweenFromPayload(expr->path(), payload);

    return std::make_unique<ExprMatchExpression>(
        boost::intrusive_ptr<ExpressionInternalFLEBetween>(internalFleBetween.release()),
        _rewriter->getExpressionContext());
}

std::unique_ptr<Expression> RangePredicate::rewriteToRuntimeComparison(Expression* expr) const {
    auto [fieldpath, ffp] = validateBetween(expr);
    if (!isPayload(ffp)) {
        return nullptr;
    }
    auto payload = parseFindPayload<ParsedFindRangePayload>(ffp);
    return fleBetweenFromPayload(fieldpath, payload);
}
}  // namespace mongo::fle
