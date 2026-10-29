// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
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

    auto callExpr = makeE<EFunction>(EFn::kAbs, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-6));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
        ASSERT_EQ(6, value::bitcastTo<int32_t>(result.value()));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt32,
                            value::bitcastFrom<int32_t>(std::numeric_limits<int32_t>::min()));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt64, result.tag());
        ASSERT_EQ(-static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
                  value::bitcastTo<int64_t>(result.value()));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-6000000000));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt64, result.tag());
        ASSERT_EQ(6000000000, value::bitcastTo<int64_t>(result.value()));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64,
                            value::bitcastFrom<int64_t>(std::numeric_limits<int64_t>::min()));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::Nothing, result.tag());
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-6e300));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_APPROX_EQUAL(6e300, value::bitcastTo<double>(result.value()), 1e297);
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"-6e300"});
        inputAccessor.reset(inputTag, inputVal);

        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDecimal, result.tag());
        ASSERT(Decimal128{"6e300"} == value::bitcastTo<Decimal128>(result.value()));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble,
                            value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN()));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_TRUE(std::isnan(value::bitcastTo<double>(result.value())));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-NAN));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_TRUE(std::isnan(value::bitcastTo<double>(result.value())));
    }
}

TEST_F(SBEMathBuiltinTest, Ceil) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto callExpr = makeE<EFunction>(EFn::kCeil, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0001));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_EQ(-10.0, value::bitcastTo<double>(result.value()));
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"-123.456"});
        inputAccessor.reset(inputTag, inputVal);

        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDecimal, result.tag());
        ASSERT(Decimal128{"-123"} == value::bitcastTo<Decimal128>(result.value()));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-10));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
        ASSERT_EQ(-10, value::bitcastTo<int32_t>(result.value()));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64,
                            value::bitcastFrom<int64_t>(-10000000000));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt64, result.tag());
        ASSERT_EQ(-10000000000, value::bitcastTo<int64_t>(result.value()));
    }
}

TEST_F(SBEMathBuiltinTest, Floor) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto callExpr = makeE<EFunction>(EFn::kFloor, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0001));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_EQ(-11.0, value::bitcastTo<double>(result.value()));
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"-123.456"});
        inputAccessor.reset(inputTag, inputVal);

        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDecimal, result.tag());
        ASSERT(Decimal128{"-124"} == value::bitcastTo<Decimal128>(result.value()));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-10));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
        ASSERT_EQ(-10, value::bitcastTo<int32_t>(result.value()));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64,
                            value::bitcastFrom<int64_t>(-10000000000));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt64, result.tag());
        ASSERT_EQ(-10000000000, value::bitcastTo<int64_t>(result.value()));
    }
}

TEST_F(SBEMathBuiltinTest, Exp) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto callExpr = makeE<EFunction>(EFn::kExp, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_APPROX_EQUAL(7.389, value::bitcastTo<double>(result.value()), 0.001);
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_APPROX_EQUAL(20.08, value::bitcastTo<double>(result.value()), 0.01);
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.5));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_APPROX_EQUAL(12.18, value::bitcastTo<double>(result.value()), 0.01);
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"3.5"});
        inputAccessor.reset(inputTag, inputVal);
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDecimal, result.tag());
        auto expected = Decimal128{"33.12"};
        ASSERT(expected.subtract(value::bitcastTo<Decimal128>(result.value()))
                   .toAbs()
                   .isLess(Decimal128{"0.01"}));
    }
}

TEST_F(SBEMathBuiltinTest, Ln) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto callExpr = makeE<EFunction>(EFn::kLn, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_APPROX_EQUAL(0.6931, value::bitcastTo<double>(result.value()), 0.0001);
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(20000000000));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_APPROX_EQUAL(23.72, value::bitcastTo<double>(result.value()), 0.01);
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.1e20));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_APPROX_EQUAL(46.79, value::bitcastTo<double>(result.value()), 0.01);
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"4.2e25"});
        inputAccessor.reset(inputTag, inputVal);
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDecimal, result.tag());
        auto expected = Decimal128{"59.00"};
        ASSERT(expected.subtract(value::bitcastTo<Decimal128>(result.value()))
                   .toAbs()
                   .isLess(Decimal128{"0.01"}));
    }

    // Non-positive values evaluate to Nothing
    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::Nothing, result.tag());
    }
}

