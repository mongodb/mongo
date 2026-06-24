/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/crypto/fle_payload_validation.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/encryption_fields_validation.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
using QueriesVariant = std::variant<std::vector<QueryTypeConfig>, QueryTypeConfig>;

QueryTypeConfig equalityQtc(int64_t contention) {
    QueryTypeConfig q;
    q.setQueryType(QueryTypeEnum::Equality);
    q.setContention(contention);
    return q;
}

QueryTypeConfig rangeQtc(int64_t contention,
                         boost::optional<int64_t> sparsity = boost::none,
                         boost::optional<int32_t> precision = boost::none,
                         boost::optional<Value> min = boost::none,
                         boost::optional<Value> max = boost::none) {
    QueryTypeConfig q;
    q.setQueryType(QueryTypeEnum::Range);
    q.setContention(contention);
    q.setSparsity(sparsity);
    q.setPrecision(precision);
    q.setMin(min);
    q.setMax(max);
    return q;
}

QueryTypeConfig textQtc(QueryTypeEnum qt, int64_t contention) {
    QueryTypeConfig q;
    q.setQueryType(qt);
    q.setContention(contention);
    return q;
}

EncryptedField fieldWith(std::string_view path, QueryTypeConfig qtc) {
    EncryptedField ef(UUID::gen(), std::string{path});
    ef.setBsonType("int"_sd);
    ef.setQueries(QueriesVariant{std::move(qtc)});
    return ef;
}

EncryptedField fieldWith(std::string_view path, std::vector<QueryTypeConfig> qtcs) {
    EncryptedField ef(UUID::gen(), std::string{path});
    ef.setBsonType("string"_sd);
    ef.setQueries(QueriesVariant{std::move(qtcs)});
    return ef;
}

FLE2PayloadParams sampled(QueryTypeEnum expectedType,
                          boost::optional<int64_t> contention = boost::none) {
    FLE2PayloadParams p;
    p.contentionKind = FLE2PayloadParams::ContentionKind::kSampled;
    p.contention = contention;
    p.expectedTypes = {expectedType};
    return p;
}
FLE2PayloadParams configuredMax(QueryTypeEnum expectedType,
                                boost::optional<int64_t> contention = boost::none) {
    FLE2PayloadParams p;
    p.contentionKind = FLE2PayloadParams::ContentionKind::kConfiguredMax;
    p.contention = contention;
    p.expectedTypes = {expectedType};
    return p;
}
}  // namespace

TEST(FLEPayloadValidation, QueryTypeMismatchThrows) {
    auto field = fieldWith("encrypted", equalityQtc(8));
    ASSERT_THROWS_CODE(
        validatePayloadAgainstQueryTypeConfig("encrypted", field, sampled(QueryTypeEnum::Range)),
        DBException,
        9188707);
}

TEST(FLEPayloadValidation, NoQueriesThrows) {
    EncryptedField field(UUID::gen(), "encrypted");
    field.setBsonType("int"_sd);
    ASSERT_THROWS_CODE(
        validatePayloadAgainstQueryTypeConfig("encrypted", field, sampled(QueryTypeEnum::Equality)),
        DBException,
        9188707);
}

TEST(FLEPayloadValidation, ContentionSampledWithinMax) {
    auto field = fieldWith("encrypted", equalityQtc(8));
    for (int64_t k : {int64_t(0), int64_t(1), int64_t(8)}) {
        validatePayloadAgainstQueryTypeConfig(
            "encrypted", field, sampled(QueryTypeEnum::Equality, k));
    }
}

TEST(FLEPayloadValidation, ContentionSampledExceedsMax) {
    auto field = fieldWith("encrypted", equalityQtc(1));
    ASSERT_THROWS_CODE(validatePayloadAgainstQueryTypeConfig(
                           "encrypted", field, sampled(QueryTypeEnum::Equality, int64_t(100))),
                       DBException,
                       9188700);
}

TEST(FLEPayloadValidation, ContentionMaxEquality) {
    auto field = fieldWith("encrypted", rangeQtc(4));
    validatePayloadAgainstQueryTypeConfig(
        "encrypted", field, configuredMax(QueryTypeEnum::Range, int64_t(4)));
    ASSERT_THROWS_CODE(validatePayloadAgainstQueryTypeConfig(
                           "encrypted", field, configuredMax(QueryTypeEnum::Range, int64_t(3))),
                       DBException,
                       9188701);
}

TEST(FLEPayloadValidation, SoftModeContentionAbsent) {
    auto field = fieldWith("encrypted", equalityQtc(1));
    validatePayloadAgainstQueryTypeConfig("encrypted", field, sampled(QueryTypeEnum::Equality));
}

TEST(FLEPayloadValidation, SparsityMismatchAndSoftMode) {
    auto field = fieldWith("encrypted", rangeQtc(8, /*sparsity=*/int64_t(2)));
    auto bad = sampled(QueryTypeEnum::Range);
    bad.sparsity = int64_t(4);
    ASSERT_THROWS_CODE(
        validatePayloadAgainstQueryTypeConfig("encrypted", field, bad), DBException, 9188702);
    validatePayloadAgainstQueryTypeConfig("encrypted", field, sampled(QueryTypeEnum::Range));
}

