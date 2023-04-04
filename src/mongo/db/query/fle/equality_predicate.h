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

#pragma once

#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/query/fle/encrypted_predicate.h"

namespace mongo::fle {
/**
 * Server-side rewrite for the encrypted equality index. This rewrite expects either a $eq or $in
 * expression.
 */
class EqualityPredicate : public EncryptedPredicate {
public:
    EqualityPredicate(const QueryRewriterInterface* rewriter) : EncryptedPredicate(rewriter) {}

protected:
    std::vector<PrfBlock> generateTags(BSONValue payload) const override;

    std::unique_ptr<MatchExpression> rewriteToTagDisjunction(MatchExpression* expr) const override;
    std::unique_ptr<Expression> rewriteToTagDisjunction(Expression* expr) const override;

    std::unique_ptr<MatchExpression> rewriteToRuntimeComparison(
        MatchExpression* expr) const override;
    std::unique_ptr<Expression> rewriteToRuntimeComparison(Expression* expr) const override;

    bool isDeprecatedPayloadType(EncryptedBinDataType type) const override {
        return type == EncryptedBinDataType::kFLE2FindEqualityPayload ||
            type == EncryptedBinDataType::kFLE2UnindexedEncryptedValue;
    }

private:
    EncryptedBinDataType encryptedBinDataType() const override {

        return EncryptedBinDataType::kFLE2FindEqualityPayloadV2;
    }

    boost::optional<std::pair<ExpressionFieldPath*, ExpressionConstant*>>
    extractDetailsFromComparison(ExpressionCompare* eqExpr) const;

    boost::optional<const ExpressionFieldPath*> validateIn(ExpressionIn* inExpr,
                                                           ExpressionArray* inList) const;
};
}  // namespace mongo::fle