TEST_F(SBEMathBuiltinTest, Log10) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto callExpr = makeE<EFunction>(EFn::kLog10, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(100));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_EQ(2.0, value::bitcastTo<double>(result.value()));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10000000000));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_EQ(10.0, value::bitcastTo<double>(result.value()));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.1e20));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_APPROX_EQUAL(20.322, value::bitcastTo<double>(result.value()), 0.01);
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"1e2000"});
        inputAccessor.reset(inputTag, inputVal);
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDecimal, result.tag());
        ASSERT(Decimal128{"2000"} == value::bitcastTo<Decimal128>(result.value()));
    }
}

TEST_F(SBEMathBuiltinTest, Sqrt) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto callExpr = makeE<EFunction>(EFn::kSqrt, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_EQ(2.0, value::bitcastTo<double>(result.value()));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10000000000));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_EQ(100000.0, value::bitcastTo<double>(result.value()));
    }

    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.5));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_APPROX_EQUAL(1.581, value::bitcastTo<double>(result.value()), 0.001);
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"5.2"});
        inputAccessor.reset(inputTag, inputVal);
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDecimal, result.tag());
        auto expected = Decimal128{"2.280"};
        ASSERT(expected.subtract(value::bitcastTo<Decimal128>(result.value()))
                   .toAbs()
                   .isLess(Decimal128{"0.001"}));
    }

    // For types with signed zero, we do not distinguish -0 from 0. The domain of sqrt _does_
    // include -0, and -0 evaluates to a value that is equal to zero.
    {
        inputAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-0.0));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_EQ(0.0, value::bitcastTo<double>(result.value()));
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"-0.0"});
        inputAccessor.reset(inputTag, inputVal);
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDecimal, result.tag());
        ASSERT(value::bitcastTo<Decimal128>(result.value())
                   .normalize()
                   .isEqual(Decimal128::kNormalizedZero));
    }
}

