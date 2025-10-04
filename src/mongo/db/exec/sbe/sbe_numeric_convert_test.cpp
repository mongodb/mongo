/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <limits>
#include <memory>

#include <fmt/format.h>

namespace mongo::sbe {
namespace test_detail {
template <typename T>
std::unique_ptr<sbe::EExpression> makeEFromNumber(const T in,
                                                  value::TypeTags sourceTag,
                                                  value::TypeTags targetTag) {
    return makeE<ENumericConvert>(makeE<EConstant>(sourceTag, value::bitcastFrom<T>(in)),
                                  targetTag);
}

template <>
std::unique_ptr<sbe::EExpression> makeEFromNumber<mongo::Decimal128>(const Decimal128 in,
                                                                     value::TypeTags sourceTag,
                                                                     value::TypeTags targetTag) {
    auto [tag, value] = sbe::value::makeCopyDecimal(in);
    return makeE<ENumericConvert>(makeE<EConstant>(tag, value), targetTag);
}
}  // namespace test_detail

class SBENumericTest : public EExpressionTestFixture {
protected:
    // Assert that convert(input) == output.
    template <typename Input, typename Output>
    void assertConversion(const Input input,
                          const Output output,
                          const value::TypeTags srcTag,
                          const value::TypeTags targetTag) {

        auto expr = test_detail::makeEFromNumber(input, srcTag, targetTag);
        auto compiledExpr = compileExpression(*expr);
        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);

        ASSERT_EQUALS(tag, targetTag);

        if constexpr (std::is_same_v<Output, Decimal128>) {
            ASSERT(value::bitcastTo<Decimal128>(val).isEqual(output));
        } else if constexpr (std::is_same_v<Output, double>) {
            ASSERT_APPROX_EQUAL(
                value::bitcastTo<Output>(val), output, std::numeric_limits<double>::epsilon());
        } else {
            ASSERT_EQUALS(value::bitcastTo<Output>(val), output);
        }
    }

    // assert that a conversion is lossy.
    template <typename T>
    void assertLossy(const T input, const value::TypeTags srcTag, const value::TypeTags targetTag) {
        auto expr = test_detail::makeEFromNumber(input, srcTag, targetTag);
        auto compiledExpr = compileExpression(*expr);

        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(tag, val);
        ASSERT_EQUALS(tag, value::TypeTags::Nothing);
    }
};

TEST_F(SBENumericTest, Compile) {
    GoldenTestContext gctx(&goldenTestConfigSbe);
    gctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    auto& os = gctx.outStream();

    auto expr = test_detail::makeEFromNumber(
        Decimal128(123), value::TypeTags::NumberDecimal, value::TypeTags::NumberInt64);
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);
}

TEST_F(SBENumericTest, Int32ToInt64) {
    assertConversion(int32_t{-2147483648},
                     int64_t{-2147483648},
                     value::TypeTags::NumberInt32,
                     value::TypeTags::NumberInt64);
    assertConversion(
        int32_t{-10}, int64_t{-10}, value::TypeTags::NumberInt32, value::TypeTags::NumberInt64);
    assertConversion(
        int32_t{0}, int64_t{0}, value::TypeTags::NumberInt32, value::TypeTags::NumberInt64);
    assertConversion(
        int32_t{-0}, int64_t{-0}, value::TypeTags::NumberInt32, value::TypeTags::NumberInt64);
    assertConversion(
        int32_t{10}, int64_t{10}, value::TypeTags::NumberInt32, value::TypeTags::NumberInt64);
    assertConversion(int32_t{2147483647},
                     int64_t{2147483647},
                     value::TypeTags::NumberInt32,
                     value::TypeTags::NumberInt64);
}

TEST_F(SBENumericTest, Int32ToDouble) {
    assertConversion(
        int32_t{-2}, double{-2.0}, value::TypeTags::NumberInt32, value::TypeTags::NumberDouble);
    assertConversion(
        int32_t{-1}, double{-1.0}, value::TypeTags::NumberInt32, value::TypeTags::NumberDouble);
    assertConversion(
        int32_t{0}, double{0.0}, value::TypeTags::NumberInt32, value::TypeTags::NumberDouble);
    assertConversion(
        int32_t{-0}, double{-0.0}, value::TypeTags::NumberInt32, value::TypeTags::NumberDouble);
    assertConversion(
        int32_t{1}, double{1.0}, value::TypeTags::NumberInt32, value::TypeTags::NumberDouble);
}

