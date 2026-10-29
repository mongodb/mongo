// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_crypto_types.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/fle/encrypted_predicate.h"
#include "mongo/db/query/fle/query_rewriter_interface.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::fle {
using namespace std::literals::string_view_literals;
/**
 * Rewrite for the encrypted range index, which expects a comparison operator expression.
 */
class RangePredicate : public EncryptedPredicate {
public:
    RangePredicate(const QueryRewriterInterface* rewriter) : EncryptedPredicate(rewriter) {}

protected:
    std::vector<PrfBlock> generateTags(BSONValue payload, std::string_view path) const override;

    std::unique_ptr<MatchExpression> rewriteToTagDisjunction(MatchExpression* expr) const override;
    std::unique_ptr<Expression> rewriteToTagDisjunction(Expression* expr) const override;

    std::unique_ptr<MatchExpression> rewriteToRuntimeComparison(
        MatchExpression* expr) const override;
    std::unique_ptr<Expression> rewriteToRuntimeComparison(Expression* expr) const override;

    // Pass empty string and boost::none to skip validation; this is just a structural check and the
    // ParsedFindRangePayload is scoped within this function so it doesn't need strict validation.
    virtual bool isStub(BSONElement elt) const {
        auto parsedPayload = parseFindPayload<ParsedFindRangePayload>(elt, ""sv, boost::none);
        return parsedPayload.isStub();
    }

    virtual bool isStub(Value elt) const {
        auto parsedPayload = parseFindPayload<ParsedFindRangePayload>(elt, ""sv, boost::none);
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
        std::string_view path, ParsedFindRangePayload payload) const;

    std::unique_ptr<ExpressionInternalFLEBetween> fleBetweenFromPayload(
        boost::intrusive_ptr<Expression> fieldpath, ParsedFindRangePayload payload) const;
};
}  // namespace mongo::fle