TEST_F(SBEMathBuiltinTest, Pow) {
    value::OwnedValueAccessor inputAccessor1;
    value::OwnedValueAccessor inputAccessor2;
    auto inputSlot1 = bindAccessor(&inputAccessor1);
    auto inputSlot2 = bindAccessor(&inputAccessor2);

    auto callExpr = makeE<EFunction>(
        EFn::kPow, makeEs(makeE<EVariable>(inputSlot1), makeE<EVariable>(inputSlot2)));
    auto compiledExpr = compileExpression(*callExpr);

    {
        // base and exponent positive int32_t and res int32_t

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
        ASSERT_EQ(64, result.value());
    }

    {
        // base and exponent positive int32_t and res int64_t

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(85));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt64, result.tag());
        int64_t expected = 85 * 85 * 85 * 85 * 85ll;
        ASSERT_EQ(expected, result.value());
    }

    {
        // base nagative int32_t, exponent positive int32_t and res int32_t

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-4));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
        ASSERT_EQ(-4 * -4 * -4, result.value());
    }

    {
        // base positive int64_t, exponent positive int32_t

        inputAccessor1.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(12125));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt64, result.tag());
        int64_t expected = 12125 * 12125 * static_cast<int64_t>(12125);
        ASSERT_EQ(expected, result.value());
    }

    {
        // base positive int32_t, exponent positive int64_t

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4));
        inputAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt64, result.tag());
        ASSERT_EQ(4 * 4 * 4, result.value());
    }

    {
        // base positive int64_t, exponent positive int64_t and res double

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int64_t>(128));
        inputAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(12));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        double expected = 1.934e25;
        ASSERT(std::abs(expected - value::bitcastTo<double>(result.value())) < 0.001e25);
    }

    {
        // base negative int64_t, exponent positive int64_t

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int64_t>(-4));
        inputAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt64, result.tag());
        ASSERT_EQ(-4 * -4 * -4, result.value());
    }

    {
        // base a decimal

        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"5.5"});
        inputAccessor1.reset(inputTag, inputVal);
        inputAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDecimal, result.tag());
        auto expected = Decimal128{std::to_string(5.5 * 5.5 * 5.5)};
        ASSERT(expected.subtract(value::bitcastTo<Decimal128>(result.value()))
                   .toAbs()
                   .isLess(Decimal128{"0.001"}));
    }

    {
        // exponent a decimal

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4));
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"5.5"});
        inputAccessor2.reset(inputTag, inputVal);
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDecimal, result.tag());
        auto expected = Decimal128{std::to_string(4 * 4 * 4 * 4 * 4 * 2)};
        ASSERT(expected.subtract(value::bitcastTo<Decimal128>(result.value()))
                   .toAbs()
                   .isLess(Decimal128{"0.001"}));
    }

    {
        // base a double

        inputAccessor1.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.5));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        double expected = 5.5 * 5.5 * 5.5;
        ASSERT_EQ(expected, value::bitcastTo<double>(result.value()));
    }

    {
        // exponent a double

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4));
        inputAccessor2.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.5));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        double expected = 4 * 4 * 4 * 4 * 4 * 2;
        ASSERT_EQ(expected, value::bitcastTo<double>(result.value()));
    }

    {
        // exponent > 63

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(65));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        double expected = 3.68935e19;
        ASSERT(std::abs(expected - value::bitcastTo<double>(result.value())) < 0.001e19);
    }

    {
        // exponent < 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-3));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT_EQ(0.125, value::bitcastTo<double>(result.value()));
    }

    {
        // base = 0, exponent = 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
        ASSERT_EQ(1, result.value());
    }
    {
        // base = 0, exponent > 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
        inputAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3000000000));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt64, result.tag());
        ASSERT_EQ(0, result.value());
    }
    {
        // int/long base = 0, exponent < 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
        inputAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(-120));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::Nothing, result.tag());
    }
    {
        // decimal base = 0, exponent < 0

        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"0.0"});
        inputAccessor1.reset(inputTag, inputVal);
        inputAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(-120));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::Nothing, result.tag());
    }
    {
        // double base = 0, exponent < 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<double>(0.0));
        inputAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(-120));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::Nothing, result.tag());
    }
    {
        // base = 1, exponent = 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
        ASSERT_EQ(1, result.value());
    }
    {
        // base = 1, exponent > 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5000));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
        ASSERT_EQ(1, result.value());
    }
    {
        // base = 1, exponent < 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-5000));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
        ASSERT_EQ(1, result.value());
    }

    {
        // base = -1, exponent = 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
        ASSERT_EQ(1, result.value());
    }
    {
        // base = -1, exponent > 0 and exponent%2 = 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1024));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
        ASSERT_EQ(1, result.value());
    }
    {
        // base = -1, exponent > 0 and exponent%2 = 1

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1023));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
        ASSERT_EQ(-1, result.value());
    }
    {
        // base = -1, exponent < 0 and exponent%2 = 0

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1024));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
        ASSERT_EQ(1, result.value());
    }
    {
        // base = -1, exponent < 0 and exponent%2 = 1

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1023));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
        ASSERT_EQ(-1, result.value());
    }

    {
        // base not a number

        auto [inputTag, inputVal] = value::makeNewString("short");
        inputAccessor1.reset(inputTag, inputVal);
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::Nothing, result.tag());
    }

    {
        // exponent not a number

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2));
        auto [inputTag, inputVal] = value::makeNewString("short");
        inputAccessor2.reset(inputTag, inputVal);
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::Nothing, result.tag());
    }

    {
        // base < 0, -1 < exponent < 1

        inputAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-5));
        inputAccessor2.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.5));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT(std::isnan(value::bitcastTo<double>(result.value())));
    }

    {
        // base is nothing

        inputAccessor1.reset();
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::Nothing, result.tag());
    }

    {
        // base in null

        inputAccessor1.reset(value::TypeTags::Null, 0);
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::Nothing, result.tag());
    }

    {
        // base is NaN

        inputAccessor1.reset(value::TypeTags::NumberDouble,
                             value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN()));
        inputAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5));
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
        ASSERT(std::isnan(value::bitcastTo<double>(result.value())));
    }
}

