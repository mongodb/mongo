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

#include "mongo/db/exec/sbe/expression_test_base.h"

namespace mongo::sbe {

class SBETrigonometricExprTest : public EExpressionTestFixture {
protected:
    void runAndAssertExpression(const vm::CodeFragment* compiledExpr, double expectedVal) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);

        ASSERT_EQUALS(value::TypeTags::NumberDouble, tag);
        ASSERT_APPROX_EQUAL(value::bitcastTo<double>(val), expectedVal, 0.000001);
    }

    void runAndAssertExpression(const vm::CodeFragment* compiledExpr, Decimal128 expectedVal) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);

        ASSERT_EQUALS(value::TypeTags::NumberDecimal, tag);
        ASSERT(value::bitcastTo<Decimal128>(val)
                   .subtract(expectedVal)
                   .toAbs()
                   .isLessEqual(Decimal128(".000001")));
    }

    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);
        ASSERT_EQUALS(value::TypeTags::Nothing, tag);
    }
};

TEST_F(SBETrigonometricExprTest, ComputesAcos) {
    value::OwnedValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto acosExpr = sbe::makeE<sbe::EFunction>("acos", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*acosExpr);

    slotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(0));
    runAndAssertExpression(compiledExpr.get(), std::acos(0));

    slotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{1}));
    runAndAssertExpression(compiledExpr.get(), std::acos(1));

    slotAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(0.10001));
    runAndAssertExpression(compiledExpr.get(), std::acos(0.10001));

    Decimal128 arg(0.777);
    auto [tagArg, valArg] = value::makeCopyDecimal(arg);
    slotAccessor.reset(tagArg, valArg);
    runAndAssertExpression(compiledExpr.get(), arg.acos());

    auto [tagStrArg, valStrArg] = value::makeNewString("xyz");
    slotAccessor.reset(tagStrArg, valStrArg);
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBETrigonometricExprTest, ComputesAcosh) {
    value::OwnedValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto acoshExpr = sbe::makeE<sbe::EFunction>("acosh", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*acoshExpr);

    slotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(1));
    runAndAssertExpression(compiledExpr.get(), std::acosh(1));

    slotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{7}));
    runAndAssertExpression(compiledExpr.get(), std::acosh(7));

    slotAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(1.10001));
    runAndAssertExpression(compiledExpr.get(), std::acosh(1.10001));

    Decimal128 arg(7.777);
    auto [tagArg, valArg] = value::makeCopyDecimal(arg);
    slotAccessor.reset(tagArg, valArg);
    runAndAssertExpression(compiledExpr.get(), arg.acosh());

    auto [tagStrArg, valStrArg] = value::makeNewString("xyz");
    slotAccessor.reset(tagStrArg, valStrArg);
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBETrigonometricExprTest, ComputesAsin) {
    value::OwnedValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto asinExpr = sbe::makeE<sbe::EFunction>("asin", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*asinExpr);

    slotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(0));
    runAndAssertExpression(compiledExpr.get(), std::asin(0));

    slotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{1}));
    runAndAssertExpression(compiledExpr.get(), std::asin(1));

    slotAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(0.10001));
    runAndAssertExpression(compiledExpr.get(), std::asin(0.10001));

    Decimal128 arg(0.9);
    auto [tagArg, valArg] = value::makeCopyDecimal(arg);
    slotAccessor.reset(tagArg, valArg);
    runAndAssertExpression(compiledExpr.get(), arg.asin());

    auto [tagStrArg, valStrArg] = value::makeNewString("xyz");
    slotAccessor.reset(tagStrArg, valStrArg);
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBETrigonometricExprTest, ComputesAsinh) {
    value::OwnedValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto asinhExpr = sbe::makeE<sbe::EFunction>("asinh", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*asinhExpr);

    slotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(0));
    runAndAssertExpression(compiledExpr.get(), std::asinh(0));

    slotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{1}));
    runAndAssertExpression(compiledExpr.get(), std::asinh(1));

    slotAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(0.10001));
    runAndAssertExpression(compiledExpr.get(), std::asinh(0.10001));

    Decimal128 arg(1.5);
    auto [tagArg, valArg] = value::makeCopyDecimal(arg);
    slotAccessor.reset(tagArg, valArg);
    runAndAssertExpression(compiledExpr.get(), arg.asinh());

    auto [tagStrArg, valStrArg] = value::makeNewString("xyz");
    slotAccessor.reset(tagStrArg, valStrArg);
    runAndAssertNothing(compiledExpr.get());
}


