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
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mongo::sbe {

namespace {
using SBEMathBuiltinTest = EExpressionTestFixture;

TEST_F(SBEMathBuiltinTest, Abs) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto callExpr = makeE<EFunction>("abs", makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-6));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(6, value::bitcastTo<int32_t>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt32,
                            value::bitcastFrom<int32_t>(std::numeric_limits<int32_t>::min()));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt64, resultTag);
        ASSERT_EQ(-static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
                  value::bitcastTo<int64_t>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-6000000000));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt64, resultTag);
        ASSERT_EQ(6000000000, value::bitcastTo<int64_t>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64,
                            value::bitcastFrom<int64_t>(std::numeric_limits<int64_t>::min()));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::Nothing, resultTag);
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-6e300));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_APPROX_EQUAL(6e300, value::bitcastTo<double>(resultVal), 1e297);
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"-6e300"});
        inputAccessor.reset(inputTag, inputVal);

        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guardResult(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDecimal, resultTag);
        ASSERT(Decimal128{"6e300"} == value::bitcastTo<Decimal128>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble,
                            value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN()));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_TRUE(std::isnan(value::bitcastTo<double>(resultVal)));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-NAN));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_TRUE(std::isnan(value::bitcastTo<double>(resultVal)));
    }
}

TEST_F(SBEMathBuiltinTest, Ceil) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto callExpr = makeE<EFunction>("ceil", makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0001));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_EQ(-10.0, value::bitcastTo<double>(resultVal));
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"-123.456"});
        inputAccessor.reset(inputTag, inputVal);

        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guardResult(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDecimal, resultTag);
        ASSERT(Decimal128{"-123"} == value::bitcastTo<Decimal128>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-10));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(-10, value::bitcastTo<int32_t>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64,
                            value::bitcastFrom<int64_t>(-10000000000));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt64, resultTag);
        ASSERT_EQ(-10000000000, value::bitcastTo<int64_t>(resultVal));
    }
}

TEST_F(SBEMathBuiltinTest, Floor) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto callExpr = makeE<EFunction>("floor", makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0001));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_EQ(-11.0, value::bitcastTo<double>(resultVal));
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"-123.456"});
        inputAccessor.reset(inputTag, inputVal);

        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guardResult(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDecimal, resultTag);
        ASSERT(Decimal128{"-124"} == value::bitcastTo<Decimal128>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-10));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(-10, value::bitcastTo<int32_t>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64,
                            value::bitcastFrom<int64_t>(-10000000000));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt64, resultTag);
        ASSERT_EQ(-10000000000, value::bitcastTo<int64_t>(resultVal));
    }
}

TEST_F(SBEMathBuiltinTest, Exp) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto callExpr = makeE<EFunction>("exp", makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_APPROX_EQUAL(7.389, value::bitcastTo<double>(resultVal), 0.001);
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_APPROX_EQUAL(20.08, value::bitcastTo<double>(resultVal), 0.01);
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.5));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_APPROX_EQUAL(12.18, value::bitcastTo<double>(resultVal), 0.01);
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"3.5"});
        inputAccessor.reset(inputTag, inputVal);
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDecimal, resultTag);
        auto expected = Decimal128{"33.12"};
        ASSERT(expected.subtract(value::bitcastTo<Decimal128>(resultVal))
                   .toAbs()
                   .isLess(Decimal128{"0.01"}));
    }
}

TEST_F(SBEMathBuiltinTest, Ln) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto callExpr = makeE<EFunction>("ln", makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_APPROX_EQUAL(0.6931, value::bitcastTo<double>(resultVal), 0.0001);
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(20000000000));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_APPROX_EQUAL(23.72, value::bitcastTo<double>(resultVal), 0.01);
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.1e20));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_APPROX_EQUAL(46.79, value::bitcastTo<double>(resultVal), 0.01);
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"4.2e25"});
        inputAccessor.reset(inputTag, inputVal);
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDecimal, resultTag);
        auto expected = Decimal128{"59.00"};
        ASSERT(expected.subtract(value::bitcastTo<Decimal128>(resultVal))
                   .toAbs()
                   .isLess(Decimal128{"0.01"}));
    }

    // Non-positive values evaluate to Nothing
    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::Nothing, resultTag);
    }
}

