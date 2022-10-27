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

#include "encrypted_predicate.h"

#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/query/fle/query_rewriter_interface.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace fle {

ExpressionToRewriteMap aggPredicateRewriteMap{};
MatchTypeToRewriteMap matchPredicateRewriteMap{};

void logTagsExceeded(const ExceptionFor<ErrorCodes::FLEMaxTagLimitExceeded>& ex) {
    LOGV2_DEBUG(
        6672410, 2, "FLE Max tag limit hit during query rewrite", "__error__"_attr = ex.what());
}

std::unique_ptr<MatchExpression> makeTagDisjunction(BSONArray&& tagArray) {
    auto tagElems = std::vector<BSONElement>();
    tagArray.elems(tagElems);

    auto newExpr = std::make_unique<InMatchExpression>(StringData(kSafeContent));
    newExpr->setBackingBSON(std::move(tagArray));
    uassertStatusOK(newExpr->setEqualities(std::move(tagElems)));

    return newExpr;
}

BSONArray toBSONArray(std::vector<PrfBlock>&& vec) {
    auto bab = BSONArrayBuilder();
    for (auto& elt : vec) {
        bab.appendBinData(elt.size(), BinDataType::BinDataGeneral, elt.data());
    }
    return bab.arr();
}

std::vector<Value> toValues(std::vector<PrfBlock>&& vec) {
    std::vector<Value> output;
    for (auto& elt : vec) {
        output.push_back(Value(BSONBinData(elt.data(), elt.size(), BinDataType::BinDataGeneral)));
    }
    return output;
}

std::unique_ptr<Expression> makeTagDisjunction(ExpressionContext* expCtx,
                                               std::vector<Value>&& tags) {
    std::vector<boost::intrusive_ptr<Expression>> orListElems;
    for (auto&& tagElt : tags) {
        // ... and for each tag, construct expression {$in: [tag,
        // "$__safeContent__"]}.
        std::vector<boost::intrusive_ptr<Expression>> inVec{
            ExpressionConstant::create(expCtx, tagElt),
            ExpressionFieldPath::createPathFromString(
                expCtx, kSafeContent, expCtx->variablesParseState)};
        orListElems.push_back(make_intrusive<ExpressionIn>(expCtx, std::move(inVec)));
    }
    return std::make_unique<ExpressionOr>(expCtx, std::move(orListElems));
}
}  // namespace fle
}  // namespace mongo