TEST_F(SBEMathBuiltinTest, InvalidInputsToUnaryNumericFunctions) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    std::vector<EFn> functionNames = {
        EFn::kAbs, EFn::kCeil, EFn::kFloor, EFn::kExp, EFn::kLn, EFn::kLog10, EFn::kSqrt};
    std::vector<std::unique_ptr<vm::CodeFragment>> compiledExpressionList;
    std::transform(functionNames.begin(),
                   functionNames.end(),
                   std::back_inserter(compiledExpressionList),
                   [&](EFn name) {
                       auto callExpr = makeE<EFunction>(name, makeEs(makeE<EVariable>(inputSlot)));
                       return compileExpression(*callExpr);
                   });

    auto runAllExpressionsExpectingNothing = [&]() {
        for (auto&& compiledExpr : compiledExpressionList) {
            value::TagValueOwned result =
                value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

            ASSERT_EQ(value::TypeTags::Nothing, result.tag());
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
            value::TagValueOwned result =
                value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

            ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
            ASSERT(std::isnan(value::bitcastTo<double>(result.value())));
        }
    }

    {
        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128::kPositiveNaN);
        inputAccessor.reset(inputTag, inputVal);

        for (auto&& compiledExpr : compiledExpressionList) {
            value::TagValueOwned result =
                value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

            ASSERT_EQ(value::TypeTags::NumberDecimal, result.tag());
            ASSERT(value::bitcastTo<Decimal128>(result.value()).isNaN());
        }
    }
}

TEST_F(SBEMathBuiltinTest, DoubleDoubleSummation) {
    {
        value::OwnedValueAccessor inputAccessor;
        auto inputSlot = bindAccessor(&inputAccessor);

        auto callExpr =
            makeE<EFunction>(EFn::kDoubleDoubleSum, makeEs(makeE<EVariable>(inputSlot)));
        auto compiledExpr = compileExpression(*callExpr);

        auto [inputTag, inputVal] = value::makeCopyDecimal(Decimal128{"-1.0"});
        inputAccessor.reset(inputTag, inputVal);
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDecimal, result.tag());
        ASSERT(value::bitcastTo<Decimal128>(result.value()).isEqual(Decimal128{"-1.0"}));
    }

    {
        constexpr auto arity = 3;
        std::vector<Decimal128> vals = {Decimal128("1.0"), Decimal128("2.0"), Decimal128("3.0")};
        EExpression::Vector args;

        for (size_t i = 0; i < arity; ++i) {
            auto [tag, val] = value::makeCopyDecimal(vals[i]);
            args.push_back(makeE<EConstant>(tag, val));
        }

        auto callExpr = makeE<EFunction>(EFn::kDoubleDoubleSum, std::move(args));
        auto compiledExpr = compileExpression(*callExpr);

        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        ASSERT_EQ(value::TypeTags::NumberDecimal, result.tag());
        ASSERT(value::bitcastTo<Decimal128>(result.value()).isEqual(Decimal128{"6.0"}));
    }
}

TEST_F(SBEMathBuiltinTest, DoubleDoubleSumFromAccSumsArrayElementsIgnoringNonNumeric) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr =
        makeE<EFunction>(EFn::kDoubleDoubleSumFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    auto [arrTag, arrVal] = value::makeNewArray();
    auto* arr = value::getArrayView(arrVal);
    arr->push_back_raw(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    arr->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));
    arr->push_back_raw(value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.5));
    arr->push_back_raw(value::makeNewString("not a number"));
    arr->push_back_raw(value::TypeTags::Null, 0);
    inputAccessor.reset(arrTag, arrVal);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
    ASSERT_EQ(6.5, value::bitcastTo<double>(result.value()));
}

TEST_F(SBEMathBuiltinTest, DoubleDoubleSumFromAccEmptyArrayYieldsInt32Zero) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr =
        makeE<EFunction>(EFn::kDoubleDoubleSumFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    auto [arrTag, arrVal] = value::makeNewArray();
    inputAccessor.reset(arrTag, arrVal);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
    ASSERT_EQ(0, value::bitcastTo<int32_t>(result.value()));
}

TEST_F(SBEMathBuiltinTest, DoubleDoubleSumFromAccSingleNumericArgumentSumsToItself) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr =
        makeE<EFunction>(EFn::kDoubleDoubleSumFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(42));

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
    ASSERT_EQ(42, value::bitcastTo<int32_t>(result.value()));
}

TEST_F(SBEMathBuiltinTest, DoubleDoubleSumFromAccSingleNonNumericArgumentYieldsInt32Zero) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr =
        makeE<EFunction>(EFn::kDoubleDoubleSumFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    auto [strTag, strVal] = value::makeNewString("not a number");
    inputAccessor.reset(strTag, strVal);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
    ASSERT_EQ(0, value::bitcastTo<int32_t>(result.value()));
}