TEST_F(SBEMathBuiltinTest, Log10) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto callExpr = makeE<EFunction>("log10", makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(100));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_EQ(2.0, value::bitcastTo<double>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10000000000));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_EQ(10.0, value::bitcastTo<double>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.1e20));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_APPROX_EQUAL(20.322, value::bitcastTo<double>(resultVal), 0.01);
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"1e2000"});
        inputAccessor.reset(inputTag, inputVal);
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDecimal, resultTag);
        ASSERT(Decimal128{"2000"} == value::bitcastTo<Decimal128>(resultVal));
    }
}

TEST_F(SBEMathBuiltinTest, Sqrt) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto callExpr = makeE<EFunction>("sqrt", makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_EQ(2.0, value::bitcastTo<double>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10000000000));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_EQ(100000.0, value::bitcastTo<double>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.5));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_APPROX_EQUAL(1.581, value::bitcastTo<double>(resultVal), 0.001);
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"5.2"});
        inputAccessor.reset(inputTag, inputVal);
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDecimal, resultTag);
        auto expected = Decimal128{"2.280"};
        ASSERT(expected.subtract(value::bitcastTo<Decimal128>(resultVal))
                   .toAbs()
                   .isLess(Decimal128{"0.001"}));
    }

    // For types with signed zero, we do not distinguish -0 from 0. The domain of sqrt _does_
    // include -0, and -0 evaluates to a value that is equal to zero.
    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-0.0));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_EQ(0.0, value::bitcastTo<double>(resultVal));
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"-0.0"});
        inputAccessor.reset(inputTag, inputVal);
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDecimal, resultTag);
        ASSERT(value::bitcastTo<Decimal128>(resultVal).normalize().isEqual(
            Decimal128::kNormalizedZero));
    }
}