TEST_F(SBENumericTest, Int32ToDecimal) {
    assertConversion(int32_t{-2},
                     Decimal128{-2.0},
                     value::TypeTags::NumberInt32,
                     value::TypeTags::NumberDecimal);
    assertConversion(int32_t{-1},
                     Decimal128{-1.0},
                     value::TypeTags::NumberInt32,
                     value::TypeTags::NumberDecimal);
    assertConversion(
        int32_t{0}, Decimal128{0.0}, value::TypeTags::NumberInt32, value::TypeTags::NumberDecimal);
    assertConversion(int32_t{-0},
                     Decimal128{-0.0},
                     value::TypeTags::NumberInt32,
                     value::TypeTags::NumberDecimal);
    assertConversion(
        int32_t{1}, Decimal128{1.0}, value::TypeTags::NumberInt32, value::TypeTags::NumberDecimal);
}

TEST_F(SBENumericTest, Int64ToInt32) {
    assertConversion(int64_t{-2147483648},
                     int32_t{-2147483648},
                     value::TypeTags::NumberInt64,
                     value::TypeTags::NumberInt32);
    assertConversion(
        int64_t{-10}, int32_t{-10}, value::TypeTags::NumberInt64, value::TypeTags::NumberInt32);
    assertConversion(
        int64_t{0}, int32_t{0}, value::TypeTags::NumberInt64, value::TypeTags::NumberInt32);
    assertConversion(
        int64_t{-0}, int32_t{-0}, value::TypeTags::NumberInt64, value::TypeTags::NumberInt32);
    assertConversion(
        int64_t{10}, int32_t{10}, value::TypeTags::NumberInt64, value::TypeTags::NumberInt32);
    assertConversion(int64_t{2147483647},
                     int32_t{2147483647},
                     value::TypeTags::NumberInt64,
                     value::TypeTags::NumberInt32);
}

TEST_F(SBENumericTest, Int64ToInt64) {
    assertConversion(int64_t{-2147483648},
                     int64_t{-2147483648},
                     value::TypeTags::NumberInt64,
                     value::TypeTags::NumberInt64);
    assertConversion(
        int64_t{-10}, int64_t{-10}, value::TypeTags::NumberInt64, value::TypeTags::NumberInt64);
    assertConversion(
        int64_t{0}, int64_t{0}, value::TypeTags::NumberInt64, value::TypeTags::NumberInt64);
    assertConversion(
        int64_t{-0}, int64_t{-0}, value::TypeTags::NumberInt64, value::TypeTags::NumberInt64);
    assertConversion(
        int64_t{10}, int64_t{10}, value::TypeTags::NumberInt64, value::TypeTags::NumberInt64);
    assertConversion(int64_t{2147483647},
                     int64_t{2147483647},
                     value::TypeTags::NumberInt64,
                     value::TypeTags::NumberInt64);
}

TEST_F(SBENumericTest, Int64ToDouble) {
    assertConversion(int64_t{-2147483649},
                     double{-2147483649.0},
                     value::TypeTags::NumberInt64,
                     value::TypeTags::NumberDouble);
    assertConversion(
        int64_t{-10}, double{-10.0}, value::TypeTags::NumberInt64, value::TypeTags::NumberDouble);
    assertConversion(
        int64_t{-1}, double{-1.0}, value::TypeTags::NumberInt64, value::TypeTags::NumberDouble);
    assertConversion(
        int64_t{0}, double{0.0}, value::TypeTags::NumberInt64, value::TypeTags::NumberDouble);
    assertConversion(
        int64_t{-0}, double{-0.0}, value::TypeTags::NumberInt64, value::TypeTags::NumberDouble);
    assertConversion(
        int64_t{1}, double{1.0}, value::TypeTags::NumberInt64, value::TypeTags::NumberDouble);
    assertConversion(
        int64_t{10}, double{10.0}, value::TypeTags::NumberInt64, value::TypeTags::NumberDouble);
    assertConversion(int64_t{2147483648},
                     double{2147483648.0},
                     value::TypeTags::NumberInt64,
                     value::TypeTags::NumberDouble);
}

