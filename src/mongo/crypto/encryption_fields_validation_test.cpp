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
        validateRangeIndex(fieldType, indexConfig);
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

    ASSERT_OK(validateRangeIndexTest(31, BSONType::NumberInt, Value(INT32_MIN), Value(INT32_MAX)));
    ASSERT_NOT_OK(
        validateRangeIndexTest(32, BSONType::NumberInt, Value(INT32_MIN), Value(INT32_MAX)));

    ASSERT_OK(validateRangeIndexTest(
        63, BSONType::NumberLong, Value((long long)INT64_MIN), Value((long long)INT64_MAX)));
    ASSERT_NOT_OK(validateRangeIndexTest(
        64, BSONType::NumberLong, Value((long long)INT64_MIN), Value((long long)INT64_MAX)));

    ASSERT_OK(
        validateRangeIndexTest(63, BSONType::Date, Value(Date_t::min()), Value(Date_t::max())));
    ASSERT_NOT_OK(
        validateRangeIndexTest(64, BSONType::Date, Value(Date_t::min()), Value(Date_t::max())));

    // (2^10 > ) 1000 values in domain, we can trim top 9 layers
    ASSERT_OK(validateRangeIndexTest(
        9, BSONType::NumberDouble, Value(0.), Value(100.), 1 /* precision */));
    ASSERT_NOT_OK(validateRangeIndexTest(
        10, BSONType::NumberDouble, Value(0.), Value(100.), 1 /* precision */));

    ASSERT_OK(validateRangeIndexTest(63, BSONType::NumberDouble, boost::none, boost::none));
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

    ASSERT_OK(validateRangeIndexTest(127, BSONType::NumberDecimal, boost::none, boost::none));
    ASSERT_NOT_OK(validateRangeIndexTest(128, BSONType::NumberDecimal, boost::none, boost::none));
}
}  // namespace mongo