TEST_F(SBEMathBuiltinTest, DoubleDoubleSumFromAccInt32OverflowWidensToInt64) {
    EExpression::Vector args;
    args.push_back(
        makeE<EConstant>(value::TypeTags::NumberInt32,
                         value::bitcastFrom<int32_t>(std::numeric_limits<int32_t>::max())));
    args.push_back(makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)));

    auto callExpr = makeE<EFunction>(EFn::kDoubleDoubleSumFromAcc, std::move(args));
    auto compiledExpr = compileExpression(*callExpr);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberInt64, result.tag());
    ASSERT_EQ(static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1,
              value::bitcastTo<int64_t>(result.value()));
}

TEST_F(SBEMathBuiltinTest, DoubleDoubleSumFromAccMultipleArgumentsIgnoreNonNumericAndArrays) {
    // Multiple arguments are summed directly, ignoring non-numeric ones. Note that unlike the
    // single-argument case, an array argument is not expanded and is simply ignored.
    EExpression::Vector args;
    args.push_back(makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)));
    auto [arrTag, arrVal] = value::makeNewArray();
    value::getArrayView(arrVal)->push_back_raw(value::TypeTags::NumberInt32,
                                               value::bitcastFrom<int32_t>(100));
    args.push_back(makeE<EConstant>(arrTag, arrVal));
    auto [strTag, strVal] = value::makeNewString("not a number");
    args.push_back(makeE<EConstant>(strTag, strVal));
    args.push_back(makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)));

    auto callExpr = makeE<EFunction>(EFn::kDoubleDoubleSumFromAcc, std::move(args));
    auto compiledExpr = compileExpression(*callExpr);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
    ASSERT_EQ(3, value::bitcastTo<int32_t>(result.value()));
}

TEST_F(SBEMathBuiltinTest, DoubleDoubleSumFromAccNoArgumentsYieldsInt32Zero) {
    auto callExpr = makeE<EFunction>(EFn::kDoubleDoubleSumFromAcc, makeEs());
    auto compiledExpr = compileExpression(*callExpr);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
    ASSERT_EQ(0, value::bitcastTo<int32_t>(result.value()));
}

TEST_F(SBEMathBuiltinTest, DoubleDoubleSumFromAccDecimalArgumentWidensToDecimal) {
    EExpression::Vector args;
    args.push_back(makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)));
    auto [decTag, decVal] = value::makeCopyDecimal(Decimal128{"2.5"});
    args.push_back(makeE<EConstant>(decTag, decVal));

    auto callExpr = makeE<EFunction>(EFn::kDoubleDoubleSumFromAcc, std::move(args));
    auto compiledExpr = compileExpression(*callExpr);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberDecimal, result.tag());
    ASSERT(value::bitcastTo<Decimal128>(result.value()).isEqual(Decimal128{"3.5"}));
}

TEST_F(SBEMathBuiltinTest, AvgFromAccAveragesArrayElementsIgnoringNonNumeric) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kAvgFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    auto [arrTag, arrVal] = value::makeNewArray();
    auto* arr = value::getArrayView(arrVal);
    arr->push_back_raw(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    arr->push_back_raw(value::makeNewString("not a number"));
    arr->push_back_raw(value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.5));
    arr->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(7));
    arr->push_back_raw(value::TypeTags::Null, 0);
    inputAccessor.reset(arrTag, arrVal);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
    ASSERT_EQ(3.5, value::bitcastTo<double>(result.value()));
}

TEST_F(SBEMathBuiltinTest, AvgFromAccEmptyArrayYieldsNull) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kAvgFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    auto [arrTag, arrVal] = value::makeNewArray();
    inputAccessor.reset(arrTag, arrVal);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::Null, result.tag());
}

TEST_F(SBEMathBuiltinTest, AvgFromAccSingleNullYieldsNull) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kAvgFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    inputAccessor.reset(value::TypeTags::Null, 0);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::Null, result.tag());
}

TEST_F(SBEMathBuiltinTest, AvgFromAccSingleNonNumericYieldsNull) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kAvgFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    auto [strTag, strVal] = value::makeNewString("not a number");
    inputAccessor.reset(strTag, strVal);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::Null, result.tag());
}

TEST_F(SBEMathBuiltinTest, AvgFromAccSingleScalarYieldsItselfAsDouble) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kAvgFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5));

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
    ASSERT_EQ(5.0, value::bitcastTo<double>(result.value()));
}

