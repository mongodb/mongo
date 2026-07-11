// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/fle/range_predicate.h"

#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_tags.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/fle/encrypted_predicate.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <string_view>
#include <utility>

#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::fle {

REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE(GT, RangePredicate);
REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE(GTE, RangePredicate);
REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE(LT, RangePredicate);
REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE(LTE, RangePredicate);

REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE(ExpressionCompare, RangePredicate);

namespace {
// Validate the range operator passed in and return the fieldpath, its path string, and the payload
// for the rewrite. If the passed-in expression is a comparison with $eq, $ne, or $cmp, none of
// which represent a range predicate, then return a null fieldpath so the rewrite can return null.
struct RangeOp {
    boost::intrusive_ptr<Expression> fieldpath;
    std::string path;
    Value payload;
};
RangeOp validateRangeOp(const Expression* expr) {
    auto children = [&]() {
        auto cmpExpr = dynamic_cast<const ExpressionCompare*>(expr);
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
        return {nullptr, "", Value()};
    }
    // ExpressionCompare has a fixed arity of 2.
    auto fieldpath = dynamic_cast<ExpressionFieldPath*>(children[0].get());
    uassert(6720903, "first argument should be a fieldpath", fieldpath);
    auto secondArg = dynamic_cast<ExpressionConstant*>(children[1].get());
    uassert(6720904, "second argument should be a constant", secondArg);
    return {children[0],
            fieldpath->getFieldPathWithoutCurrentPrefix().fullPath(),
            secondArg->getValue()};
}
}  // namespace

std::unique_ptr<ExpressionInternalFLEBetween> RangePredicate::fleBetweenFromPayload(
    std::string_view path, ParsedFindRangePayload payload) const {
    auto* expCtx = _rewriter->getExpressionContext();
    return fleBetweenFromPayload(ExpressionFieldPath::createPathFromString(
                                     expCtx, std::string{path}, expCtx->variablesParseState),
                                 payload);
}

std::unique_ptr<ExpressionInternalFLEBetween> RangePredicate::fleBetweenFromPayload(
    boost::intrusive_ptr<Expression> fieldpath, ParsedFindRangePayload payload) const {
    tassert(7030501,
            "$internalFleBetween can only be generated from a non-stub payload.",
            !payload.isStub());

    std::vector<ServerZerosEncryptionToken> serverZerosTokens;
    serverZerosTokens.reserve(payload.edges.value().size());

    std::transform(std::make_move_iterator(payload.edges.value().begin()),
                   std::make_move_iterator(payload.edges.value().end()),
                   std::back_inserter(serverZerosTokens),
                   [](FLEFindEdgeTokenSet&& edge) {
                       return ServerZerosEncryptionToken::deriveFrom(edge.server);
                   });

    auto* expCtx = _rewriter->getExpressionContext();
    return std::make_unique<ExpressionInternalFLEBetween>(
        expCtx, fieldpath, std::move(serverZerosTokens));
}

std::vector<PrfBlock> RangePredicate::generateTags(BSONValue payload, std::string_view path) const {
    auto parsedPayload = parseFindPayload<ParsedFindRangePayload>(
        payload, path, _rewriter->getEncryptedFieldConfigForValidation());
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
        return makeTagDisjunction(toBSONArray(generateTags(payload, compExpr->path())));
    }
    MONGO_UNREACHABLE_TASSERT(6720900);
}

std::unique_ptr<Expression> RangePredicate::rewriteToTagDisjunction(Expression* expr) const {
    auto [fieldpath, path, payload] = validateRangeOp(expr);
    if (!fieldpath) {
        return nullptr;
    }
    if (!isPayload(payload)) {
        return nullptr;
    }
    if (isStub(std::ref(payload))) {
        return std::make_unique<ExpressionConstant>(_rewriter->getExpressionContext(), Value(true));
    }

    auto tags = toValues(generateTags(std::ref(payload), path));

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
    auto payload = parseFindPayload<ParsedFindRangePayload>(
        ffp, expr->path(), _rewriter->getEncryptedFieldConfigForValidation());
    auto internalFleBetween = fleBetweenFromPayload(expr->path(), payload);

    return std::make_unique<ExprMatchExpression>(
        boost::intrusive_ptr<ExpressionInternalFLEBetween>(internalFleBetween.release()),
        _rewriter->getExpressionContext());
}

std::unique_ptr<Expression> RangePredicate::rewriteToRuntimeComparison(Expression* expr) const {
    auto [fieldpath, path, ffp] = validateRangeOp(expr);
    if (!fieldpath) {
        return nullptr;
    }
    if (!isPayload(ffp)) {
        return nullptr;
    }
    auto payload = parseFindPayload<ParsedFindRangePayload>(
        ffp, path, _rewriter->getEncryptedFieldConfigForValidation());
    if (payload.isStub()) {
        return std::make_unique<ExpressionConstant>(_rewriter->getExpressionContext(), Value(true));
    }
    return fleBetweenFromPayload(fieldpath, payload);
}
}  // namespace mongo::fle