TEST_F(SBENumericTest, Int64ToDecimal) {
    assertConversion(int64_t{-2147483649},
                     Decimal128{-2147483649.0},
                     value::TypeTags::NumberInt64,
                     value::TypeTags::NumberDecimal);
    assertConversion(int64_t{-10},
                     Decimal128{-10.0},
                     value::TypeTags::NumberInt64,
                     value::TypeTags::NumberDecimal);
    assertConversion(int64_t{-1},
                     Decimal128{-1.0},
                     value::TypeTags::NumberInt64,
                     value::TypeTags::NumberDecimal);
    assertConversion(
        int64_t{0}, Decimal128{0.0}, value::TypeTags::NumberInt64, value::TypeTags::NumberDecimal);
    assertConversion(int64_t{-0},
                     Decimal128{-0.0},
                     value::TypeTags::NumberInt64,
                     value::TypeTags::NumberDecimal);
    assertConversion(
        int64_t{1}, Decimal128{1.0}, value::TypeTags::NumberInt64, value::TypeTags::NumberDecimal);
    assertConversion(int64_t{10},
                     Decimal128{10.0},
                     value::TypeTags::NumberInt64,
                     value::TypeTags::NumberDecimal);
    assertConversion(int64_t{2147483648},
                     Decimal128{2147483648.0},
                     value::TypeTags::NumberInt64,
                     value::TypeTags::NumberDecimal);
}

TEST_F(SBENumericTest, Decimal128ToInt32) {
    assertConversion(Decimal128{-2147483648.0},
                     int32_t{-2147483648},
                     value::TypeTags::NumberDecimal,
                     value::TypeTags::NumberInt32);
    assertConversion(Decimal128{-10.0},
                     int32_t{-10},
                     value::TypeTags::NumberDecimal,
                     value::TypeTags::NumberInt32);
    assertConversion(
        Decimal128{0.0}, int32_t{0}, value::TypeTags::NumberDecimal, value::TypeTags::NumberInt32);
    assertConversion(
        Decimal128{1.0}, int32_t{1}, value::TypeTags::NumberDecimal, value::TypeTags::NumberInt32);
    assertConversion(Decimal128{10.0},
                     int32_t{10},
                     value::TypeTags::NumberDecimal,
                     value::TypeTags::NumberInt32);
    assertConversion(Decimal128{2147483647.0},
                     int32_t{2147483647},
                     value::TypeTags::NumberDecimal,
                     value::TypeTags::NumberInt32);
}

TEST_F(SBENumericTest, Decimal128ToInt64) {
    assertConversion(Decimal128{-2147483649.0},
                     int64_t{-2147483649},
                     value::TypeTags::NumberDecimal,
                     value::TypeTags::NumberInt64);
    assertConversion(Decimal128{-10.0},
                     int64_t{-10},
                     value::TypeTags::NumberDecimal,
                     value::TypeTags::NumberInt64);
    assertConversion(
        Decimal128{0.0}, int64_t{0}, value::TypeTags::NumberDecimal, value::TypeTags::NumberInt64);
    assertConversion(
        Decimal128{1.0}, int64_t{1}, value::TypeTags::NumberDecimal, value::TypeTags::NumberInt64);
    assertConversion(Decimal128{10.0},
                     int64_t{10},
                     value::TypeTags::NumberDecimal,
                     value::TypeTags::NumberInt64);
    assertConversion(Decimal128{2147483648.0},
                     int64_t{2147483648},
                     value::TypeTags::NumberDecimal,
                     value::TypeTags::NumberInt64);
}

TEST_F(SBENumericTest, Decimal128ToDouble) {
    assertConversion(Decimal128{-2147483649.0},
                     double{-2147483649},
                     value::TypeTags::NumberDecimal,
                     value::TypeTags::NumberDouble);
    assertConversion(Decimal128{-10.0},
                     double{-10},
                     value::TypeTags::NumberDecimal,
                     value::TypeTags::NumberDouble);
    assertConversion(
        Decimal128{0.0}, double{0}, value::TypeTags::NumberDecimal, value::TypeTags::NumberDouble);
    assertConversion(
        Decimal128{1.0}, double{1}, value::TypeTags::NumberDecimal, value::TypeTags::NumberDouble);
    assertConversion(Decimal128{10.0},
                     double{10},
                     value::TypeTags::NumberDecimal,
                     value::TypeTags::NumberDouble);
    assertConversion(Decimal128{2147483648.0},
                     double{2147483648},
                     value::TypeTags::NumberDecimal,
                     value::TypeTags::NumberDouble);
}

