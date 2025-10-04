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
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cmath>
#include <cstdint>
#include <memory>

namespace mongo::sbe {

class SBEModExprTest : public EExpressionTestFixture {
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

    void runAndAssertExpression(const vm::CodeFragment* compiledExpr, int32_t expectedVal) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);
        ASSERT_EQUALS(value::TypeTags::NumberInt32, tag);
        ASSERT_EQUALS(value::bitcastTo<int32_t>(val), expectedVal);
    }

    void runAndAssertExpression(const vm::CodeFragment* compiledExpr, int64_t expectedVal) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);
        ASSERT_EQUALS(value::TypeTags::NumberInt64, tag);
        ASSERT_EQUALS(value::bitcastTo<int64_t>(val), expectedVal);
    }

    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);
        ASSERT_EQUALS(value::TypeTags::Nothing, tag);
    }

    void runAndAssertThrows(const vm::CodeFragment* compiledExpr) {
        ASSERT_THROWS_CODE(runCompiledExpression(compiledExpr), AssertionException, 4848403);
    }
};

TEST_F(SBEModExprTest, ComputesMod) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto argSlot1 = bindAccessor(&slotAccessor1);
    auto argSlot2 = bindAccessor(&slotAccessor2);
    auto modExpr = sbe::makeE<sbe::EFunction>(
        "mod", sbe::makeEs(makeE<EVariable>(argSlot1), makeE<EVariable>(argSlot2)));
    auto compiledExpr = compileExpression(*modExpr);

    const int32_t i32Val = 16;
    const int32_t i32Mod = 4;
    const int64_t i64Val = 2147483648;
    const int64_t i64Mod = 2147483649;
    const Decimal128 decVal(123.4);
    const Decimal128 decMod(43.2);
    const double dblVal(9.9);
    const double dblMod(2.3);

    slotAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(i32Val));
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(i32Mod));
    runAndAssertExpression(compiledExpr.get(), i32Val % i32Mod);

    slotAccessor1.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(i64Val));
    slotAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(i64Mod));
    runAndAssertExpression(compiledExpr.get(), i64Val % i64Mod);

    auto [tagArgDecVal, valArgDecVal] = value::makeCopyDecimal(decVal);
    slotAccessor1.reset(tagArgDecVal, valArgDecVal);
    auto [tagArgDecMod, valArgDecMod] = value::makeCopyDecimal(decMod);
    slotAccessor2.reset(tagArgDecMod, valArgDecMod);
    runAndAssertExpression(compiledExpr.get(), decVal.modulo(decMod));

    slotAccessor1.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(dblVal));
    slotAccessor2.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(dblMod));
    runAndAssertExpression(compiledExpr.get(), std::fmod(dblVal, dblMod));
}

TEST_F(SBEModExprTest, ComputesModDifferentWidths) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto argSlot1 = bindAccessor(&slotAccessor1);
    auto argSlot2 = bindAccessor(&slotAccessor2);
    auto modExpr = sbe::makeE<sbe::EFunction>(
        "mod", sbe::makeEs(makeE<EVariable>(argSlot1), makeE<EVariable>(argSlot2)));
    auto compiledExpr = compileExpression(*modExpr);

    const int32_t i32Val = 16;
    const int32_t i32Mod = 4;
    const int64_t i64Val = 2147483648;
    const int64_t i64Mod = 2147483649;

    slotAccessor1.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(i64Val));
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(i32Mod));
    runAndAssertExpression(compiledExpr.get(), i64Val % i32Mod);

    slotAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(i32Val));
    slotAccessor2.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(i64Mod));
    runAndAssertExpression(compiledExpr.get(), i32Val % i64Mod);
}

