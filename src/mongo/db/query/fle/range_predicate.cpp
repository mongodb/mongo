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

REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_WITH_FLAG(GT, RangePredicate, gFeatureFlagFLE2Range);
REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_WITH_FLAG(GTE, RangePredicate, gFeatureFlagFLE2Range);
REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_WITH_FLAG(LT, RangePredicate, gFeatureFlagFLE2Range);
REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_WITH_FLAG(LTE, RangePredicate, gFeatureFlagFLE2Range);

REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE_WITH_FLAG(ExpressionCompare,
                                                   RangePredicate,
                                                   gFeatureFlagFLE2Range);

namespace {
// Validate the range operator passed in and return the fieldpath and payload for the rewrite. If
// the passed-in expression is a comparison with $eq, $ne, or $cmp, none of which represent a range
// predicate, then return null to the caller so that the rewrite can return null.
std::pair<boost::intrusive_ptr<Expression>, Value> validateRangeOp(Expression* expr) {
    auto children = [&]() {
        auto cmpExpr = dynamic_cast<ExpressionCompare*>(expr);
        tassert(
            6720901, "Range rewrite should only be called with a comparison operator.", cmpExpr);
        switch (cmpExpr->getOp()) {
            case ExpressionCompare::GT:
            case ExpressionCompare::GTE:
            case ExpressionCompare::LT:
            case ExpressionCompare::LTE:
                return cmpExpr->getChildren();

            case ExpressionCompare::EQ:
            case ExpressionCompare::NE:
            case ExpressionCompare::CMP:
                return std::vector<boost::intrusive_ptr<Expression>>();
        }
        return std::vector<boost::intrusive_ptr<Expression>>();
    }();
    if (children.empty()) {
        return {nullptr, Value()};
    }
    // ExpressionCompare has a fixed arity of 2.
    auto fieldpath = dynamic_cast<ExpressionFieldPath*>(children[0].get());
    uassert(6720903, "first argument should be a fieldpath", fieldpath);
    auto secondArg = dynamic_cast<ExpressionConstant*>(children[1].get());
    uassert(6720904, "second argument should be a constant", secondArg);
    auto payload = secondArg->getValue();
    return {children[0], payload};
}
}  // namespace

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

    std::vector<ServerZerosEncryptionToken> serverZerosTokens;
    serverZerosTokens.reserve(payload.edges.value().size());

    std::transform(
        std::make_move_iterator(payload.edges.value().begin()),
        std::make_move_iterator(payload.edges.value().end()),
        std::back_inserter(serverZerosTokens),
        [](FLEFindEdgeTokenSet&& edge) {
            return FLEServerMetadataEncryptionTokenGenerator::generateServerZerosEncryptionToken(
                edge.server);
        });

    auto* expCtx = _rewriter->getExpressionContext();
    return std::make_unique<ExpressionInternalFLEBetween>(
        expCtx, fieldpath, std::move(serverZerosTokens));
}

std::vector<PrfBlock> RangePredicate::generateTags(BSONValue payload) const {
    auto parsedPayload = parseFindPayload<ParsedFindRangePayload>(payload);
    std::vector<PrfBlock> tags;
    tassert(7030500, "Must generate tags from a non-stub payload.", !parsedPayload.isStub());

    // TODO - do batch generation of tags here
    for (auto& edge : parsedPayload.edges.value()) {
        auto tagsForEdge = readTags(_rewriter->getTagQueryInterface(),
                                    _rewriter->getESCNss(),
                                    edge.esc,
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
    MONGO_UNREACHABLE_TASSERT(6720900);
}

std::unique_ptr<Expression> RangePredicate::rewriteToTagDisjunction(Expression* expr) const {
    auto [fieldpath, payload] = validateRangeOp(expr);
    if (!fieldpath) {
        return nullptr;
    }
    if (!isPayload(payload)) {
        return nullptr;
    }
    if (isStub(std::ref(payload))) {
        return std::make_unique<ExpressionConstant>(_rewriter->getExpressionContext(), Value(true));
    }

    auto tags = toValues(generateTags(std::ref(payload)));

    return makeTagDisjunction(_rewriter->getExpressionContext(), std::move(tags));
}

std::unique_ptr<MatchExpression> RangePredicate::rewriteToRuntimeComparison(
    MatchExpression* expr) const {
    auto compExpr = dynamic_cast<ComparisonMatchExpression*>(expr);
    tassert(7121400, "Reange rewrite can only operate on comparison match expression", compExpr);
    switch (compExpr->matchType()) {
        case MatchExpression::GT:
        case MatchExpression::LT:
        case MatchExpression::GTE:
        case MatchExpression::LTE:
            break;
        default:
            return nullptr;
    }
    auto ffp = compExpr->getData();
    if (!isPayload(ffp)) {
        return nullptr;
    }
    // If this is a stub expression, replace expression with $alwaysTrue.
    if (isStub(ffp)) {
        return std::make_unique<AlwaysTrueMatchExpression>();
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
    auto [fieldpath, ffp] = validateRangeOp(expr);
    if (!fieldpath) {
        return nullptr;
    }
    if (!isPayload(ffp)) {
        return nullptr;
    }
    auto payload = parseFindPayload<ParsedFindRangePayload>(ffp);
    if (payload.isStub()) {
        return std::make_unique<ExpressionConstant>(_rewriter->getExpressionContext(), Value(true));
    }
    return fleBetweenFromPayload(fieldpath, payload);
}
}  // namespace mongo::fle
