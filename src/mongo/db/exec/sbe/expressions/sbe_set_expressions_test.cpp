// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/sbe_unittest_assert.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>

namespace mongo::sbe {

class SBEBuiltinSetOpTest : public EExpressionTestFixture {
protected:
    /**
     * Compares the result with the expected ArraySet, ignoring element order.
     */
    void runAndAssertExpression(const vm::CodeFragment* compiledExpr,
                                std::pair<value::TypeTags, value::Value> expectedArray) {
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));

        ASSERT(isArray(result.tag()));
        ASSERT_EQ(value::getArraySize(result.tag(), result.value()),
                  value::getArraySize(expectedArray.first, expectedArray.second));
        value::TagValueOwned resultSet = value::TagValueOwned::fromRaw(
            value::makeNewArraySet(result.tag(), result.value(), nullptr));
        ASSERT_SBE_VALUE_EQ(
            resultSet.tag(), resultSet.value(), expectedArray.first, expectedArray.second);
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

/** Runs the $setUnion tests against both the legacy builtin and setUnionArray. */
class SBEBuiltinUnionTest : public SBEBuiltinSetOpTest, public testing::WithParamInterface<bool> {
protected:
    std::unique_ptr<EExpression> makeUnion(std::unique_ptr<EExpression> lhs,
                                           std::unique_ptr<EExpression> rhs) {
        if (GetParam()) {
            return makeFunction(EFn::kSetUnionArray,
                                makeC(value::TypeTags::Nothing, 0),
                                std::move(lhs),
                                std::move(rhs));
        }
        return makeFunction(EFn::kSetUnion, std::move(lhs), std::move(rhs));
    }
};

INSTANTIATE_TEST_SUITE_P(LegacyAndArrayResult, SBEBuiltinUnionTest, testing::Bool());

TEST_P(SBEBuiltinUnionTest, ComputesSetUnion) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setUnionExpr = makeUnion(makeVariable(arrSlot1), makeVariable(arrSlot2));
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

TEST_P(SBEBuiltinUnionTest, ReturnsNothingSetUnion) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setUnionExpr = makeUnion(makeVariable(arrSlot1), makeVariable(arrSlot2));
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

TEST_P(SBEBuiltinUnionTest, SetUnionWithinLimitSucceeds) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setUnionExpr = makeUnion(makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*setUnionExpr);

    const std::string largeStr(1024, 'a');
    BSONArrayBuilder builder1;
    BSONArrayBuilder builder2;
    for (int i = 0; i < 5; ++i) {
        builder1.append(largeStr + std::to_string(i));
        builder2.append(largeStr + std::to_string(i + 5));
    }
    auto [arrTag1, arrVal1] = makeArray(builder1.arr());
    slotAccessor1.reset(arrTag1, arrVal1);
    auto [arrTag2, arrVal2] = makeArray(builder2.arr());
    slotAccessor2.reset(arrTag2, arrVal2);

    // 10 unique strings * ~1025 bytes = ~10KB; limit set well above that.
    unittest::ServerParameterGuard limit{"internalQueryMaxSingleExpressionMemoryUsageBytes",
                                         20 * 1024};
    auto [resTag, resVal] = runCompiledExpression(compiledExpr.get());
    value::TagValueOwned guard = value::TagValueOwned::fromRaw(resTag, resVal);

    ASSERT_EQ(resTag, GetParam() ? value::TypeTags::Array : value::TypeTags::ArraySet);
    ASSERT_EQUALS(value::getArraySize(resTag, resVal), 10u);
}

TEST_P(SBEBuiltinUnionTest, SetUnionExceedsMemoryLimit) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setUnionExpr = makeUnion(makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*setUnionExpr);

    const std::string largeStr(1024, 'a');
    BSONArrayBuilder builder1;
    BSONArrayBuilder builder2;
    for (int i = 0; i < 5; ++i) {
        builder1.append(largeStr + std::to_string(i));
        builder2.append(largeStr + std::to_string(i + 5));
    }
    auto [arrTag1, arrVal1] = makeArray(builder1.arr());
    slotAccessor1.reset(arrTag1, arrVal1);
    auto [arrTag2, arrVal2] = makeArray(builder2.arr());
    slotAccessor2.reset(arrTag2, arrVal2);

    unittest::ServerParameterGuard limit{"internalQueryMaxSingleExpressionMemoryUsageBytes",
                                         10 * 1024};
    ASSERT_THROWS_CODE(runCompiledExpression(compiledExpr.get()),
                       AssertionException,
                       ErrorCodes::ExceededMemoryLimit);
}