TEST_F(SBENumericTest, DoubleToInt32) {
    assertConversion(double{-2147483648.0},
                     int32_t{-2147483648},
                     value::TypeTags::NumberDouble,
                     value::TypeTags::NumberInt32);
    assertConversion(
        double{-10.0}, int32_t{-10}, value::TypeTags::NumberDouble, value::TypeTags::NumberInt32);
    assertConversion(
        double{0.0}, int32_t{0}, value::TypeTags::NumberDouble, value::TypeTags::NumberInt32);
    assertConversion(
        double{1.0}, int32_t{1}, value::TypeTags::NumberDouble, value::TypeTags::NumberInt32);
    assertConversion(
        double{10.0}, int32_t{10}, value::TypeTags::NumberDouble, value::TypeTags::NumberInt32);
    assertConversion(double{2147483647.0},
                     int32_t{2147483647},
                     value::TypeTags::NumberDouble,
                     value::TypeTags::NumberInt32);
}

TEST_F(SBENumericTest, DoubleToInt64) {
    assertConversion(double{-2147483649.0},
                     int64_t{-2147483649},
                     value::TypeTags::NumberDouble,
                     value::TypeTags::NumberInt64);
    assertConversion(
        double{-10.0}, int64_t{-10}, value::TypeTags::NumberDouble, value::TypeTags::NumberInt64);
    assertConversion(
        double{0.0}, int64_t{0}, value::TypeTags::NumberDouble, value::TypeTags::NumberInt64);
    assertConversion(
        double{1.0}, int64_t{1}, value::TypeTags::NumberDouble, value::TypeTags::NumberInt64);
    assertConversion(
        double{10.0}, int64_t{10}, value::TypeTags::NumberDouble, value::TypeTags::NumberInt64);
    assertConversion(
        double{9999.0}, int64_t{9999}, value::TypeTags::NumberDouble, value::TypeTags::NumberInt64);
    assertConversion(double{2147483647.0},
                     int64_t{2147483647},
                     value::TypeTags::NumberDouble,
                     value::TypeTags::NumberInt64);
}

TEST_F(SBENumericTest, DoubleToDecimal) {
    assertConversion(double{-2147483649.0},
                     Decimal128{-2147483649},
                     value::TypeTags::NumberDouble,
                     value::TypeTags::NumberDecimal);
    assertConversion(double{-10.0},
                     Decimal128{-10},
                     value::TypeTags::NumberDouble,
                     value::TypeTags::NumberDecimal);
    assertConversion(
        double{0.0}, Decimal128{0}, value::TypeTags::NumberDouble, value::TypeTags::NumberDecimal);
    assertConversion(
        double{1.0}, Decimal128{1}, value::TypeTags::NumberDouble, value::TypeTags::NumberDecimal);
    assertConversion(double{10.0},
                     Decimal128{10},
                     value::TypeTags::NumberDouble,
                     value::TypeTags::NumberDecimal);
    assertConversion(double{9999.0},
                     Decimal128{9999},
                     value::TypeTags::NumberDouble,
                     value::TypeTags::NumberDecimal);
    assertConversion(double{2147483647.0},
                     Decimal128{2147483647},
                     value::TypeTags::NumberDouble,
                     value::TypeTags::NumberDecimal);
}

TEST_F(SBENumericTest, LossyConvertsToNothing) {
    assertLossy(int64_t{2147483648}, value::TypeTags::NumberInt64, value::TypeTags::NumberInt32);
    assertLossy(Decimal128{0.1}, value::TypeTags::NumberDecimal, value::TypeTags::NumberInt32);
    assertLossy(Decimal128{0.1}, value::TypeTags::NumberDecimal, value::TypeTags::NumberInt64);
    assertLossy(
        Decimal128{"1.9E308"}, value::TypeTags::NumberDecimal, value::TypeTags::NumberDouble);
    assertLossy(double{1999.1}, value::TypeTags::NumberDouble, value::TypeTags::NumberInt32);
    assertLossy(double{1999.1}, value::TypeTags::NumberDouble, value::TypeTags::NumberInt64);
}

}  // namespace mongo::sbe
