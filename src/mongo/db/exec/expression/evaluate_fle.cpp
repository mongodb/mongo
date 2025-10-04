/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/exec/expression/evaluate.h"

namespace mongo {

namespace exec::expression {

Value evaluate(const ExpressionInternalFLEEqual& expr, const Document& root, Variables* variables) {
    auto fieldValue = expr.getChildren()[0]->evaluate(root, variables);
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
               Variables* variables) {
    auto fieldValue = expr.getChildren()[0]->evaluate(root, variables);
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

Value evaluate(const ExpressionEncStrStartsWith& expr, const Document& root, Variables* variables) {
    auto fieldValue = expr.getChildren()[0]->evaluate(root, variables);
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

Value evaluate(const ExpressionEncStrEndsWith& expr, const Document& root, Variables* variables) {
    auto fieldValue = expr.getChildren()[0]->evaluate(root, variables);
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

Value evaluate(const ExpressionEncStrContains& expr, const Document& root, Variables* variables) {
    auto fieldValue = expr.getChildren()[0]->evaluate(root, variables);
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
               Variables* variables) {
    auto fieldValue = expr.getChildren()[0]->evaluate(root, variables);
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