TEST_F(SBEModExprTest, ComputesNothingIfNotNumeric) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto argSlot1 = bindAccessor(&slotAccessor1);
    auto argSlot2 = bindAccessor(&slotAccessor2);
    auto modExpr = sbe::makeE<sbe::EFunction>(
        "mod", sbe::makeEs(makeE<EVariable>(argSlot1), makeE<EVariable>(argSlot2)));
    auto compiledExpr = compileExpression(*modExpr);

    const int32_t i32Val = 16;
    const int32_t i32Mod = 4;

    auto [tagStrArg1, valStrArg1] = value::makeNewString("abc");
    slotAccessor1.reset(tagStrArg1, valStrArg1);
    auto [tagStrArg2, valStrArg2] = value::makeNewString("xyz");
    slotAccessor2.reset(tagStrArg2, valStrArg2);
    runAndAssertNothing(compiledExpr.get());

    slotAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(i32Val));
    slotAccessor2.reset(tagStrArg2, valStrArg2);
    runAndAssertNothing(compiledExpr.get());

    slotAccessor1.reset(tagStrArg1, valStrArg1);
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(i32Mod));
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBEModExprTest, ComputesNothingIfNullOrMissing) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto argSlot1 = bindAccessor(&slotAccessor1);
    auto argSlot2 = bindAccessor(&slotAccessor2);
    auto modExpr = sbe::makeE<sbe::EFunction>(
        "mod", sbe::makeEs(makeE<EVariable>(argSlot1), makeE<EVariable>(argSlot2)));
    auto compiledExpr = compileExpression(*modExpr);

    const int32_t i32Val = 16;
    const int32_t i32Mod = 4;

    slotAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(i32Val));
    slotAccessor2.reset(value::TypeTags::Nothing, 0);
    runAndAssertNothing(compiledExpr.get());

    slotAccessor1.reset(value::TypeTags::Nothing, 0);
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(i32Mod));
    runAndAssertNothing(compiledExpr.get());

    slotAccessor1.reset(value::TypeTags::Nothing, 0);
    slotAccessor2.reset(value::TypeTags::Nothing, 0);
    runAndAssertNothing(compiledExpr.get());

    slotAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(i32Val));
    slotAccessor2.reset(value::TypeTags::Null, 0);
    runAndAssertNothing(compiledExpr.get());

    slotAccessor1.reset(value::TypeTags::Null, 0);
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(i32Mod));
    runAndAssertNothing(compiledExpr.get());

    slotAccessor1.reset(value::TypeTags::Null, 0);
    slotAccessor2.reset(value::TypeTags::Null, 0);
    runAndAssertNothing(compiledExpr.get());

    slotAccessor1.reset(value::TypeTags::Null, 0);
    slotAccessor2.reset(value::TypeTags::Nothing, 0);
    runAndAssertNothing(compiledExpr.get());

    slotAccessor1.reset(value::TypeTags::Nothing, 0);
    slotAccessor2.reset(value::TypeTags::Null, 0);
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBEModExprTest, ErrorIfModRHSIsZero) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto argSlot1 = bindAccessor(&slotAccessor1);
    auto argSlot2 = bindAccessor(&slotAccessor2);
    auto modExpr = sbe::makeE<sbe::EFunction>(
        "mod", sbe::makeEs(makeE<EVariable>(argSlot1), makeE<EVariable>(argSlot2)));
    auto compiledExpr = compileExpression(*modExpr);

    const int32_t i32Val = 16;
    const int32_t i32Mod = 0;
    const Decimal128 decVal(123.4);
    const Decimal128 decMod(0);

    slotAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(i32Val));
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(i32Mod));
    runAndAssertThrows(compiledExpr.get());

    auto [tagArgDecVal, valArgDecVal] = value::makeCopyDecimal(decVal);
    slotAccessor1.reset(tagArgDecVal, valArgDecVal);
    auto [tagArgDecMod, valArgDecMod] = value::makeCopyDecimal(decMod);
    slotAccessor2.reset(tagArgDecMod, valArgDecMod);
    runAndAssertThrows(compiledExpr.get());
}

}  // namespace mongo::sbe
