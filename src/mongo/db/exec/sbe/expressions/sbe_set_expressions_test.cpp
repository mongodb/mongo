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

class SBEBuiltinSetOpTest : public EExpressionTestFixture {
protected:
    void runAndAssertExpression(const vm::CodeFragment* compiledExpr,
                                std::pair<value::TypeTags, value::Value> expectedArray) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);

        ASSERT(isArray(tag));
        auto [cmpTag, cmpVal] =
            value::compareValue(tag, val, expectedArray.first, expectedArray.second);
        ASSERT_EQUALS(cmpTag, sbe::value::TypeTags::NumberInt32);
        ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), 0);
    }

    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        auto [tag, val] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(tag, val);

        ASSERT_EQUALS(tag, sbe::value::TypeTags::Nothing);
        ASSERT_EQUALS(val, 0);
    }
};

TEST_F(SBEBuiltinSetOpTest, ComputesSetUnion) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setUnionExpr = sbe::makeE<sbe::EFunction>(
        "setUnion", sbe::makeEs(makeE<EVariable>(arrSlot1), makeE<EVariable>(arrSlot2)));
    auto compiledExpr = compileExpression(*setUnionExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2));
    slotAccessor1.reset(arrTag1, arrVal1);
    auto [arrTag2, arrVal2] = makeArray(BSON_ARRAY(2 << 5 << 3));
    slotAccessor2.reset(arrTag2, arrVal2);
    auto [resArrTag, resArrVal] = makeArraySet(BSON_ARRAY(1 << 2 << 3 << 5));
    value::ValueGuard resGuard(resArrTag, resArrVal);
    runAndAssertExpression(compiledExpr.get(), {resArrTag, resArrVal});

    std::tie(arrTag1, arrVal1) = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    std::tie(arrTag2, arrVal2) = value::makeNewArray();
    slotAccessor2.reset(arrTag2, arrVal2);
    auto [resArrTag1, resArrVal1] = makeArraySet(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard resGuard1(resArrTag1, resArrVal1);
    runAndAssertExpression(compiledExpr.get(), {resArrTag1, resArrVal1});
}

TEST_F(SBEBuiltinSetOpTest, ReturnsNothingSetUnion) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setUnionExpr = sbe::makeE<sbe::EFunction>(
        "setUnion", sbe::makeEs(makeE<EVariable>(arrSlot1), makeE<EVariable>(arrSlot2)));
    auto compiledExpr = compileExpression(*setUnionExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2));
    slotAccessor1.reset(arrTag1, arrVal1);
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(189));
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBEBuiltinSetOpTest, ComputesSetIntersection) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setIntersectionExpr = sbe::makeE<sbe::EFunction>(
        "setIntersection", sbe::makeEs(makeE<EVariable>(arrSlot1), makeE<EVariable>(arrSlot2)));
    auto compiledExpr = compileExpression(*setIntersectionExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    auto [arrTag2, arrVal2] = makeArray(BSON_ARRAY(2 << 5 << 3));
    slotAccessor2.reset(arrTag2, arrVal2);
    auto [resArrTag, resArrVal] = makeArraySet(BSON_ARRAY(2 << 3));
    value::ValueGuard resGuard(resArrTag, resArrVal);
    runAndAssertExpression(compiledExpr.get(), {resArrTag, resArrVal});

    std::tie(arrTag1, arrVal1) = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    std::tie(arrTag2, arrVal2) = value::makeNewArray();
    slotAccessor2.reset(arrTag2, arrVal2);
    auto [resArrTag1, resArrVal1] = value::makeNewArraySet();
    value::ValueGuard resGuard1(resArrTag1, resArrVal1);
    runAndAssertExpression(compiledExpr.get(), {resArrTag1, resArrVal1});
}

TEST_F(SBEBuiltinSetOpTest, ReturnsNothingSetIntersection) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setIntersectionExpr = sbe::makeE<sbe::EFunction>(
        "setIntersection", sbe::makeEs(makeE<EVariable>(arrSlot1), makeE<EVariable>(arrSlot2)));
    auto compiledExpr = compileExpression(*setIntersectionExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(21));
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBEBuiltinSetOpTest, ComputesSetDifference) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setDiffExpr = sbe::makeE<sbe::EFunction>(
        "setDifference", sbe::makeEs(makeE<EVariable>(arrSlot1), makeE<EVariable>(arrSlot2)));
    auto compiledExpr = compileExpression(*setDiffExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    auto [arrTag2, arrVal2] = makeArray(BSON_ARRAY(2 << 5 << 7));
    slotAccessor2.reset(arrTag2, arrVal2);
    auto [resArrTag, resArrVal] = makeArraySet(BSON_ARRAY(1 << 3));
    value::ValueGuard resGuard(resArrTag, resArrVal);
    runAndAssertExpression(compiledExpr.get(), {resArrTag, resArrVal});

    std::tie(arrTag1, arrVal1) = makeArray(BSON_ARRAY(1 << 2 << 3 << 1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    std::tie(arrTag2, arrVal2) = makeArray(BSON_ARRAY(2 << 5 << 7));
    slotAccessor2.reset(arrTag2, arrVal2);
    runAndAssertExpression(compiledExpr.get(), {resArrTag, resArrVal});

    std::tie(arrTag1, arrVal1) = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    std::tie(arrTag2, arrVal2) = value::makeNewArray();
    slotAccessor2.reset(arrTag2, arrVal2);
    auto [resArrTag1, resArrVal1] = makeArraySet(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard resGuard1(resArrTag1, resArrVal1);
    runAndAssertExpression(compiledExpr.get(), {resArrTag1, resArrVal1});
}

TEST_F(SBEBuiltinSetOpTest, ReturnsNothingSetDifference) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setDiffExpr = sbe::makeE<sbe::EFunction>(
        "setDifference", sbe::makeEs(makeE<EVariable>(arrSlot1), makeE<EVariable>(arrSlot2)));
    auto compiledExpr = compileExpression(*setDiffExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2));
    slotAccessor1.reset(arrTag1, arrVal1);
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(125));
    runAndAssertNothing(compiledExpr.get());
}
}  // namespace mongo::sbe
