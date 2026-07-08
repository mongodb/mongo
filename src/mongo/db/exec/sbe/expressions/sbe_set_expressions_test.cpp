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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>

namespace mongo::sbe {

class SBEBuiltinSetOpTest : public EExpressionTestFixture {
protected:
    void runAndAssertExpression(const vm::CodeFragment* compiledExpr,
                                std::pair<value::TypeTags, value::Value> expectedArray) {
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));

        ASSERT(isArray(result.tag()));
        auto [cmpTag, cmpVal] = value::compareValue(
            result.tag(), result.value(), expectedArray.first, expectedArray.second);
        ASSERT_EQUALS(cmpTag, sbe::value::TypeTags::NumberInt32);
        ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), 0);
    }

    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));

        ASSERT_EQUALS(result.tag(), sbe::value::TypeTags::Nothing);
        ASSERT_EQUALS(result.value(), 0);
    }

    void runAndAssertBoolean(const vm::CodeFragment* compiledExpr, bool expected) {
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));

        ASSERT(result.tag() == value::TypeTags::Boolean);
        ASSERT_EQUALS(value::bitcastTo<bool>(result.value()), expected);
    }
};

TEST_F(SBEBuiltinSetOpTest, ComputesSetUnion) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setUnionExpr =
        makeFunction(EFn::kSetUnion, makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*setUnionExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2));
    slotAccessor1.reset(arrTag1, arrVal1);
    auto [arrTag2, arrVal2] = makeArray(BSON_ARRAY(2 << 5 << 3));
    slotAccessor2.reset(arrTag2, arrVal2);
    value::TagValueOwned unionResult =
        value::TagValueOwned::fromRaw(makeArraySet(BSON_ARRAY(1 << 2 << 3 << 5)));
    runAndAssertExpression(compiledExpr.get(), {unionResult.tag(), unionResult.value()});

    std::tie(arrTag1, arrVal1) = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    std::tie(arrTag2, arrVal2) = value::makeNewArray();
    slotAccessor2.reset(arrTag2, arrVal2);
    value::TagValueOwned unionResult1 =
        value::TagValueOwned::fromRaw(makeArraySet(BSON_ARRAY(1 << 2 << 3)));
    runAndAssertExpression(compiledExpr.get(), {unionResult1.tag(), unionResult1.value()});
}

TEST_F(SBEBuiltinSetOpTest, ReturnsNothingSetUnion) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setUnionExpr =
        makeFunction(EFn::kSetUnion, makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*setUnionExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2));
    slotAccessor1.reset(arrTag1, arrVal1);
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(189));
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBEBuiltinSetOpTest, AggSetUnion) {
    value::OwnedValueAccessor aggAccessor, inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    auto setUnionExpr = makeFunction(EFn::kAggSetUnion, makeVariable(inputSlot));
    auto compiledExpr = compileAggExpression(*setUnionExpr, &aggAccessor);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2));
    inputAccessor.reset(arrTag1, arrVal1);
    auto [resTag1, resVal1] = makeArraySet(BSON_ARRAY(1 << 2));
    runAndAssertExpression(compiledExpr.get(), {resTag1, resVal1});
    aggAccessor.reset(resTag1, resVal1);

    auto [arrTag2, arrVal2] = makeArraySet(BSON_ARRAY(1 << 3 << 2 << 6));
    inputAccessor.reset(arrTag2, arrVal2);
    auto [resTag2, resVal2] = makeArraySet(BSON_ARRAY(1 << 2 << 3 << 6));
    runAndAssertExpression(compiledExpr.get(), {resTag2, resVal2});
    aggAccessor.reset(resTag2, resVal2);

    auto [arrTag3, arrVal3] = makeArray(BSONArray{});
    inputAccessor.reset(arrTag3, arrVal3);
    auto [resTag3, resVal3] = makeArraySet(BSON_ARRAY(1 << 2 << 3 << 6));
    runAndAssertExpression(compiledExpr.get(), {resTag3, resVal3});
    aggAccessor.reset(resTag3, resVal3);

    inputAccessor.reset(value::TypeTags::Nothing, 0);
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBEBuiltinSetOpTest, ComputesSetIntersection) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setIntersectionExpr =
        makeFunction(EFn::kSetIntersection, makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*setIntersectionExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    auto [arrTag2, arrVal2] = makeArray(BSON_ARRAY(2 << 5 << 3));
    slotAccessor2.reset(arrTag2, arrVal2);
    value::TagValueOwned intersectionResult =
        value::TagValueOwned::fromRaw(makeArraySet(BSON_ARRAY(2 << 3)));
    runAndAssertExpression(compiledExpr.get(),
                           {intersectionResult.tag(), intersectionResult.value()});

    std::tie(arrTag1, arrVal1) = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    std::tie(arrTag2, arrVal2) = value::makeNewArray();
    slotAccessor2.reset(arrTag2, arrVal2);
    value::TagValueOwned intersectionResult1 =
        value::TagValueOwned::fromRaw(value::makeNewArraySet());
    runAndAssertExpression(compiledExpr.get(),
                           {intersectionResult1.tag(), intersectionResult1.value()});
}