TEST_F(SBEMathBuiltinTest, Pow) {
    value::OwnedValueAccessor inputAccessor1;
    value::OwnedValueAccessor inputAccessor2;
    auto inputSlot1 = bindAccessor(&inputAccessor1);
    auto inputSlot2 = bindAccessor(&inputAccessor2);

    auto callExpr =
        makeE<EFunction>("pow", makeEs(makeE<EVariable>(inputSlot1), makeE<EVariable>(inputSlot2)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        // base and exponent positive int32_t and res int32_t

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(64, resultVal);
    }

    {
        // base and exponent positive int32_t and res int64_t

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(85));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt64, resultTag);
        int64_t expected = 85 * 85 * 85 * 85 * 85ll;
        ASSERT_EQ(expected, resultVal);
    }

    {
        // base nagative int32_t, exponent positive int32_t and res int32_t

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-4));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(-4 * -4 * -4, resultVal);
    }

    {
        // base positive int64_t, exponent positive int32_t

        inputAccessor1.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(12125));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt64, resultTag);
        int64_t expected = 12125 * 12125 * static_cast<int64_t>(12125);
        ASSERT_EQ(expected, resultVal);
    }

    {
        // base positive int32_t, exponent positive int64_t

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4));
        inputAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt64, resultTag);
        ASSERT_EQ(4 * 4 * 4, resultVal);
    }

    {
        // base positive int64_t, exponent positive int64_t and res double

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int64_t>(128));
        inputAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(12));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        double expected = 1.934e25;
        ASSERT(std::abs(expected - value::bitcastTo<double>(resultVal)) < 0.001e25);
    }

    {
        // base negative int64_t, exponent positive int64_t

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int64_t>(-4));
        inputAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt64, resultTag);
        ASSERT_EQ(-4 * -4 * -4, resultVal);
    }

    {
        // base a decimal

        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"5.5"});
        inputAccessor1.reset(inputTag, inputVal);
        inputAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDecimal, resultTag);
        auto expected = Decimal128{std::to_string(5.5 * 5.5 * 5.5)};
        ASSERT(expected.subtract(value::bitcastTo<Decimal128>(resultVal))
                   .toAbs()
                   .isLess(Decimal128{"0.001"}));
    }

    {
        // exponent a decimal

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4));
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"5.5"});
        inputAccessor2.reset(inputTag, inputVal);
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDecimal, resultTag);
        auto expected = Decimal128{std::to_string(4 * 4 * 4 * 4 * 4 * 2)};
        ASSERT(expected.subtract(value::bitcastTo<Decimal128>(resultVal))
                   .toAbs()
                   .isLess(Decimal128{"0.001"}));
    }

    {
        // base a double

        inputAccessor1.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.5));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        double expected = 5.5 * 5.5 * 5.5;
        ASSERT_EQ(expected, value::bitcastTo<double>(resultVal));
    }

    {
        // exponent a double

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4));
        inputAccessor2.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.5));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        double expected = 4 * 4 * 4 * 4 * 4 * 2;
        ASSERT_EQ(expected, value::bitcastTo<double>(resultVal));
    }

    {
        // exponent > 63

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(65));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        double expected = 3.68935e19;
        ASSERT(std::abs(expected - value::bitcastTo<double>(resultVal)) < 0.001e19);
    }

    {
        // exponent < 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-3));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_EQ(0.125, value::bitcastTo<double>(resultVal));
    }

    {
        // base = 0, exponent = 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(1, resultVal);
    }
    {
        // base = 0, exponent > 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
        inputAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3000000000));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt64, resultTag);
        ASSERT_EQ(0, resultVal);
    }
    {
        // int/long base = 0, exponent < 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
        inputAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(-120));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::Nothing, resultTag);
    }
    {
        // decimal base = 0, exponent < 0

        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"0.0"});
        inputAccessor1.reset(inputTag, inputVal);
        inputAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(-120));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::Nothing, resultTag);
    }
    {
        // double base = 0, exponent < 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<double>(0.0));
        inputAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(-120));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::Nothing, resultTag);
    }
    {
        // base = 1, exponent = 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(1, resultVal);
    }
    {
        // base = 1, exponent > 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5000));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(1, resultVal);
    }
    {
        // base = 1, exponent < 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-5000));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(1, resultVal);
    }

    {
        // base = -1, exponent = 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(1, resultVal);
    }
    {
        // base = -1, exponent > 0 and exponent%2 = 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1024));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(1, resultVal);
    }
    {
        // base = -1, exponent > 0 and exponent%2 = 1

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1023));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(-1, resultVal);
    }
    {
        // base = -1, exponent < 0 and exponent%2 = 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1024));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(1, resultVal);
    }
    {
        // base = -1, exponent < 0 and exponent%2 = 1

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1023));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(-1, resultVal);
    }

    {
        // base not a number

        auto [inputTag, inputVal] = value::makeNewString("short");
        inputAccessor1.reset(inputTag, inputVal);
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::Nothing, resultTag);
    }

    {
        // exponent not a number

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2));
        auto [inputTag, inputVal] = value::makeNewString("short");
        inputAccessor2.reset(inputTag, inputVal);
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::Nothing, resultTag);
    }

    {
        // base < 0, -1 < exponent < 1

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-5));
        inputAccessor2.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.5));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT(std::isnan(value::bitcastTo<double>(resultVal)));
    }

    {
        // base is nothing

        inputAccessor1.reset();
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::Nothing, resultTag);
    }

    {
        // base in null

        inputAccessor1.reset(value::TypeTags::Null, 0);
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::Nothing, resultTag);
    }

    {
        // base is NaN

        inputAccessor1.reset(value::TypeTags::NumberDouble,
                             value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN()));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT(std::isnan(value::bitcastTo<double>(resultVal)));
    }
}