TEST(FLEPayloadValidation, SparsityDefaultedWhenConfigOmits) {
    auto field = fieldWith("encrypted", rangeQtc(8));
    auto good = sampled(QueryTypeEnum::Range);
    good.sparsity = int64_t(kFLERangeSparsityDefault);
    validatePayloadAgainstQueryTypeConfig("encrypted", field, good);
    auto bad = sampled(QueryTypeEnum::Range);
    bad.sparsity = int64_t(kFLERangeSparsityDefault + 1);
    ASSERT_THROWS_CODE(
        validatePayloadAgainstQueryTypeConfig("encrypted", field, bad), DBException, 9188702);
}

TEST(FLEPayloadValidation, Precision) {
    auto field = fieldWith("encrypted", rangeQtc(8, boost::none, /*precision=*/int32_t(2)));
    auto good = sampled(QueryTypeEnum::Range);
    good.precision = int32_t(2);
    validatePayloadAgainstQueryTypeConfig("encrypted", field, good);
    auto badPrec = sampled(QueryTypeEnum::Range);
    badPrec.precision = int32_t(5);
    ASSERT_THROWS_CODE(
        validatePayloadAgainstQueryTypeConfig("encrypted", field, badPrec), DBException, 9188703);
}

TEST(FLEPayloadValidation, IndexMinMaxMatchAndMismatch) {
    auto field = fieldWith(
        "encrypted", rangeQtc(8, boost::none, boost::none, /*min=*/Value(0), /*max=*/Value(100)));
    auto matchingMinDoc = BSON("v" << 0);
    auto matchingMaxDoc = BSON("v" << 100);
    auto wrongMinDoc = BSON("v" << 5);
    auto wrongMaxDoc = BSON("v" << 200);
    auto good = sampled(QueryTypeEnum::Range);
    good.indexMin = matchingMinDoc.firstElement();
    good.indexMax = matchingMaxDoc.firstElement();
    validatePayloadAgainstQueryTypeConfig("encrypted", field, good);
    auto badMin = sampled(QueryTypeEnum::Range);
    badMin.indexMin = wrongMinDoc.firstElement();
    badMin.indexMax = matchingMaxDoc.firstElement();
    ASSERT_THROWS_CODE(
        validatePayloadAgainstQueryTypeConfig("encrypted", field, badMin), DBException, 9188705);
    auto badMax = sampled(QueryTypeEnum::Range);
    badMax.indexMin = matchingMinDoc.firstElement();
    badMax.indexMax = wrongMaxDoc.firstElement();
    ASSERT_THROWS_CODE(
        validatePayloadAgainstQueryTypeConfig("encrypted", field, badMax), DBException, 9188706);
}

TEST(FLEPayloadValidation, MultipleExpectedTypes) {
    auto field = fieldWith("encrypted",
                           {textQtc(QueryTypeEnum::Suffix, 4), textQtc(QueryTypeEnum::Prefix, 4)});
    auto good = sampled(QueryTypeEnum::Suffix);
    good.expectedTypes = {QueryTypeEnum::Suffix, QueryTypeEnum::Prefix};
    validatePayloadAgainstQueryTypeConfig("encrypted", field, good);

    // A field configured only for Suffix must reject a payload that also expects Prefix.
    auto suffixOnly = fieldWith("encrypted", {textQtc(QueryTypeEnum::Suffix, 4)});
    ASSERT_THROWS_CODE(
        validatePayloadAgainstQueryTypeConfig("encrypted", suffixOnly, good), DBException, 9188707);
}

TEST(FLEPayloadValidation, AcceptsDeprecatedPreviewVariant) {
    // A field created with the deprecated {substring/suffix/prefix}Preview type must accept a
    // payload generated for the GA substring/suffix/prefix query type, so pre-upgrade preview
    // collections stay operational (getAndValidateSchema blocks them once the GA feature flag is
    // on).
    auto substringPreview =
        fieldWith("encrypted", {textQtc(QueryTypeEnum::SubstringPreviewDeprecated, 4)});
    validatePayloadAgainstQueryTypeConfig(
        "encrypted", substringPreview, sampled(QueryTypeEnum::Substring));

    auto suffixPreview =
        fieldWith("encrypted", {textQtc(QueryTypeEnum::SuffixPreviewDeprecated, 4)});
    validatePayloadAgainstQueryTypeConfig(
        "encrypted", suffixPreview, sampled(QueryTypeEnum::Suffix));

    auto prefixPreview =
        fieldWith("encrypted", {textQtc(QueryTypeEnum::PrefixPreviewDeprecated, 4)});
    validatePayloadAgainstQueryTypeConfig(
        "encrypted", prefixPreview, sampled(QueryTypeEnum::Prefix));
}

TEST(FLEPayloadValidation, MatchAnyStringSearchType) {
    auto textField = fieldWith("encrypted", {textQtc(QueryTypeEnum::Suffix, 4)});
    FLE2PayloadParams anyText;
    anyText.matchAnyStringSearchType = true;
    validatePayloadAgainstQueryTypeConfig("encrypted", textField, anyText);

    // A non-text field has no string search query type to satisfy a normalized-equality payload.
    auto eqField = fieldWith("encrypted", equalityQtc(4));
    ASSERT_THROWS_CODE(
        validatePayloadAgainstQueryTypeConfig("encrypted", eqField, anyText), DBException, 9188704);
}

}  // namespace mongo
