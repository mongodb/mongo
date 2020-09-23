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

#include <queue>

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::sbe {

namespace {
using SBEMathBuiltinTest = EExpressionTestFixture;

TEST_F(SBEMathBuiltinTest, Abs) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto callExpr = makeE<EFunction>("abs", makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(int32_t{-6}));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(6, value::bitcastTo<int32_t>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt32,
                            value::bitcastFrom(std::numeric_limits<int32_t>::min()));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt64, resultTag);
        ASSERT_EQ(-static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
                  value::bitcastTo<int64_t>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{-6000000000}));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt64, resultTag);
        ASSERT_EQ(6000000000, value::bitcastTo<int64_t>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64,
                            value::bitcastFrom(std::numeric_limits<int64_t>::min()));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::Nothing, resultTag);
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(-6e300));
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
}

TEST_F(SBEMathBuiltinTest, Ceil) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto callExpr = makeE<EFunction>("ceil", makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(double{-10.0001}));
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
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(int32_t{-10}));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(-10, value::bitcastTo<int32_t>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64,
                            value::bitcastFrom(int64_t{-10000000000}));
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
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(double{-10.0001}));
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
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(int32_t{-10}));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberInt32, resultTag);
        ASSERT_EQ(-10, value::bitcastTo<int32_t>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64,
                            value::bitcastFrom(int64_t{-10000000000}));
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
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(int32_t{2}));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_APPROX_EQUAL(7.389, value::bitcastTo<double>(resultVal), 0.001);
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{3}));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_APPROX_EQUAL(20.08, value::bitcastTo<double>(resultVal), 0.01);
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(double{2.5}));
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
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(int32_t{2}));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_APPROX_EQUAL(0.6931, value::bitcastTo<double>(resultVal), 0.0001);
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{20000000000}));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_APPROX_EQUAL(23.72, value::bitcastTo<double>(resultVal), 0.01);
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(double{2.1e20}));
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
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(int32_t{0}));
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
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(int32_t{100}));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_EQ(2.0, value::bitcastTo<double>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{10000000000}));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_EQ(10.0, value::bitcastTo<double>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(double{2.1e20}));
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
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(int32_t{4}));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_EQ(2.0, value::bitcastTo<double>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{10000000000}));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(resultTag, resultVal);

        ASSERT_EQ(value::TypeTags::NumberDouble, resultTag);
        ASSERT_EQ(100000.0, value::bitcastTo<double>(resultVal));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(double{2.5}));
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
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(double{-0.0}));
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
                            value::bitcastFrom(std::numeric_limits<double>::quiet_NaN()));

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

}  // namespace

}  // namespace mongo::sbe