TEST_F(SBEMathBuiltinTest, AvgFromAccMultipleIntArgumentsAverageToDouble) {
    EExpression::Vector args;
    args.push_back(makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)));
    args.push_back(makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)));

    auto callExpr = makeE<EFunction>(EFn::kAvgFromAcc, std::move(args));
    auto compiledExpr = compileExpression(*callExpr);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
    ASSERT_EQ(1.5, value::bitcastTo<double>(result.value()));
}

TEST_F(SBEMathBuiltinTest, AvgFromAccDecimalArgumentWidensResultToDecimal) {
    EExpression::Vector args;
    args.push_back(makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)));
    auto [decTag, decVal] = value::makeCopyDecimal(Decimal128{"2.5"});
    args.push_back(makeE<EConstant>(decTag, decVal));

    auto callExpr = makeE<EFunction>(EFn::kAvgFromAcc, std::move(args));
    auto compiledExpr = compileExpression(*callExpr);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberDecimal, result.tag());
    ASSERT(value::bitcastTo<Decimal128>(result.value()).isEqual(Decimal128{"1.75"}));
}

TEST_F(SBEMathBuiltinTest, StdDevPopFromAccArrayElementsIgnoringNonNumeric) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kStdDevPopFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    // Population standard deviation of {2, 4, 4, 4, 5, 5, 7, 9} is exactly 2.0.
    auto [arrTag, arrVal] = value::makeNewArray();
    auto* arr = value::getArrayView(arrVal);
    for (auto v : {2, 4, 4, 4, 5, 5, 7, 9}) {
        arr->push_back_raw(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(v));
    }
    arr->push_back_raw(value::makeNewString("not a number"));
    arr->push_back_raw(value::TypeTags::Null, 0);
    inputAccessor.reset(arrTag, arrVal);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
    ASSERT_EQ(2.0, value::bitcastTo<double>(result.value()));
}

TEST_F(SBEMathBuiltinTest, StdDevPopFromAccEmptyArrayYieldsNull) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kStdDevPopFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    auto [arrTag, arrVal] = value::makeNewArray();
    inputAccessor.reset(arrTag, arrVal);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::Null, result.tag());
}

TEST_F(SBEMathBuiltinTest, StdDevPopFromAccSingleNullYieldsNull) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kStdDevPopFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    inputAccessor.reset(value::TypeTags::Null, 0);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::Null, result.tag());
}

TEST_F(SBEMathBuiltinTest, StdDevPopFromAccSingleScalarYieldsZero) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kStdDevPopFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(42));

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
    ASSERT_EQ(0.0, value::bitcastTo<double>(result.value()));
}

TEST_F(SBEMathBuiltinTest, StdDevPopFromAccMultipleArgumentsIgnoreNonNumeric) {
    // Population standard deviation of {2, 4} is exactly 1.0.
    EExpression::Vector args;
    args.push_back(makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)));
    auto [strTag, strVal] = value::makeNewString("not a number");
    args.push_back(makeE<EConstant>(strTag, strVal));
    args.push_back(makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4)));

    auto callExpr = makeE<EFunction>(EFn::kStdDevPopFromAcc, std::move(args));
    auto compiledExpr = compileExpression(*callExpr);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
    ASSERT_EQ(1.0, value::bitcastTo<double>(result.value()));
}

TEST_F(SBEMathBuiltinTest, StdDevPopFromAccDecimalInputsAreConvertedToDouble) {
    // $stdDevPop does not maintain decimal precision; the result is always a double.
    EExpression::Vector args;
    auto [dec1Tag, dec1Val] = value::makeCopyDecimal(Decimal128{"2.0"});
    args.push_back(makeE<EConstant>(dec1Tag, dec1Val));
    auto [dec2Tag, dec2Val] = value::makeCopyDecimal(Decimal128{"4.0"});
    args.push_back(makeE<EConstant>(dec2Tag, dec2Val));

    auto callExpr = makeE<EFunction>(EFn::kStdDevPopFromAcc, std::move(args));
    auto compiledExpr = compileExpression(*callExpr);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
    ASSERT_EQ(1.0, value::bitcastTo<double>(result.value()));
}

