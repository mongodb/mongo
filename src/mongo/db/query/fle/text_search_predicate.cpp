// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/fle/text_search_predicate.h"

#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/crypto/fle_tags.h"
#include "mongo/db/server_feature_flags_gen.h"

namespace mongo::fle {

REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE_WITH_FLAG(ExpressionEncStrStartsWith,
                                                   TextSearchPredicate,
                                                   gFeatureFlagQETextSearchPreview);
REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE_WITH_FLAG(ExpressionEncStrEndsWith,
                                                   TextSearchPredicate,
                                                   gFeatureFlagQETextSearchPreview);
REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE_WITH_FLAG(ExpressionEncStrContains,
                                                   TextSearchPredicate,
                                                   gFeatureFlagQETextSearchPreview);
REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE_WITH_FLAG(ExpressionEncStrNormalizedEq,
                                                   TextSearchPredicate,
                                                   gFeatureFlagQETextSearchPreview);

std::vector<PrfBlock> TextSearchPredicate::generateTags(BSONValue payload,
                                                        std::string_view path) const {
    ParsedFindTextSearchPayload tokens = parseFindPayload<ParsedFindTextSearchPayload>(
        payload, path, _rewriter->getEncryptedFieldConfigForValidation());
    return readTags(_rewriter->getTagQueryInterface(),
                    _rewriter->getESCNss(),
                    tokens.esc,
                    tokens.edc,
                    tokens.maxCounter);
}

std::unique_ptr<Expression> TextSearchPredicate::rewriteToTagDisjunction(Expression* expr) const {
    if (auto textSearchExpr = dynamic_cast<ExpressionEncTextSearch*>(expr); textSearchExpr) {
        const auto& textConstant = textSearchExpr->getText();
        auto payload = textConstant.getValue();
        if (!isPayload(payload)) {
            return nullptr;
        }
        auto path = textSearchExpr->getInput().getFieldPathWithoutCurrentPrefix().fullPath();
        return makeTagDisjunction(_rewriter->getExpressionContext(),
                                  toValues(generateTags(std::ref(payload), path)));
    }
    MONGO_UNREACHABLE_TASSERT(10112602);
}

std::unique_ptr<MatchExpression> TextSearchPredicate::rewriteToTagDisjunctionAsMatch(
    const ExpressionEncTextSearch& expr) const {
    return _tryRewriteToTagDisjunction<MatchExpression>(
        [&]() { return this->_rewriteToTagDisjunctionAsMatch(expr); });
}

std::unique_ptr<MatchExpression> TextSearchPredicate::_rewriteToTagDisjunctionAsMatch(
    const ExpressionEncTextSearch& expr) const {
    const auto& textConstant = expr.getText();
    auto payload = textConstant.getValue();
    if (!isPayload(payload)) {
        return nullptr;
    }
    auto path = expr.getInput().getFieldPathWithoutCurrentPrefix().fullPath();
    // Here we create a match style in list. It is only allowed when replacing a single
    // root level $expr predicate.
    return makeTagDisjunction(toBSONArray(generateTags(payload, path)));
}

std::unique_ptr<Expression> TextSearchPredicate::rewriteToRuntimeComparison(
    Expression* expr) const {
    if (auto textSearchExpr = dynamic_cast<ExpressionEncTextSearch*>(expr); textSearchExpr) {
        // Since we don't support non-encrypted ExpressionEncTextSearch expressions such as
        // $encStrStartsWith $encStrEndsWith, the expression is already considered the
        // runtime comparison, therefore we don't need to do rewriting here.
        return nullptr;
    }
    MONGO_UNREACHABLE_TASSERT(10112603);
}

std::unique_ptr<MatchExpression> TextSearchPredicate::rewriteToTagDisjunction(
    MatchExpression* expr) const {
    tasserted(10112600,
              "Encrypted text search predicates are only supported as aggregation expressions.");
}

std::unique_ptr<MatchExpression> TextSearchPredicate::rewriteToRuntimeComparison(
    MatchExpression* expr) const {
    tasserted(10112601,
              "Encrypted text search predicates are only supported as aggregation expressions.");
}

bool TextSearchPredicate::hasValidPayload(const ExpressionEncTextSearch& expr) const {
    const auto& textConstant = expr.getText();
    auto payload = textConstant.getValue();
    return isPayload(payload);
}

}  // namespace mongo::fle