TEST_F(SBEBuiltinSetOpTest, ReturnsNothingSetIntersection) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setIntersectionExpr =
        makeFunction(EFn::kSetIntersection, makeVariable(arrSlot1), makeVariable(arrSlot2));
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
    auto setDiffExpr =
        makeFunction(EFn::kSetDifference, makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*setDiffExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    auto [arrTag2, arrVal2] = makeArray(BSON_ARRAY(2 << 5 << 7));
    slotAccessor2.reset(arrTag2, arrVal2);
    value::TagValueOwned diffResult =
        value::TagValueOwned::fromRaw(makeArraySet(BSON_ARRAY(1 << 3)));
    runAndAssertExpression(compiledExpr.get(), {diffResult.tag(), diffResult.value()});

    std::tie(arrTag1, arrVal1) = makeArray(BSON_ARRAY(1 << 2 << 3 << 1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    std::tie(arrTag2, arrVal2) = makeArray(BSON_ARRAY(2 << 5 << 7));
    slotAccessor2.reset(arrTag2, arrVal2);
    runAndAssertExpression(compiledExpr.get(), {diffResult.tag(), diffResult.value()});

    std::tie(arrTag1, arrVal1) = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    std::tie(arrTag2, arrVal2) = value::makeNewArray();
    slotAccessor2.reset(arrTag2, arrVal2);
    value::TagValueOwned diffResult1 =
        value::TagValueOwned::fromRaw(makeArraySet(BSON_ARRAY(1 << 2 << 3)));
    runAndAssertExpression(compiledExpr.get(), {diffResult1.tag(), diffResult1.value()});
}

TEST_F(SBEBuiltinSetOpTest, ReturnsNothingSetDifference) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setDiffExpr =
        makeFunction(EFn::kSetDifference, makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*setDiffExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2));
    slotAccessor1.reset(arrTag1, arrVal1);
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(125));
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBEBuiltinSetOpTest, ComputesSetEquals) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setEqualsExpr =
        makeFunction(EFn::kSetEquals, makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*setEqualsExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    auto [arrTag2, arrVal2] = makeArray(BSON_ARRAY(3 << 2 << 3 << 1));
    slotAccessor2.reset(arrTag2, arrVal2);
    runAndAssertBoolean(compiledExpr.get(), true);

    std::tie(arrTag1, arrVal1) = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    std::tie(arrTag2, arrVal2) = value::makeNewArray();
    slotAccessor2.reset(arrTag2, arrVal2);
    runAndAssertBoolean(compiledExpr.get(), false);
}

TEST_F(SBEBuiltinSetOpTest, ReturnsNothingSetEquals) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setEqualsExpr =
        makeFunction(EFn::kSetEquals, makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*setEqualsExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2));
    slotAccessor1.reset(arrTag1, arrVal1);
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(189));
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBEBuiltinSetOpTest, ComputesSetIsSubset) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setIsSubsetExpr =
        makeFunction(EFn::kSetIsSubset, makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*setIsSubsetExpr);

    // all elements are the same
    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    auto [arrTag2, arrVal2] = makeArray(BSON_ARRAY(3 << 2 << 1));
    slotAccessor2.reset(arrTag2, arrVal2);
    runAndAssertBoolean(compiledExpr.get(), true);

    // first array has the same elements multiple times
    std::tie(arrTag1, arrVal1) = makeArray(BSON_ARRAY(1 << 2 << 3 << 1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    std::tie(arrTag2, arrVal2) = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor2.reset(arrTag2, arrVal2);
    runAndAssertBoolean(compiledExpr.get(), true);

    // first array is subset of the second array
    std::tie(arrTag1, arrVal1) = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    std::tie(arrTag2, arrVal2) = makeArray(BSON_ARRAY(1 << 2 << 3 << 4 << 5));
    slotAccessor2.reset(arrTag2, arrVal2);
    runAndAssertBoolean(compiledExpr.get(), true);

    // first array is not subset of the second array
    std::tie(arrTag1, arrVal1) = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    std::tie(arrTag2, arrVal2) = makeArray(BSON_ARRAY(1 << 2 << 4 << 5));
    slotAccessor2.reset(arrTag2, arrVal2);
    runAndAssertBoolean(compiledExpr.get(), false);

    // second array is empty
    std::tie(arrTag1, arrVal1) = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    std::tie(arrTag2, arrVal2) = value::makeNewArray();
    slotAccessor2.reset(arrTag2, arrVal2);
    runAndAssertBoolean(compiledExpr.get(), false);

    // first array is empty
    std::tie(arrTag1, arrVal1) = value::makeNewArray();
    slotAccessor1.reset(arrTag1, arrVal1);
    std::tie(arrTag2, arrVal2) = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor2.reset(arrTag2, arrVal2);
    runAndAssertBoolean(compiledExpr.get(), true);
}

TEST_F(SBEBuiltinSetOpTest, ReturnsNothingSetIsSubset) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setIsSubsetExpr =
        makeFunction(EFn::kSetIsSubset, makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*setIsSubsetExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2));
    slotAccessor1.reset(arrTag1, arrVal1);
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(125));
    runAndAssertNothing(compiledExpr.get());

    slotAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(125));
    auto [arrTag2, arrVal2] = makeArray(BSON_ARRAY(1 << 2));
    slotAccessor2.reset(arrTag2, arrVal2);
    runAndAssertNothing(compiledExpr.get());
}
}  // namespace mongo::sbe
