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
 * Server-side rewrite used for classes that are derived from ExpressionEncTextSearch.
 */
class TextSearchPredicate : public EncryptedPredicate {
public:
    TextSearchPredicate(const QueryRewriterInterface* rewriter) : EncryptedPredicate(rewriter) {}

    /**
     * Encrypted text search predicates may exhibit poor performance when they generate an
     * aggregation expression tag disjunction, because the query optimization rewrite relies on a
     * residual filter which has an added cost that does not perform well at scale. For this reason,
     * we provide this method to allow for ExpressionEncTextSearch to generate a match style tag
     * disjunction instead.
     */
    std::unique_ptr<MatchExpression> rewriteToTagDisjunctionAsMatch(
        const ExpressionEncTextSearch& expr) const;

    bool hasValidPayload(const ExpressionEncTextSearch& expr) const;

protected:
    std::vector<PrfBlock> generateTags(BSONValue payload, std::string_view path) const override;

    std::unique_ptr<Expression> rewriteToTagDisjunction(Expression* expr) const override;
    std::unique_ptr<MatchExpression> rewriteToTagDisjunction(MatchExpression* expr) const override;

    std::unique_ptr<Expression> rewriteToRuntimeComparison(Expression* expr) const override;
    std::unique_ptr<MatchExpression> rewriteToRuntimeComparison(
        MatchExpression* expr) const override;

private:
    EncryptedBinDataType encryptedBinDataType() const override {
        return EncryptedBinDataType::kFLE2FindTextPayload;
    }

    std::unique_ptr<MatchExpression> _rewriteToTagDisjunctionAsMatch(
        const ExpressionEncTextSearch& expr) const;
};

}  // namespace mongo::fle