TEST_F(SBEMathBuiltinTest, StdDevSampFromAccArrayElementsIgnoringNonNumeric) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kStdDevSampFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    // Sample standard deviation of {2, 4, 6} is exactly 2.0 (the population one would be
    // sqrt(8/3) ~= 1.63, so this also catches a mixup between the two).
    auto [arrTag, arrVal] = value::makeNewArray();
    auto* arr = value::getArrayView(arrVal);
    for (auto v : {2, 4, 6}) {
        arr->push_back_raw(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(v));
    }
    arr->push_back_raw(value::makeNewString("not a number"));
    arr->push_back_raw(value::TypeTags::Null, 0);
    inputAccessor.reset(arrTag, arrVal);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
    ASSERT_EQ(2.0, value::bitcastTo<double>(result.value()));
}

TEST_F(SBEMathBuiltinTest, StdDevSampFromAccEmptyArrayYieldsNull) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kStdDevSampFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    auto [arrTag, arrVal] = value::makeNewArray();
    inputAccessor.reset(arrTag, arrVal);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::Null, result.tag());
}

TEST_F(SBEMathBuiltinTest, StdDevSampFromAccSingleScalarYieldsNull) {
    // Unlike $stdDevPop (which yields 0), the sample standard deviation of a single value is not
    // defined, so $stdDevSamp yields null.
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kStdDevSampFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(42));

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::Null, result.tag());
}

TEST_F(SBEMathBuiltinTest, StdDevSampFromAccMultipleArgumentsIgnoreNonNumeric) {
    EExpression::Vector args;
    args.push_back(makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)));
    auto [strTag, strVal] = value::makeNewString("not a number");
    args.push_back(makeE<EConstant>(strTag, strVal));
    args.push_back(makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4)));
    args.push_back(makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(6)));

    auto callExpr = makeE<EFunction>(EFn::kStdDevSampFromAcc, std::move(args));
    auto compiledExpr = compileExpression(*callExpr);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
    ASSERT_EQ(2.0, value::bitcastTo<double>(result.value()));
}

TEST_F(SBEMathBuiltinTest, MinFromAccArrayElementsIgnoringNullish) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kMinFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    auto [arrTag, arrVal] = value::makeNewArray();
    auto* arr = value::getArrayView(arrVal);
    arr->push_back_raw(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5));
    arr->push_back_raw(value::TypeTags::Null, 0);
    arr->push_back_raw(value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.5));
    arr->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(7));
    inputAccessor.reset(arrTag, arrVal);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
    ASSERT_EQ(2.5, value::bitcastTo<double>(result.value()));
}

TEST_F(SBEMathBuiltinTest, MinFromAccEmptyArrayYieldsNull) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kMinFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    auto [arrTag, arrVal] = value::makeNewArray();
    inputAccessor.reset(arrTag, arrVal);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::Null, result.tag());
}

TEST_F(SBEMathBuiltinTest, MinFromAccSingleNullYieldsNull) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kMinFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    inputAccessor.reset(value::TypeTags::Null, 0);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::Null, result.tag());
}

TEST_F(SBEMathBuiltinTest, MinFromAccSingleScalarYieldsItself) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kMinFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(42));

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
    ASSERT_EQ(42, value::bitcastTo<int32_t>(result.value()));
}

TEST_F(SBEMathBuiltinTest, MinFromAccMultipleArgumentsCompareDirectly) {
    // Multiple arguments are compared directly, with nullish ones ignored. Strings compare using
    // the BSON sort order.
    EExpression::Vector args;
    auto [str1Tag, str1Val] = value::makeNewString("banana");
    args.push_back(makeE<EConstant>(str1Tag, str1Val));
    args.push_back(makeE<EConstant>(value::TypeTags::Null, 0));
    auto [str2Tag, str2Val] = value::makeNewString("apple");
    args.push_back(makeE<EConstant>(str2Tag, str2Val));

    auto callExpr = makeE<EFunction>(EFn::kMinFromAcc, std::move(args));
    auto compiledExpr = compileExpression(*callExpr);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT(value::isString(result.tag()));
    ASSERT_EQ("apple", value::getStringView(result.tag(), result.value()));
}

