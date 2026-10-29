// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_crypto_types.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/fle/encrypted_predicate.h"
#include "mongo/db/query/fle/query_rewriter_interface.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::fle {
/**
 * Server-side rewrite for the encrypted equality index. This rewrite expects either a $eq or $in
 * expression.
 */
class EqualityPredicate : public EncryptedPredicate {
public:
    EqualityPredicate(const QueryRewriterInterface* rewriter) : EncryptedPredicate(rewriter) {}

protected:
    std::vector<PrfBlock> generateTags(BSONValue payload, std::string_view path) const override;

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

    boost::optional<std::pair<const ExpressionFieldPath*, const ExpressionConstant*>>
    extractDetailsFromComparison(const ExpressionCompare* eqExpr) const;

    boost::optional<const ExpressionFieldPath*> validateIn(const ExpressionIn* inExpr,
                                                           const ExpressionArray* inList) const;
};
}  // namespace mongo::fle
