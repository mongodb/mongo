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

#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/query/fle/encrypted_predicate.h"

namespace mongo::fle {

REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_WITH_FLAG(BETWEEN,
                                                     RangePredicate,
                                                     gFeatureFlagFLE2Range);

// TODO: SERVER-67206 Generate tags for range payload.
std::vector<PrfBlock> RangePredicate::generateTags(BSONValue payload) const {
    return {};
}

std::unique_ptr<MatchExpression> RangePredicate::rewriteToTagDisjunction(
    MatchExpression* expr) const {
    invariant(expr->matchType() == MatchExpression::BETWEEN);
    auto betExpr = static_cast<BetweenMatchExpression*>(expr);
    auto ffp = betExpr->rhs();

    if (!isPayload(ffp)) {
        return nullptr;
    }

    auto obj = toBSONArray(generateTags(ffp));
    auto tags = std::vector<BSONElement>();
    obj.elems(tags);
    auto inExpr = std::make_unique<InMatchExpression>(kSafeContent);
    inExpr->setBackingBSON(std::move(obj));
    auto status = inExpr->setEqualities(std::move(tags));
    uassertStatusOK(status);
    return inExpr;
}

// TODO: SERVER-67209 Server-side rewrite for agg expressions with $between.
std::unique_ptr<Expression> RangePredicate::rewriteToTagDisjunction(Expression* expr) const {
    return nullptr;
}

// TODO: SERVER-67267 Rewrite $between to $_internalFleBetween when number of tags exceeds
// limit.
std::unique_ptr<MatchExpression> RangePredicate::rewriteToRuntimeComparison(
    MatchExpression* expr) const {
    return nullptr;
}

// TODO: SERVER-67267 Rewrite $between to $_internalFleBetween when number of tags exceeds
// limit.
std::unique_ptr<Expression> RangePredicate::rewriteToRuntimeComparison(Expression* expr) const {
    return nullptr;
}
}  // namespace mongo::fle
