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

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/encryption_fields_validation.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

TEST(FLEValidationUtils, ValidateDoublePrecisionRange) {
    ASSERT(validateDoublePrecisionRange(3.000, 0));
    ASSERT(validateDoublePrecisionRange(3.000, 1));
    ASSERT(validateDoublePrecisionRange(3.000, 2));
    ASSERT(validateDoublePrecisionRange(-3.000, 0));
    ASSERT(validateDoublePrecisionRange(-3.000, 1));
    ASSERT(validateDoublePrecisionRange(-3.000, 2));

    ASSERT_FALSE(validateDoublePrecisionRange(3.100, 0));
    ASSERT(validateDoublePrecisionRange(3.100, 1));
    ASSERT(validateDoublePrecisionRange(3.100, 2));
    ASSERT_FALSE(validateDoublePrecisionRange(-3.100, 0));
    ASSERT(validateDoublePrecisionRange(-3.100, 1));
    ASSERT(validateDoublePrecisionRange(-3.100, 2));

    ASSERT(validateDoublePrecisionRange(1.000, 3));
    ASSERT(validateDoublePrecisionRange(-1.000, 3));

    ASSERT_FALSE(validateDoublePrecisionRange(3.140, 1));
    ASSERT_FALSE(validateDoublePrecisionRange(-3.140, 1));

    ASSERT(validateDoublePrecisionRange(0.000, 0));
    ASSERT(validateDoublePrecisionRange(0.000, 1));
    ASSERT(validateDoublePrecisionRange(0.000, 50));
    ASSERT(validateDoublePrecisionRange(-0.000, 0));
    ASSERT(validateDoublePrecisionRange(-0.000, 1));
    ASSERT(validateDoublePrecisionRange(-0.000, 50));
}

bool validateDecimal128PrecisionRangeTest(std::string s, uint32_t precision) {
    Decimal128 dec(s);
    return validateDecimal128PrecisionRange(dec, precision);
}

TEST(FLEValidationUtils, ValidateDecimalPrecisionRange) {

    ASSERT(validateDecimal128PrecisionRangeTest("3.000", 0));
    ASSERT(validateDecimal128PrecisionRangeTest("3.000", 1));
    ASSERT(validateDecimal128PrecisionRangeTest("3.000", 2));
    ASSERT(validateDecimal128PrecisionRangeTest("-3.000", 0));
    ASSERT(validateDecimal128PrecisionRangeTest("-3.000", 1));
    ASSERT(validateDecimal128PrecisionRangeTest("-3.000", 2));

    ASSERT_FALSE(validateDecimal128PrecisionRangeTest("3.100", 0));
    ASSERT(validateDecimal128PrecisionRangeTest("3.100", 1));
    ASSERT(validateDecimal128PrecisionRangeTest("3.100", 2));
    ASSERT_FALSE(validateDecimal128PrecisionRangeTest("-3.100", 0));
    ASSERT(validateDecimal128PrecisionRangeTest("-3.100", 1));
    ASSERT(validateDecimal128PrecisionRangeTest("-3.100", 2));

    ASSERT_FALSE(validateDecimal128PrecisionRangeTest("3.140", 1));
    ASSERT_FALSE(validateDecimal128PrecisionRangeTest("-3.140", 1));

    ASSERT(validateDecimal128PrecisionRangeTest("0.000", 0));
    ASSERT(validateDecimal128PrecisionRangeTest("0.000", 1));
    ASSERT(validateDecimal128PrecisionRangeTest("0.000", 50));
    ASSERT(validateDecimal128PrecisionRangeTest("-0.000", 0));
    ASSERT(validateDecimal128PrecisionRangeTest("-0.000", 1));
    ASSERT(validateDecimal128PrecisionRangeTest("-0.000", 50));
}