TEST_F(SBETrigonometricExprTest, ComputesAtan) {
    value::OwnedValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto atanExpr = sbe::makeE<sbe::EFunction>("atan", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*atanExpr);

    slotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(1));
    runAndAssertExpression(compiledExpr.get(), std::atan(1));

    slotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{2}));
    runAndAssertExpression(compiledExpr.get(), std::atan(2));

    slotAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(0.10001));
    runAndAssertExpression(compiledExpr.get(), std::atan(0.10001));

    Decimal128 arg(1.0471975512);
    auto [tagArg, valArg] = value::makeCopyDecimal(arg);
    slotAccessor.reset(tagArg, valArg);
    runAndAssertExpression(compiledExpr.get(), arg.atan());

    auto [tagStrArg, valStrArg] = value::makeNewString("xyz");
    slotAccessor.reset(tagStrArg, valStrArg);
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBETrigonometricExprTest, ComputesAtanh) {
    value::OwnedValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto atanhExpr = sbe::makeE<sbe::EFunction>("atanh", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*atanhExpr);

    slotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(0));
    runAndAssertExpression(compiledExpr.get(), std::atanh(0));

    slotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{0}));
    runAndAssertExpression(compiledExpr.get(), std::atanh(0));

    slotAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(0.10001));
    runAndAssertExpression(compiledExpr.get(), std::atanh(0.10001));

    Decimal128 arg(-0.345);
    auto [tagArg, valArg] = value::makeCopyDecimal(arg);
    slotAccessor.reset(tagArg, valArg);
    runAndAssertExpression(compiledExpr.get(), arg.atanh());

    auto [tagStrArg, valStrArg] = value::makeNewString("xyz");
    slotAccessor.reset(tagStrArg, valStrArg);
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBETrigonometricExprTest, ComputesAtan2) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto argSlot1 = bindAccessor(&slotAccessor1);
    auto argSlot2 = bindAccessor(&slotAccessor2);
    auto atan2Expr = sbe::makeE<sbe::EFunction>(
        "atan2", sbe::makeEs(makeE<EVariable>(argSlot1), makeE<EVariable>(argSlot2)));
    auto compiledExpr = compileExpression(*atan2Expr);

    slotAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom(1));
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom(0));
    runAndAssertExpression(compiledExpr.get(), std::atan2(1, 0));

    slotAccessor1.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{0}));
    slotAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{-1}));
    runAndAssertExpression(compiledExpr.get(), std::atan2(0, -1));

    slotAccessor1.reset(value::TypeTags::NumberDouble, value::bitcastFrom(0.10001));
    slotAccessor2.reset(value::TypeTags::NumberDouble, value::bitcastFrom(0.707106781187));
    runAndAssertExpression(compiledExpr.get(), std::atan2(0.10001, 0.707106781187));

    Decimal128 arg1(0.5), arg2(0.866025403784);
    auto [tagArg1, valArg1] = value::makeCopyDecimal(arg1);
    slotAccessor1.reset(tagArg1, valArg1);
    auto [tagArg2, valArg2] = value::makeCopyDecimal(arg2);
    slotAccessor2.reset(tagArg2, valArg2);
    runAndAssertExpression(compiledExpr.get(), arg1.atan2(arg2));

    auto [tagStrArg, valStrArg] = value::makeNewString("xyz");
    slotAccessor1.reset(tagStrArg, valStrArg);
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom(1));
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBETrigonometricExprTest, ComputesCos) {
    value::OwnedValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto cosExpr = sbe::makeE<sbe::EFunction>("cos", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*cosExpr);

    slotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(1));
    runAndAssertExpression(compiledExpr.get(), std::cos(1));

    slotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{2}));
    runAndAssertExpression(compiledExpr.get(), std::cos(2));

    slotAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(0.10001));
    runAndAssertExpression(compiledExpr.get(), std::cos(0.10001));

    Decimal128 arg(1.000001);
    auto [tagArg, valArg] = value::makeCopyDecimal(arg);
    slotAccessor.reset(tagArg, valArg);
    runAndAssertExpression(compiledExpr.get(), arg.cos());

    auto [tagStrArg, valStrArg] = value::makeNewString("xyz");
    slotAccessor.reset(tagStrArg, valStrArg);
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBETrigonometricExprTest, ComputesCosh) {
    value::OwnedValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto coshExpr = sbe::makeE<sbe::EFunction>("cosh", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*coshExpr);

    slotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(1));
    runAndAssertExpression(compiledExpr.get(), std::cosh(1));

    slotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{2}));
    runAndAssertExpression(compiledExpr.get(), std::cosh(2));

    slotAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(0.10001));
    runAndAssertExpression(compiledExpr.get(), std::cosh(0.10001));

    Decimal128 arg(4.18879020479);
    auto [tagArg, valArg] = value::makeCopyDecimal(arg);
    slotAccessor.reset(tagArg, valArg);
    runAndAssertExpression(compiledExpr.get(), arg.cosh());

    auto [tagStrArg, valStrArg] = value::makeNewString("xyz");
    slotAccessor.reset(tagStrArg, valStrArg);
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBETrigonometricExprTest, ComputesDegreesToRadians) {
    value::OwnedValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto degreesToRadiansExpr =
        sbe::makeE<sbe::EFunction>("degreesToRadians", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*degreesToRadiansExpr);

    slotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(45));
    runAndAssertExpression(compiledExpr.get(), 0.7853981633974483);

    slotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{135}));
    runAndAssertExpression(compiledExpr.get(), 2.356194490192345);

    slotAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(240.0));
    runAndAssertExpression(compiledExpr.get(), 4.1887902047863905);

    Decimal128 arg(180);
    auto [tagArg, valArg] = value::makeCopyDecimal(arg);
    slotAccessor.reset(tagArg, valArg);
    runAndAssertExpression(compiledExpr.get(), Decimal128(3.1415926535897));

    auto [tagStrArg, valStrArg] = value::makeNewString("xyz");
    slotAccessor.reset(tagStrArg, valStrArg);
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBETrigonometricExprTest, ComputesRadiansToDegrees) {
    value::OwnedValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto radiansToDegreesExpr =
        sbe::makeE<sbe::EFunction>("radiansToDegrees", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*radiansToDegreesExpr);

    slotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(1));
    runAndAssertExpression(compiledExpr.get(), 57.29577951308232);

    slotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{3}));
    runAndAssertExpression(compiledExpr.get(), 171.88733853924697);

    slotAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(5.235987755982989));
    runAndAssertExpression(compiledExpr.get(), 300.0);

    Decimal128 arg(3.141592653589793);
    auto [tagArg, valArg] = value::makeCopyDecimal(arg);
    slotAccessor.reset(tagArg, valArg);
    runAndAssertExpression(compiledExpr.get(), Decimal128(180.0));

    auto [tagStrArg, valStrArg] = value::makeNewString("xyz");
    slotAccessor.reset(tagStrArg, valStrArg);
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBETrigonometricExprTest, ComputesSin) {
    value::OwnedValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto sinExpr = sbe::makeE<sbe::EFunction>("sin", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*sinExpr);

    slotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(1));
    runAndAssertExpression(compiledExpr.get(), std::sin(1));

    slotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{2}));
    runAndAssertExpression(compiledExpr.get(), std::sin(2));

    slotAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(0.10001));
    runAndAssertExpression(compiledExpr.get(), std::sin(0.10001));

    Decimal128 arg(1.000001);
    auto [tagArg, valArg] = value::makeCopyDecimal(arg);
    slotAccessor.reset(tagArg, valArg);
    runAndAssertExpression(compiledExpr.get(), arg.sin());

    auto [tagStrArg, valStrArg] = value::makeNewString("xyz");
    slotAccessor.reset(tagStrArg, valStrArg);
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBETrigonometricExprTest, ComputesSinh) {
    value::OwnedValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto sinhExpr = sbe::makeE<sbe::EFunction>("sinh", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*sinhExpr);

    slotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(1));
    runAndAssertExpression(compiledExpr.get(), std::sinh(1));

    slotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{2}));
    runAndAssertExpression(compiledExpr.get(), std::sinh(2));

    slotAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(0.523598775598));
    runAndAssertExpression(compiledExpr.get(), std::sinh(0.523598775598));

    Decimal128 arg(3.66519142919);
    auto [tagArg, valArg] = value::makeCopyDecimal(arg);
    slotAccessor.reset(tagArg, valArg);
    runAndAssertExpression(compiledExpr.get(), arg.sinh());

    auto [tagStrArg, valStrArg] = value::makeNewString("xyz");
    slotAccessor.reset(tagStrArg, valStrArg);
    runAndAssertNothing(compiledExpr.get());
}
TEST_F(SBETrigonometricExprTest, ComputesTan) {
    value::OwnedValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto tanExpr = sbe::makeE<sbe::EFunction>("tan", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*tanExpr);

    slotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(1));
    runAndAssertExpression(compiledExpr.get(), std::tan(1));

    slotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{2}));
    runAndAssertExpression(compiledExpr.get(), std::tan(2));

    slotAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(0.10001));
    runAndAssertExpression(compiledExpr.get(), std::tan(0.10001));

    Decimal128 arg(1.000001);
    auto [tagArg, valArg] = value::makeCopyDecimal(arg);
    slotAccessor.reset(tagArg, valArg);
    runAndAssertExpression(compiledExpr.get(), arg.tan());

    auto [tagStrArg, valStrArg] = value::makeNewString("xyz");
    slotAccessor.reset(tagStrArg, valStrArg);
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBETrigonometricExprTest, ComputesTanh) {
    value::OwnedValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    auto tanhExpr = sbe::makeE<sbe::EFunction>("tanh", sbe::makeEs(makeE<EVariable>(argSlot)));
    auto compiledExpr = compileExpression(*tanhExpr);

    slotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom(1));
    runAndAssertExpression(compiledExpr.get(), std::tanh(1));

    slotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{2}));
    runAndAssertExpression(compiledExpr.get(), std::tanh(2));

    slotAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom(1.0471975512));
    runAndAssertExpression(compiledExpr.get(), std::tanh(1.0471975512));

    Decimal128 arg(0.785398163397);
    auto [tagArg, valArg] = value::makeCopyDecimal(arg);
    slotAccessor.reset(tagArg, valArg);
    runAndAssertExpression(compiledExpr.get(), arg.tanh());

    auto [tagStrArg, valStrArg] = value::makeNewString("xyz");
    slotAccessor.reset(tagStrArg, valStrArg);
    runAndAssertNothing(compiledExpr.get());
}
}  // namespace mongo::sbe
