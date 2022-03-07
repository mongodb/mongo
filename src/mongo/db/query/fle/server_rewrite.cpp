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


#include "mongo/db/query/fle/server_rewrite.h"
namespace mongo::fle {

std::unique_ptr<MatchExpression> FLEFindRewriter::rewriteMatchExpression(
    std::unique_ptr<MatchExpression> expr) {
    if (auto result = _rewrite(expr.get())) {
        return result;
    } else {
        return expr;
    }
}

// Rewrite the passed-in match expression in-place.
std::unique_ptr<MatchExpression> FLEFindRewriter::_rewrite(MatchExpression* expr) {
    switch (expr->matchType()) {
        case MatchExpression::EQ:
            return rewriteEq(std::move(static_cast<const EqualityMatchExpression*>(expr)));
        case MatchExpression::MATCH_IN:
            return rewriteIn(std::move(static_cast<const InMatchExpression*>(expr)));
        case MatchExpression::AND:
        case MatchExpression::OR:
        case MatchExpression::NOT:
        case MatchExpression::NOR:
            for (size_t i = 0; i < expr->numChildren(); i++) {
                auto child = expr->getChild(i);
                if (auto newChild = _rewrite(child)) {
                    expr->resetChild(i, newChild.release());
                }
            }
            return nullptr;
        default:
            return nullptr;
    }
}

BSONObj FLEFindRewriter::rewritePayloadAsTags(BSONElement fleFindPayload) {
    return BSONObj();
}

std::unique_ptr<InMatchExpression> FLEFindRewriter::rewriteEq(const EqualityMatchExpression* expr) {
    auto ffp = expr->getData();
    if (!isFleFindPayload(ffp)) {
        return nullptr;
    }

    auto obj = rewritePayloadAsTags(ffp);

    auto tags = std::vector<BSONElement>();
    obj.elems(tags);

    auto inExpr = std::make_unique<InMatchExpression>(kSafeContent);
    inExpr->setBackingBSON(std::move(obj));
    auto status = inExpr->setEqualities(std::move(tags));
    uassertStatusOK(status);
    return inExpr;
}

std::unique_ptr<InMatchExpression> FLEFindRewriter::rewriteIn(const InMatchExpression* expr) {
    auto backingBSONBuilder = BSONArrayBuilder();
    size_t numFFPs = 0;
    for (auto& eq : expr->getEqualities()) {
        if (isFleFindPayload(eq)) {
            auto obj = rewritePayloadAsTags(eq);
            ++numFFPs;
            for (auto&& elt : obj) {
                backingBSONBuilder.append(elt);
            }
        }
    }
    if (numFFPs == 0) {
        return nullptr;
    }
    // All elements in an encrypted $in expression should be FFPs.
    uassert(
        6329400,
        "If any elements in a $in expression are encrypted, then all elements should be encrypted.",
        numFFPs == expr->getEqualities().size());

    auto backingBSON = backingBSONBuilder.arr();
    auto allTags = std::vector<BSONElement>();
    backingBSON.elems(allTags);

    auto inExpr = std::make_unique<InMatchExpression>(kSafeContent);
    inExpr->setBackingBSON(std::move(backingBSON));
    auto status = inExpr->setEqualities(std::move(allTags));
    uassertStatusOK(status);

    return inExpr;
}

}  // namespace mongo::fle
