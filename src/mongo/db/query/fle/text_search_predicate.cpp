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

#include "text_search_predicate.h"

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

std::vector<PrfBlock> TextSearchPredicate::generateTags(BSONValue payload) const {
    ParsedFindTextSearchPayload tokens = parseFindPayload<ParsedFindTextSearchPayload>(payload);
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
        return makeTagDisjunction(_rewriter->getExpressionContext(),
                                  toValues(generateTags(std::ref(payload))));
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
    // Here we create a match style in list. It is only allowed when replacing a single
    // root level $expr predicate.
    return makeTagDisjunction(toBSONArray(generateTags(payload)));
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