Status validateRangeIndexTest(int trimFactor,
                              BSONType fieldType,
                              const boost::optional<Value>& min,
                              const boost::optional<Value>& max,
                              const boost::optional<int32_t>& precision = boost::none) {
    QueryTypeConfig indexConfig;
    indexConfig.setMin(min);
    indexConfig.setMax(max);
    indexConfig.setPrecision(precision);
    indexConfig.setTrimFactor(trimFactor);
    indexConfig.setSparsity(1);
    try {
        validateRangeIndex(fieldType, "rangeField"_sd, indexConfig);
        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

TEST(FLEValidationUtils, ValidateTrimFactorRange) {
    // 2^1 == 2 values in domain, we can't trim
    ASSERT_OK(validateRangeIndexTest(0, BSONType::NumberInt, Value(0), Value(1)));
    ASSERT_NOT_OK(validateRangeIndexTest(1, BSONType::NumberInt, Value(0), Value(1)));

    // 2^2 > 3 values in domain, we can trim just the root
    ASSERT_OK(validateRangeIndexTest(1, BSONType::NumberInt, Value(0), Value(2)));
    ASSERT_NOT_OK(validateRangeIndexTest(2, BSONType::NumberInt, Value(0), Value(2)));

    ASSERT_OK(validateRangeIndexTest(3, BSONType::NumberInt, Value(INT32_MIN), Value(INT32_MAX)));
    ASSERT_NOT_OK(
        validateRangeIndexTest(32, BSONType::NumberInt, Value(INT32_MIN), Value(INT32_MAX)));

    ASSERT_OK(validateRangeIndexTest(
        3, BSONType::NumberLong, Value((long long)INT64_MIN), Value((long long)INT64_MAX)));
    ASSERT_NOT_OK(validateRangeIndexTest(
        64, BSONType::NumberLong, Value((long long)INT64_MIN), Value((long long)INT64_MAX)));

    ASSERT_OK(
        validateRangeIndexTest(3, BSONType::Date, Value(Date_t::min()), Value(Date_t::max())));
    ASSERT_NOT_OK(
        validateRangeIndexTest(64, BSONType::Date, Value(Date_t::min()), Value(Date_t::max())));

    // (2^10 > ) 1000 values in domain, we can trim top 9 layers
    ASSERT_OK(validateRangeIndexTest(
        9, BSONType::NumberDouble, Value(0.), Value(100.), 1 /* precision */));
    ASSERT_NOT_OK(validateRangeIndexTest(
        10, BSONType::NumberDouble, Value(0.), Value(100.), 1 /* precision */));

    ASSERT_OK(validateRangeIndexTest(3, BSONType::NumberDouble, boost::none, boost::none));
    ASSERT_NOT_OK(validateRangeIndexTest(64, BSONType::NumberDouble, boost::none, boost::none));

    ASSERT_OK(validateRangeIndexTest(9,
                                     BSONType::NumberDecimal,
                                     Value(Decimal128(0)),
                                     Value(Decimal128(100)),
                                     1 /* precision */));
    ASSERT_NOT_OK(validateRangeIndexTest(10,
                                         BSONType::NumberDecimal,
                                         Value(Decimal128(0)),
                                         Value(Decimal128(100)),
                                         1 /* precision */));

    ASSERT_OK(validateRangeIndexTest(7, BSONType::NumberDecimal, boost::none, boost::none));
    ASSERT_NOT_OK(validateRangeIndexTest(128, BSONType::NumberDecimal, boost::none, boost::none));
}

TEST(FLEValidationUtils, ValidateTrimFactorRangeInt32) {
    // Positive: Santiy check
    validateRangeBoundsInt32(1, 100, 1, 1);
    validateRangeBoundsInt32(1, 1000000, 1, 1);

    // Positive: Small bounds but bogus tf and sp
    validateRangeBoundsInt32(1, 100, 100, 100);

    // Negative: Bogus tf or sp
    ASSERT_THROWS_CODE(validateRangeBoundsInt32(1, 1000000, 100, 1), AssertionException, 9203504);
    ASSERT_THROWS_CODE(validateRangeBoundsInt32(1, 1000000, 1, 100), AssertionException, 9203504);

    // Limits
    validateRangeBoundsInt32(0, INT32_MAX, 1, 1);
    validateRangeBoundsInt32(INT32_MIN, 0, 1, 1);
    validateRangeBoundsInt32(INT32_MIN, INT32_MAX, 1, 1);

    // Negative: High sp + tf
    ASSERT_THROWS_CODE(validateRangeBoundsInt32(1, 10000000, 8, 13), AssertionException, 9203504);

    // Negative: High sp + tf + domain
    ASSERT_THROWS_CODE(validateRangeBoundsInt32(1, 10000000, 8, 11), AssertionException, 9203508);
}

TEST(FLEValidationUtils, ValidateTrimFactorRangeInt64) {
    // Positive: Santiy check
    validateRangeBoundsInt64(1, 100, 1, 1);
    validateRangeBoundsInt64(1, 1000000, 1, 1);

    // Positive: Small bounds but bogus tf and sp
    validateRangeBoundsInt64(1, 100, 100, 100);

    // Negative: Bogus tf or sp
    ASSERT_THROWS_CODE(validateRangeBoundsInt64(1, 1000000, 100, 1), AssertionException, 9203504);
    ASSERT_THROWS_CODE(validateRangeBoundsInt64(1, 1000000, 1, 100), AssertionException, 9203504);

    // Limits
    validateRangeBoundsInt64(0, INT64_MAX, 1, 1);
    validateRangeBoundsInt64(INT64_MIN, 0, 1, 1);
    validateRangeBoundsInt64(INT64_MIN, INT64_MAX, 1, 1);

    // Negative: High sp + tf
    ASSERT_THROWS_CODE(validateRangeBoundsInt64(1, 1000000, 8, 13), AssertionException, 9203504);

    // Negative: High sp + tf + domain
    ASSERT_THROWS_CODE(validateRangeBoundsInt64(1, 10000000, 8, 11), AssertionException, 9203508);
}

TEST(FLEValidationUtils, ValidateTrimFactorRangeDouble) {
    // Positive: Santiy check
    validateRangeBoundsDouble(1, 100, 1, 1, boost::none);
    validateRangeBoundsDouble(1, 1000000, 1, 1, boost::none);

    // Negative: Small bounds but bogus tf and sp
    ASSERT_THROWS_CODE(
        validateRangeBoundsDouble(1, 100, 100, 100, boost::none), AssertionException, 9203504);

    // Negative: Bogus tf or sp
    ASSERT_THROWS_CODE(
        validateRangeBoundsDouble(1, 1000000, 100, 1, boost::none), AssertionException, 9203504);
    ASSERT_THROWS_CODE(
        validateRangeBoundsDouble(1, 1000000, 1, 100, boost::none), AssertionException, 9203504);

    // Limits
    // Negative: High sp + tf
    ASSERT_THROWS_CODE(
        validateRangeBoundsDouble(1, 1000000, 8, 13, 1), AssertionException, 9203504);

    // Negative: High sp + tf + domain
    ASSERT_THROWS_CODE(
        validateRangeBoundsDouble(1, 10000000, 8, 11, 1), AssertionException, 9203508);


    validateRangeBoundsDouble(0, DBL_MAX, 1, 1, boost::none);
    validateRangeBoundsDouble(DBL_MIN, 1, 1, 1, boost::none);
    validateRangeBoundsDouble(DBL_TRUE_MIN, 1, 1, 1, boost::none);
    validateRangeBoundsDouble(DBL_MIN, DBL_MAX, 1, 1, boost::none);

    // Negative: High sp + tf
    ASSERT_THROWS_CODE(
        validateRangeBoundsDouble(1, 1000000, 8, 13, boost::none), AssertionException, 9203504);

    // Negative: High sp + tf + domain
    ASSERT_THROWS_CODE(
        validateRangeBoundsDouble(1, 10000000, 8, 11, boost::none), AssertionException, 9203508);

    // precision testing
    // force dmoain to be smaller then max tags and ignore tf/sp
    validateRangeBoundsDouble(1, 100, 100, 100, 2);
}

TEST(FLEValidationUtils, ValidateTrimFactorRangeDecimal128) {
    // Positive: Santiy check
    validateRangeBoundsDecimal128(Decimal128(1), Decimal128(100), 1, 1, boost::none);
    validateRangeBoundsDecimal128(Decimal128(1), Decimal128(1000000), 1, 1, boost::none);

    // Negative: Small bounds but bogus tf and sp
    ASSERT_THROWS_CODE(
        validateRangeBoundsDecimal128(Decimal128(1), Decimal128(100), 100, 100, boost::none),
        AssertionException,
        9203504);

    // Negative: Bogus tf or sp
    ASSERT_THROWS_CODE(
        validateRangeBoundsDecimal128(Decimal128(1), Decimal128(1000000), 100, 1, boost::none),
        AssertionException,
        9203504);
    ASSERT_THROWS_CODE(
        validateRangeBoundsDecimal128(Decimal128(1), Decimal128(1000000), 1, 100, boost::none),
        AssertionException,
        9203504);

    // Limits
    // Negative: High sp + tf
    ASSERT_THROWS_CODE(validateRangeBoundsDecimal128(Decimal128(1), Decimal128(1000000), 8, 13, 1),
                       AssertionException,
                       9203504);

    // Negative: High sp + tf + domain
    ASSERT_THROWS_CODE(validateRangeBoundsDecimal128(Decimal128(1), Decimal128(10000000), 8, 11, 1),
                       AssertionException,
                       9203508);

    // Negative: High sp + tf
    ASSERT_THROWS_CODE(
        validateRangeBoundsDecimal128(Decimal128(1), Decimal128(1000000), 8, 13, boost::none),
        AssertionException,
        9203504);

    // Negative: High sp + tf + domain
    ASSERT_THROWS_CODE(
        validateRangeBoundsDecimal128(Decimal128(1), Decimal128(10000000), 8, 11, boost::none),
        AssertionException,
        9203508);

    // precision testing
    // force dmoain to be smaller then max tags and ignore tf/sp
    validateRangeBoundsDecimal128(Decimal128(1), Decimal128(100), 100, 100, 2);
}

QueryTypeConfig validateTextSearchIndexCommonTests(QueryTypeEnum qtype) {
    constexpr StringData field = "foo"_sd;
    constexpr int32_t kMin = 2, kMax = 80;
    QueryTypeConfig qtc;
    qtc.setQueryType(qtype);
    qtc.setStrMaxLength(100);
    qtc.setStrMinQueryLength(kMin);
    qtc.setStrMaxQueryLength(kMax);
    qtc.setCaseSensitive(true);
    qtc.setDiacriticSensitive(false);
    qtc.setContention(8);

    validateTextSearchIndex(BSONType::String, field, qtc, boost::none, boost::none, boost::none);
    validateTextSearchIndex(BSONType::String,
                            field,
                            qtc,
                            qtc.getCaseSensitive(),
                            qtc.getDiacriticSensitive(),
                            qtc.getContention());

    // Bad Type
    ASSERT_THROWS_CODE(validateTextSearchIndex(
                           BSONType::Symbol, field, qtc, boost::none, boost::none, boost::none),
                       AssertionException,
                       9783400);

    // Missing min query length
    qtc.setStrMinQueryLength(boost::none);
    ASSERT_THROWS_CODE(validateTextSearchIndex(
                           BSONType::String, field, qtc, boost::none, boost::none, boost::none),
                       AssertionException,
                       9783402);
    qtc.setStrMinQueryLength(kMin);

    // Missing max query length
    qtc.setStrMaxQueryLength(boost::none);
    ASSERT_THROWS_CODE(validateTextSearchIndex(
                           BSONType::String, field, qtc, boost::none, boost::none, boost::none),
                       AssertionException,
                       9783403);
    qtc.setStrMaxQueryLength(kMax);

    // Missing case sensitive
    qtc.setCaseSensitive(boost::none);
    ASSERT_THROWS_CODE(validateTextSearchIndex(
                           BSONType::String, field, qtc, boost::none, boost::none, boost::none),
                       AssertionException,
                       9783404);
    qtc.setCaseSensitive(true);

    // Missing diacritic sensitive
    qtc.setDiacriticSensitive(boost::none);
    ASSERT_THROWS_CODE(validateTextSearchIndex(
                           BSONType::String, field, qtc, boost::none, boost::none, boost::none),
                       AssertionException,
                       9783405);
    qtc.setDiacriticSensitive(false);

    // min > max
    qtc.setStrMinQueryLength(kMax + 1);
    ASSERT_THROWS_CODE(validateTextSearchIndex(
                           BSONType::String, field, qtc, boost::none, boost::none, boost::none),
                       AssertionException,
                       9783406);
    qtc.setStrMinQueryLength(kMin);

    // Case sensitive does not match previous
    ASSERT_THROWS_CODE(validateTextSearchIndex(BSONType::String,
                                               field,
                                               qtc,
                                               !qtc.getCaseSensitive().value(),
                                               boost::none,
                                               boost::none),
                       AssertionException,
                       9783409);

    // Diacritic sensitive does not match previous
    ASSERT_THROWS_CODE(validateTextSearchIndex(BSONType::String,
                                               field,
                                               qtc,
                                               boost::none,
                                               !qtc.getDiacriticSensitive().value(),
                                               boost::none),
                       AssertionException,
                       9783410);

    // Contention factor does not match previous
    ASSERT_THROWS_CODE(
        validateTextSearchIndex(
            BSONType::String, field, qtc, boost::none, boost::none, qtc.getContention() + 1),
        AssertionException,
        9783411);
    return qtc;
}

TEST(FLEValidationUtils, ValidateTextSearchIndexSubstring) {
    QueryTypeConfig qtc = validateTextSearchIndexCommonTests(QueryTypeEnum::SubstringPreview);

    // Missing max length
    qtc.setStrMaxLength(boost::none);
    ASSERT_THROWS_CODE(validateTextSearchIndex(
                           BSONType::String, "foo"_sd, qtc, boost::none, boost::none, boost::none),
                       AssertionException,
                       9783407);
    // max query length > max length
    qtc.setStrMaxLength(100);
    qtc.setStrMaxQueryLength(1000);
    qtc.setStrMinQueryLength(1);
    ASSERT_THROWS_CODE(validateTextSearchIndex(
                           BSONType::String, "foo"_sd, qtc, boost::none, boost::none, boost::none),
                       AssertionException,
                       9783408);
}

TEST(FLEValidationUtils, ValidateTextSearchIndexSuffix) {
    validateTextSearchIndexCommonTests(QueryTypeEnum::SuffixPreview);
}

TEST(FLEValidationUtils, ValidateTextSearchIndexPrefix) {
    validateTextSearchIndexCommonTests(QueryTypeEnum::PrefixPreview);
}

TEST(FLEValidationUtils, ValidateTextSearchIndexBadQueryType) {
    QueryTypeConfig qtc;
    for (auto qtype :
         {QueryTypeEnum::Equality, QueryTypeEnum::RangePreviewDeprecated, QueryTypeEnum::Range}) {
        qtc.setQueryType(qtype);
        ASSERT_THROWS_CODE(
            validateTextSearchIndex(
                BSONType::String, "foo"_sd, qtc, boost::none, boost::none, boost::none),
            AssertionException,
            9783401);
    }
}

}  // namespace
}  // namespace mongo