TEST_F(SBEBuiltinSetOpTest, SetUnionArrayZeroOperands) {
    auto setUnionExpr = makeFunction(EFn::kSetUnionArray, makeC(value::TypeTags::Nothing, 0));
    auto compiledExpr = compileExpression(*setUnionExpr);

    value::TagValueOwned unionResult = value::TagValueOwned::fromRaw(value::makeNewArraySet());
    runAndAssertExpression(compiledExpr.get(), {unionResult.tag(), unionResult.value()});
}

TEST_F(SBEBuiltinSetOpTest, SetUnionArrayAcceptsPreparedSetsAndBsonArrays) {
    auto bson = BSON_ARRAY(3 << 4 << 1.0);
    auto setUnionExpr =
        makeFunction(EFn::kSetUnionArray,
                     makeC(value::TypeTags::Nothing, 0),
                     makeC(makeArraySet(BSON_ARRAY(1 << 2))),
                     makeC(value::copyValue(value::TypeTags::bsonArray,
                                            value::bitcastFrom<const char*>(bson.objdata()))));
    auto compiledExpr = compileExpression(*setUnionExpr);

    value::TagValueOwned unionResult =
        value::TagValueOwned::fromRaw(makeArraySet(BSON_ARRAY(1 << 2 << 3 << 4)));
    runAndAssertExpression(compiledExpr.get(), {unionResult.tag(), unionResult.value()});
}

TEST_F(SBEBuiltinSetOpTest, SetUnionArrayCollationKeepsFirstRepresentative) {
    CollatorInterfaceMock collator{CollatorInterfaceMock::MockType::kToLowerString};
    auto setUnionExpr =
        makeFunction(EFn::kSetUnionArray,
                     makeC(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterface*>(collator.clone().release())),
                     makeC(makeArray(BSON_ARRAY("B" << "a" << "A"))),
                     makeC(makeArray(BSON_ARRAY("b" << "C"))));
    auto compiledExpr = compileExpression(*setUnionExpr);

    value::TagValueOwned unionResult =
        value::TagValueOwned::fromRaw(makeArraySet(BSON_ARRAY("B" << "a" << "C")));
    runAndAssertExpression(compiledExpr.get(), {unionResult.tag(), unionResult.value()});
}

TEST_F(SBEBuiltinSetOpTest, SetUnionArrayDuplicatePayloadsOnlyCountOnce) {
    auto str = std::string(1024, 'x');
    auto setUnionExpr = makeFunction(EFn::kSetUnionArray,
                                     makeC(value::TypeTags::Nothing, 0),
                                     makeC(makeArray(BSON_ARRAY(str << str << str))));
    auto compiledExpr = compileExpression(*setUnionExpr);

    unittest::ServerParameterGuard limit{"internalQueryMaxSingleExpressionMemoryUsageBytes", 2048};
    value::TagValueOwned unionResult = value::TagValueOwned::fromRaw(makeArraySet(BSON_ARRAY(str)));
    runAndAssertExpression(compiledExpr.get(), {unionResult.tag(), unionResult.value()});
}

TEST_F(SBEBuiltinSetOpTest, SetUnionArrayTransfersOwnedValuesAndCopiesViews) {
    const auto input = BSON_ARRAY("an allocating string" << BSON("x" << 1) << BSON_ARRAY(1 << 2));
    for (bool owned : {false, true}) {
        value::OwnedValueAccessor slotAccessor;
        std::unique_ptr<EExpression> arg = makeVariable(bindAccessor(&slotAccessor));
        if (owned) {
            arg = makeFunction(EFn::kMakeOwn, std::move(arg));
        }
        auto setUnionExpr =
            makeFunction(EFn::kSetUnionArray, makeC(value::TypeTags::Nothing, 0), std::move(arg));
        auto compiledExpr = compileExpression(*setUnionExpr);

        auto [arrTag, arrVal] = makeArray(input);
        slotAccessor.reset(arrTag, arrVal);
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));
        if (!owned) {
            for (size_t i = 0; i < 3; ++i) {
                ASSERT_NE(value::getArrayView(result.value())->getAt(i).value,
                          value::getArrayView(arrVal)->getAt(i).value);
            }
        }

        // The result must stay valid after the input is released.
        slotAccessor.reset();
        value::TagValueOwned expected = value::TagValueOwned::fromRaw(makeArray(input));
        ASSERT_SBE_VALUE_EQ(result.view(), expected.view());
    }
}

class SBEBuiltinSetOpDeathTest : public SBEBuiltinSetOpTest {};

DEATH_TEST_REGEX_F(SBEBuiltinSetOpDeathTest, SetUnionArrayInvalidCollator, "13889300") {
    auto setUnionExpr = makeFunction(
        EFn::kSetUnionArray, makeC(value::TypeTags::Null, 0), makeC(value::makeNewArray()));
    runExpression(*setUnionExpr);
}
}  // namespace mongo::sbe
