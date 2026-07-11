// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/fle/encrypted_predicate.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_crypto_types.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/logv2/log.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <array>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

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
    auto inExpr = std::make_unique<InMatchExpression>(kSafeContent);

    uassertStatusOK(inExpr->setEqualitiesArray(std::move(tagArray)));

    // __safeContent__ is always an array, so wrap the $in expr in an $elemMatch.
    // most of the time, wrapping the predicate in $elemMatch doesn't have an effect (see the Single
    // Query Condition in the $elemMatch documentation), but it is necessary for upserts to
    // recognize that __safeContent__ should be an array when there is only one tag in the
    // predicate.
    auto elemMatchExpr = std::make_unique<ElemMatchValueMatchExpression>(kSafeContent);
    elemMatchExpr->add(std::move(inExpr));

    return elemMatchExpr;
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
                expCtx, kSafeContentString, expCtx->variablesParseState)};
        orListElems.push_back(make_intrusive<ExpressionIn>(expCtx, std::move(inVec)));
    }
    return std::make_unique<ExpressionOr>(expCtx, std::move(orListElems));
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
    output.reserve(vec.size());
    for (auto& elt : vec) {
        output.push_back(Value(BSONBinData(elt.data(), elt.size(), BinDataType::BinDataGeneral)));
    }
    return output;
}

bool isPayloadOfType(EncryptedBinDataType ty, const BSONElement& elt) {
    return getEncryptedBinDataType(elt) == ty;
}


bool isPayloadOfType(EncryptedBinDataType ty, const Value& v) {
    return getEncryptedBinDataType(v) == ty;
}

bool EncryptedPredicate::validateType(EncryptedBinDataType type) const {
    uassert(7292602,
            str::stream() << "Encountered a Queryable Encryption find payload type "
                             "that is no longer supported: "
                          << static_cast<uint32_t>(type),
            !this->isDeprecatedPayloadType(type));
    return this->encryptedBinDataType() == type;
}
}  // namespace fle
}  // namespace mongo
