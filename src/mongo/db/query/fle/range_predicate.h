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

#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/query/fle/encrypted_predicate.h"

namespace mongo::fle {
/**
 * Rewrite for the encrypted range index, which expects a comparison operator expression.
 */
class RangePredicate : public EncryptedPredicate {
public:
    RangePredicate(const QueryRewriterInterface* rewriter) : EncryptedPredicate(rewriter) {}

protected:
    std::vector<PrfBlock> generateTags(BSONValue payload) const override;

    std::unique_ptr<MatchExpression> rewriteToTagDisjunction(MatchExpression* expr) const override;
    std::unique_ptr<Expression> rewriteToTagDisjunction(Expression* expr) const override;

    std::unique_ptr<MatchExpression> rewriteToRuntimeComparison(
        MatchExpression* expr) const override;
    std::unique_ptr<Expression> rewriteToRuntimeComparison(Expression* expr) const override;

    virtual bool isStub(BSONElement elt) const {
        auto parsedPayload = parseFindPayload<ParsedFindRangePayload>(elt);
        return parsedPayload.isStub();
    }

    virtual bool isStub(Value elt) const {
        auto parsedPayload = parseFindPayload<ParsedFindRangePayload>(elt);
        return parsedPayload.isStub();
    }

    bool isDeprecatedPayloadType(EncryptedBinDataType type) const override {
        return type == EncryptedBinDataType::kFLE2FindRangePayload ||
            type == EncryptedBinDataType::kFLE2UnindexedEncryptedValue;
    }

private:
    EncryptedBinDataType encryptedBinDataType() const override {
        return EncryptedBinDataType::kFLE2FindRangePayloadV2;
    }
    /**
     * Generate an expression for encrypted collscan for a range index.
     */
    std::unique_ptr<ExpressionInternalFLEBetween> fleBetweenFromPayload(
        StringData path, ParsedFindRangePayload payload) const;

    std::unique_ptr<ExpressionInternalFLEBetween> fleBetweenFromPayload(
        boost::intrusive_ptr<Expression> fieldpath, ParsedFindRangePayload payload) const;
};
}  // namespace mongo::fle
