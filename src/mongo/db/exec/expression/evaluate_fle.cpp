// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/exec/expression/evaluate.h"

namespace mongo {

namespace exec::expression {

Value evaluate(const ExpressionInternalFLEEqual& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto fieldValue = expr.getChildren()[0]->evaluate(root, variables, ctx);
    if (fieldValue.nullish()) {
        return Value(BSONNULL);
    }

    // Hang on to the FLE2IndexedEqualityEncryptedValueV2 object, because getRawMetadataBlock
    // returns a view on its member.
    boost::optional<FLE2IndexedEqualityEncryptedValueV2> value;
    return Value(expr.getEncryptedPredicateEvaluator().evaluate(
        fieldValue, EncryptedBinDataType::kFLE2EqualityIndexedValueV2, [&value](auto serverValue) {
            // extractMetadataBlocks should only be run once.
            tassert(9588901, "extractMetadataBlocks should only be run once by evaluate", !value);
            value.emplace(serverValue);
            std::vector<ConstFLE2TagAndEncryptedMetadataBlock> metadataBlocks;
            metadataBlocks.push_back(value->getRawMetadataBlock());
            return metadataBlocks;
        }));
}

Value evaluate(const ExpressionInternalFLEBetween& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto fieldValue = expr.getChildren()[0]->evaluate(root, variables, ctx);
    if (fieldValue.nullish()) {
        return Value(BSONNULL);
    }

    // Hang on to the FLE2IndexedRangeEncryptedValueV2 object, because getMetadataBlocks
    // returns a view on its member and its lifetime must last through the completion of evaluate.
    boost::optional<FLE2IndexedRangeEncryptedValueV2> value;
    return Value(expr.getEncryptedPredicateEvaluator().evaluate(
        fieldValue, EncryptedBinDataType::kFLE2RangeIndexedValueV2, [&value](auto serverValue) {
            // extractMetadataBlocks should only be run once.
            tassert(9588707, "extractMetadataBlocks should only be run once by evaluate", !value);
            value.emplace(serverValue);
            return value->getMetadataBlocks();
        }));
}

Value evaluate(const ExpressionEncStrStartsWith& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto fieldValue = expr.getChildren()[0]->evaluate(root, variables, ctx);
    if (fieldValue.nullish()) {
        return Value(BSONNULL);
    }
    uassert(10111808,
            "ExpressionEncStrStartsWith can't be evaluated without binary payload",
            expr.canBeEvaluated());

    // Hang on to the FLE2IndexedTextEncryptedValue object, because getPrefixMetadataBlocks
    // returns a view on its member and its lifetime must last through the completion of evaluate.
    boost::optional<FLE2IndexedTextEncryptedValue> value;
    return Value(expr.getEncryptedPredicateEvaluator().evaluate(
        fieldValue, EncryptedBinDataType::kFLE2TextIndexedValue, [&](auto serverValue) {
            tassert(10456800, "extractMetadataBlocks should only be run once by evaluate", !value);
            value.emplace(serverValue);
            return value->getPrefixMetadataBlocks();
        }));
}

Value evaluate(const ExpressionEncStrEndsWith& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto fieldValue = expr.getChildren()[0]->evaluate(root, variables, ctx);
    if (fieldValue.nullish()) {
        return Value(BSONNULL);
    }
    uassert(10120901,
            "ExpressionEncStrEndsWith can't be evaluated without binary payload",
            expr.canBeEvaluated());

    // Hang on to the FLE2IndexedTextEncryptedValue object, because getSuffixMetadataBlocks
    // returns a view on its member and its lifetime must last through the completion of evaluate.
    boost::optional<FLE2IndexedTextEncryptedValue> value;
    return Value(expr.getEncryptedPredicateEvaluator().evaluate(
        fieldValue, EncryptedBinDataType::kFLE2TextIndexedValue, [&](auto serverValue) {
            tassert(10456801, "extractMetadataBlocks should only be run once by evaluate", !value);
            value.emplace(serverValue);
            return value->getSuffixMetadataBlocks();
        }));
}

Value evaluate(const ExpressionEncStrContains& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto fieldValue = expr.getChildren()[0]->evaluate(root, variables, ctx);
    if (fieldValue.nullish()) {
        return Value(BSONNULL);
    }
    uassert(10208801,
            "ExpressionEncStrContains can't be evaluated without binary payload",
            expr.canBeEvaluated());

    // Hang on to the FLE2IndexedTextEncryptedValue object, because getSubstringMetadataBlocks
    // returns a view on its member and its lifetime must last through the completion of evaluate.
    boost::optional<FLE2IndexedTextEncryptedValue> value;
    return Value(expr.getEncryptedPredicateEvaluator().evaluate(
        fieldValue, EncryptedBinDataType::kFLE2TextIndexedValue, [&](auto serverValue) {
            tassert(10209100, "extractMetadataBlocks should only be run once by evaluate", !value);
            value.emplace(serverValue);
            return value->getSubstringMetadataBlocks();
        }));
}

Value evaluate(const ExpressionEncStrNormalizedEq& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto fieldValue = expr.getChildren()[0]->evaluate(root, variables, ctx);
    if (fieldValue.nullish()) {
        return Value(BSONNULL);
    }
    uassert(10255700,
            "ExpressionEncStrNormalizedEq can't be evaluated without binary payload",
            expr.canBeEvaluated());

    // Hang on to the FLE2IndexedTextEncryptedValue object, because getSubstringMetadataBlocks
    // returns a view on its member and its lifetime must last through the completion of evaluate.
    boost::optional<FLE2IndexedTextEncryptedValue> value;
    return Value(expr.getEncryptedPredicateEvaluator().evaluate(
        fieldValue, EncryptedBinDataType::kFLE2TextIndexedValue, [&](auto serverValue) {
            tassert(10256000, "extractMetadataBlocks should only be run once by evaluate", !value);
            value.emplace(serverValue);
            std::vector<ConstFLE2TagAndEncryptedMetadataBlock> metadataBlocks;
            metadataBlocks.push_back(value->getExactStringMetadataBlock());
            return metadataBlocks;
        }));
}

}  // namespace exec::expression
}  // namespace mongo