TEST_F(SBEMathBuiltinTest, MinFromAccWithCollatorUsesCollation) {
    // With a collator as the first argument, string comparison uses the collation.
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    value::ViewOfValueAccessor collatorAccessor;
    auto collatorSlot = bindAccessor(&collatorAccessor);
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterface*>(&collator));

    EExpression::Vector args;
    args.push_back(makeE<EVariable>(collatorSlot));
    auto [str1Tag, str1Val] = value::makeNewString("az");
    args.push_back(makeE<EConstant>(str1Tag, str1Val));
    auto [str2Tag, str2Val] = value::makeNewString("by");
    args.push_back(makeE<EConstant>(str2Tag, str2Val));

    auto callExpr = makeE<EFunction>(EFn::kMinFromAcc, std::move(args));
    auto compiledExpr = compileExpression(*callExpr);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT(value::isString(result.tag()));
    ASSERT_EQ("by", value::getStringView(result.tag(), result.value()));
}

TEST_F(SBEMathBuiltinTest, MaxFromAccArrayElementsIgnoringNullish) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kMaxFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    auto [arrTag, arrVal] = value::makeNewArray();
    auto* arr = value::getArrayView(arrVal);
    arr->push_back_raw(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5));
    arr->push_back_raw(value::TypeTags::Null, 0);
    arr->push_back_raw(value::TypeTags::NumberDouble, value::bitcastFrom<double>(7.5));
    arr->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));
    inputAccessor.reset(arrTag, arrVal);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberDouble, result.tag());
    ASSERT_EQ(7.5, value::bitcastTo<double>(result.value()));
}

TEST_F(SBEMathBuiltinTest, MaxFromAccEmptyArrayYieldsNull) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kMaxFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    auto [arrTag, arrVal] = value::makeNewArray();
    inputAccessor.reset(arrTag, arrVal);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::Null, result.tag());
}

TEST_F(SBEMathBuiltinTest, MaxFromAccSingleNullYieldsNull) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kMaxFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    inputAccessor.reset(value::TypeTags::Null, 0);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::Null, result.tag());
}

TEST_F(SBEMathBuiltinTest, MaxFromAccSingleScalarYieldsItself) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto callExpr = makeE<EFunction>(EFn::kMaxFromAcc, makeEs(makeE<EVariable>(inputSlot)));
    auto compiledExpr = compileExpression(*callExpr);

    inputAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(42));

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT_EQ(value::TypeTags::NumberInt32, result.tag());
    ASSERT_EQ(42, value::bitcastTo<int32_t>(result.value()));
}

TEST_F(SBEMathBuiltinTest, MaxFromAccMultipleArgumentsCompareDirectly) {
    // Multiple arguments are compared directly, with nullish ones ignored. Strings compare using
    // the BSON sort order.
    EExpression::Vector args;
    auto [str1Tag, str1Val] = value::makeNewString("banana");
    args.push_back(makeE<EConstant>(str1Tag, str1Val));
    args.push_back(makeE<EConstant>(value::TypeTags::Null, 0));
    auto [str2Tag, str2Val] = value::makeNewString("apple");
    args.push_back(makeE<EConstant>(str2Tag, str2Val));

    auto callExpr = makeE<EFunction>(EFn::kMaxFromAcc, std::move(args));
    auto compiledExpr = compileExpression(*callExpr);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT(value::isString(result.tag()));
    ASSERT_EQ("banana", value::getStringView(result.tag(), result.value()));
}

TEST_F(SBEMathBuiltinTest, MaxFromAccWithCollatorUsesCollation) {
    // With a collator as the first argument, string comparison uses the collation.
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    value::ViewOfValueAccessor collatorAccessor;
    auto collatorSlot = bindAccessor(&collatorAccessor);
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterface*>(&collator));

    EExpression::Vector args;
    args.push_back(makeE<EVariable>(collatorSlot));
    auto [str1Tag, str1Val] = value::makeNewString("az");
    args.push_back(makeE<EConstant>(str1Tag, str1Val));
    auto [str2Tag, str2Val] = value::makeNewString("by");
    args.push_back(makeE<EConstant>(str2Tag, str2Val));

    auto callExpr = makeE<EFunction>(EFn::kMaxFromAcc, std::move(args));
    auto compiledExpr = compileExpression(*callExpr);

    value::TagValueOwned result =
        value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

    ASSERT(value::isString(result.tag()));
    ASSERT_EQ("az", value::getStringView(result.tag(), result.value()));
}
}  // namespace

}  // namespace mongo::sbe