TEST_F(SBEMathBuiltinTest, InvalidInputsToUnaryNumericFunctions) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    std::vector<std::string> functionNames = {"abs", "ceil", "floor", "exp", "ln", "log10", "sqrt"};
    std::vector<std::unique_ptr<vm::CodeFragment>> compiledExpressionList;
    std::transform(functionNames.begin(),
                   functionNames.end(),
                   std::back_inserter(compiledExpressionList),
                   [&](std::string name) {
                       auto callExpr = makeE<EFunction>(name, makeEs(makeE<EVariable>(inputSlot)));
                       return compileExpression(*callExpr);
                   });

    auto runAllExpressionsExpectingNothing = [&]() {
        for (auto&& compiledExpr : compiledExpressionList) {
            auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
            value::ValueGuard guard(resultTag, resultVal);

            ASSERT_EQ(value::TypeTags::Nothing, resultTag);
        }
    };

    {
        inputAccessor.reset();  // Nothing
        runAllExpressionsExpectingNothing();
    }

    {
        inputAccessor.reset(value::TypeTags::Null, 0);
        runAllExpressionsExpectingNothing();
    }

    {
        auto [inputTag, inputVal] = value::makeNewString("short");
        inputAccessor.reset(inputTag, inputVal);
        runAllExpressionsExpectingNothing();
    }

    {
        auto [inputTag, inputVal] =
            value::makeNewString("a string of by no means insubstantial length");
        inputAccessor.reset(inputTag, inputVal);
        runAllExpressionsExpectingNothing();
    }

    {
        auto testObj = BSON("a" << 1 << "b" << 2);
        auto [inputTag, inputVal] = value::copyValue(
            value::TypeTags::bsonObject, value::bitcastFrom<const char*>(testObj.objdata()));
        inputAccessor.reset(inputTag, inputVal);
        runAllExpressionsExpectingNothing();
    }

    // NaN is a valid input but returns NaN
    {
        inputAccessor.reset(value::TypeTags::NumberDouble,
                            value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN()));

        for (auto&& compiledExpr : compiledExpressionList) {
            auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
            value::ValueGuard guard(resultTag, resultVal);

            ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
            ASSERT(std::isnan(value::bitcastTo<double>(resultVal)));
        }
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128::kPositiveNaN);
        inputAccessor.reset(inputTag, inputVal);

        for (auto&& compiledExpr : compiledExpressionList) {
            auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
            value::ValueGuard guard(resultTag, resultVal);

            ASSERT_EQ(value::TypeTags::NumberDecimal, resultTag);
            ASSERT(value::bitcastTo<Decimal128>(resultVal).isNaN());
        }
    }
}

TEST_F(SBEMathBuiltinTest, DoubleDoubleSummation) {
    {
        value::OwnedValueAccessor inputAccessor;
        auto inputSlot = bindAccessor(&inputAccessor);

        auto callExpr = makeE<EFunction>("doubleDoubleSum", makeEs(makeE<EVariable>(inputSlot)));
        auto compiledExpr = compileExpression(*callExpr);

        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"-1.0"});
        inputAccessor.reset(inputTag, inputVal);
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDecimal, resultTag);
        ASSERT(value::bitcastTo<Decimal128>(resultVal).isEqual(Decimal128{"-1.0"}));
    }

    {
        constexpr auto arity = 3;
        std::vector<Decimal128> vals = {Decimal128("1.0"), Decimal128("2.0"), Decimal128("3.0")};
        EExpression::Vector args;

        for (size_t i = 0; i < arity; ++i) {
            auto [tag, val] = value::makeCopyDecimal(vals[i]);
            args.push_back(makeE<EConstant>(tag, val));
        }

        auto callExpr = makeE<EFunction>("doubleDoubleSum", std::move(args));
        auto compiledExpr = compileExpression(*callExpr);

        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDecimal, resultTag);
        ASSERT(value::bitcastTo<Decimal128>(resultVal).isEqual(Decimal128{"6.0"}));
    }
}
}  // namespace

}  // namespace mongo::sbe
