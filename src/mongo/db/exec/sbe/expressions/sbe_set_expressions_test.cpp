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

#include <array>
#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>

namespace mongo::sbe {

// The array-result set builtins covered by the shared tests below.
constexpr std::array kArrayResultSetFns{
    EFn::kSetUnionArray, EFn::kSetIntersectionArray, EFn::kSetDifferenceArray};

// The array-result set builtins that accept any number of operands.
constexpr std::array kVariadicArrayResultSetFns{EFn::kSetUnionArray, EFn::kSetIntersectionArray};

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

    // Compiles and runs 'expr', then compares the result with 'expected', ignoring element order.
    void runAndAssertExpression(const EExpression& expr, const BSONArray& expected) {
        auto compiledExpr = compileExpression(expr);
        value::TagValueOwned expectedSet = value::TagValueOwned::fromRaw(makeArraySet(expected));
        runAndAssertExpression(compiledExpr.get(), {expectedSet.tag(), expectedSet.value()});
    }

    void runAndAssertNothing(const EExpression& expr) {
        auto compiledExpr = compileExpression(expr);
        runAndAssertNothing(compiledExpr.get());
    }

    static std::unique_ptr<EExpression> noCollator() {
        return makeC(value::TypeTags::Nothing, 0);
    }

    static std::unique_ptr<EExpression> arrayC(const BSONArray& array) {
        return makeC(makeArray(array));
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

/**
 * Runs the shared tests against both the legacy builtins and the array-result builtins.
 */
class SBEBuiltinArrayResultTest : public SBEBuiltinSetOpTest,
                                  public testing::WithParamInterface<bool> {
protected:
    std::unique_ptr<EExpression> makeUnion(std::unique_ptr<EExpression> lhs,
                                           std::unique_ptr<EExpression> rhs) {
        return makeSetOp(EFn::kSetUnion, EFn::kSetUnionArray, std::move(lhs), std::move(rhs));
    }

    std::unique_ptr<EExpression> makeIntersection(std::unique_ptr<EExpression> lhs,
                                                  std::unique_ptr<EExpression> rhs) {
        return makeSetOp(
            EFn::kSetIntersection, EFn::kSetIntersectionArray, std::move(lhs), std::move(rhs));
    }

    std::unique_ptr<EExpression> makeDifference(std::unique_ptr<EExpression> lhs,
                                                std::unique_ptr<EExpression> rhs) {
        return makeSetOp(
            EFn::kSetDifference, EFn::kSetDifferenceArray, std::move(lhs), std::move(rhs));
    }

    std::unique_ptr<EExpression> makeEquals(std::unique_ptr<EExpression> lhs,
                                            std::unique_ptr<EExpression> rhs) {
        return makeSetOp(EFn::kSetEquals, EFn::kSetEqualsArray, std::move(lhs), std::move(rhs));
    }

    std::unique_ptr<EExpression> makeIsSubset(std::unique_ptr<EExpression> lhs,
                                              std::unique_ptr<EExpression> rhs) {
        return makeSetOp(EFn::kSetIsSubset, EFn::kSetIsSubsetArray, std::move(lhs), std::move(rhs));
    }

private:
    std::unique_ptr<EExpression> makeSetOp(EFn legacyFn,
                                           EFn arrayFn,
                                           std::unique_ptr<EExpression> lhs,
                                           std::unique_ptr<EExpression> rhs) {
        if (GetParam()) {
            return makeFunction(
                arrayFn, makeC(value::TypeTags::Nothing, 0), std::move(lhs), std::move(rhs));
        }
        return makeFunction(legacyFn, std::move(lhs), std::move(rhs));
    }
};

INSTANTIATE_TEST_SUITE_P(LegacyAndArrayResult, SBEBuiltinArrayResultTest, testing::Bool());

TEST_P(SBEBuiltinArrayResultTest, ComputesSetUnion) {
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

TEST_P(SBEBuiltinArrayResultTest, ReturnsNothingSetUnion) {
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

TEST_P(SBEBuiltinArrayResultTest, ComputesSetIntersection) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setIntersectionExpr = makeIntersection(makeVariable(arrSlot1), makeVariable(arrSlot2));
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

TEST_P(SBEBuiltinArrayResultTest, ReturnsNothingSetIntersection) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setIntersectionExpr = makeIntersection(makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*setIntersectionExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2 << 3));
    slotAccessor1.reset(arrTag1, arrVal1);
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(21));
    runAndAssertNothing(compiledExpr.get());
}

TEST_P(SBEBuiltinArrayResultTest, ComputesSetDifference) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setDiffExpr = makeDifference(makeVariable(arrSlot1), makeVariable(arrSlot2));
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

TEST_P(SBEBuiltinArrayResultTest, ReturnsNothingSetDifference) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setDiffExpr = makeDifference(makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*setDiffExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2));
    slotAccessor1.reset(arrTag1, arrVal1);
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(125));
    runAndAssertNothing(compiledExpr.get());

    slotAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(125));
    auto [arrTag2, arrVal2] = makeArray(BSON_ARRAY(1 << 2));
    slotAccessor2.reset(arrTag2, arrVal2);
    runAndAssertNothing(compiledExpr.get());
}

TEST_P(SBEBuiltinArrayResultTest, ComputesSetEquals) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setEqualsExpr = makeEquals(makeVariable(arrSlot1), makeVariable(arrSlot2));
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

TEST_P(SBEBuiltinArrayResultTest, ReturnsNothingSetEquals) {
    value::OwnedValueAccessor slotAccessor1, slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setEqualsExpr = makeEquals(makeVariable(arrSlot1), makeVariable(arrSlot2));
    auto compiledExpr = compileExpression(*setEqualsExpr);

    auto [arrTag1, arrVal1] = makeArray(BSON_ARRAY(1 << 2));
    slotAccessor1.reset(arrTag1, arrVal1);
    slotAccessor2.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(189));
    runAndAssertNothing(compiledExpr.get());

    slotAccessor1.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(189));
    auto [arrTag2, arrVal2] = makeArray(BSON_ARRAY(1 << 2));
    slotAccessor2.reset(arrTag2, arrVal2);
    runAndAssertNothing(compiledExpr.get());
}

TEST_P(SBEBuiltinArrayResultTest, ComputesSetIsSubset) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setIsSubsetExpr = makeIsSubset(makeVariable(arrSlot1), makeVariable(arrSlot2));
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

TEST_P(SBEBuiltinArrayResultTest, ReturnsNothingSetIsSubset) {
    value::OwnedValueAccessor slotAccessor1;
    value::OwnedValueAccessor slotAccessor2;
    auto arrSlot1 = bindAccessor(&slotAccessor1);
    auto arrSlot2 = bindAccessor(&slotAccessor2);
    auto setIsSubsetExpr = makeIsSubset(makeVariable(arrSlot1), makeVariable(arrSlot2));
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

TEST_P(SBEBuiltinArrayResultTest, SetUnionWithinLimitSucceeds) {
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

TEST_P(SBEBuiltinArrayResultTest, SetUnionExceedsMemoryLimit) {
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

TEST_F(SBEBuiltinSetOpTest, ArrayResultZeroOperands) {
    for (auto fn : kVariadicArrayResultSetFns) {
        runAndAssertExpression(*makeFunction(fn, noCollator()), BSONArray());
    }
}

TEST_F(SBEBuiltinSetOpTest, ArrayResultAcceptsPreparedSetsAndBsonArrays) {
    auto bson = BSON_ARRAY(3.0 << 2 << 4);
    for (const auto& [fn, expected] : {std::pair{EFn::kSetUnionArray, BSON_ARRAY(1 << 2 << 3 << 4)},
                                       std::pair{EFn::kSetIntersectionArray, BSON_ARRAY(2 << 3)},
                                       std::pair{EFn::kSetDifferenceArray, BSON_ARRAY(1)}}) {
        auto expr =
            makeFunction(fn,
                         noCollator(),
                         makeC(makeArraySet(BSON_ARRAY(1 << 2 << 3))),
                         makeC(value::copyValue(value::TypeTags::bsonArray,
                                                value::bitcastFrom<const char*>(bson.objdata()))));
        runAndAssertExpression(*expr, expected);
    }
}

TEST_F(SBEBuiltinSetOpTest, ArrayResultCollationKeepsFirstRepresentative) {
    CollatorInterfaceMock collator{CollatorInterfaceMock::MockType::kToLowerString};
    for (const auto& [fn, expected] :
         {std::pair{EFn::kSetUnionArray, BSON_ARRAY("B" << "a" << "d" << "C")},
          std::pair{EFn::kSetIntersectionArray, BSON_ARRAY("B" << "a")},
          std::pair{EFn::kSetDifferenceArray, BSON_ARRAY("d")}}) {
        auto expr =
            makeFunction(fn,
                         makeC(value::TypeTags::collator,
                               value::bitcastFrom<CollatorInterface*>(collator.clone().release())),
                         arrayC(BSON_ARRAY("B" << "a" << "A" << "d" << "D")),
                         arrayC(BSON_ARRAY("b" << "C" << "A")));
        runAndAssertExpression(*expr, expected);
    }
}

TEST_F(SBEBuiltinSetOpTest, ArrayResultTransfersOwnedValuesAndCopiesViews) {
    // Each operation returns the first operand: union and intersection when the second operand is
    // equal to it, and difference when the second operand is empty.
    const auto input = BSON_ARRAY("an allocating string" << BSON("x" << 1) << BSON_ARRAY(1 << 2));
    for (auto fn : kArrayResultSetFns) {
        auto second = fn == EFn::kSetDifferenceArray ? BSONArray() : input;
        for (bool owned : {false, true}) {
            value::OwnedValueAccessor slotAccessor;
            std::unique_ptr<EExpression> arg = makeVariable(bindAccessor(&slotAccessor));
            if (owned) {
                arg = makeFunction(EFn::kMakeOwn, std::move(arg));
            }
            auto expr = makeFunction(fn, noCollator(), std::move(arg), arrayC(second));
            auto compiledExpr = compileExpression(*expr);

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
}

TEST_F(SBEBuiltinSetOpTest, SetUnionArrayDuplicatePayloadsOnlyCountOnce) {
    auto str = std::string(1024, 'x');
    unittest::ServerParameterGuard limit{"internalQueryMaxSingleExpressionMemoryUsageBytes", 2048};
    runAndAssertExpression(
        *makeFunction(EFn::kSetUnionArray, noCollator(), arrayC(BSON_ARRAY(str << str << str))),
        BSON_ARRAY(str));
}

TEST_F(SBEBuiltinSetOpTest, SetIntersectionArrayManyOperands) {
    auto intersect = [&](const BSONArray& first, const BSONArray& second, const BSONArray& third) {
        return makeFunction(
            EFn::kSetIntersectionArray, noCollator(), arrayC(first), arrayC(second), arrayC(third));
    };

    runAndAssertExpression(*intersect(BSON_ARRAY(1 << 2 << 2 << 3 << 4),
                                      BSON_ARRAY(3 << 2 << 3 << 5),
                                      BSON_ARRAY(9 << 3 << 2)),
                           BSON_ARRAY(2 << 3));
    // A duplicate in one operand doesn't count as a match in another operand.
    runAndAssertExpression(*intersect(BSON_ARRAY(1 << 2), BSON_ARRAY(1 << 1 << 2), BSON_ARRAY(2)),
                           BSON_ARRAY(2));
    runAndAssertExpression(*intersect(BSON_ARRAY(1 << 2), BSON_ARRAY(3), BSON_ARRAY(1 << 2)),
                           BSONArray());
}

TEST_F(SBEBuiltinSetOpTest, SetIntersectionArrayReturnsNothingAfterEmptyIntersection) {
    runAndAssertNothing(
        *makeFunction(EFn::kSetIntersectionArray,
                      noCollator(),
                      arrayC(BSON_ARRAY(1)),
                      arrayC(BSON_ARRAY(2)),
                      makeC(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5))));
}

TEST_F(SBEBuiltinSetOpTest, SetDifferenceArrayUsesPreparedRhsOnlyWithSameCollator) {
    runAndAssertExpression(*makeFunction(EFn::kSetDifferenceArray,
                                         noCollator(),
                                         arrayC(BSON_ARRAY(1 << 2 << 3 << 4 << 1)),
                                         makeC(makeArraySet(BSON_ARRAY(2 << 4)))),
                           BSON_ARRAY(1 << 3));

    CollatorInterfaceMock collator{CollatorInterfaceMock::MockType::kToLowerString};
    value::ViewOfValueAccessor collatorAccessor;
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterface*>(&collator));
    auto collatorSlot = bindAccessor(&collatorAccessor);

    // A prepared set with the same collator.
    runAndAssertExpression(
        *makeFunction(EFn::kSetDifferenceArray,
                      makeVariable(collatorSlot),
                      arrayC(BSON_ARRAY("A" << "b")),
                      makeC(makeArraySetWithCollator(BSON_ARRAY("a"), &collator))),
        BSON_ARRAY("b"));

    // A prepared set without a collator must not be used to look up collated values.
    runAndAssertExpression(*makeFunction(EFn::kSetDifferenceArray,
                                         makeVariable(collatorSlot),
                                         arrayC(BSON_ARRAY("A" << "b")),
                                         makeC(makeArraySet(BSON_ARRAY("a")))),
                           BSON_ARRAY("b"));

    // A prepared set with a collator must not be used to look up values without one.
    runAndAssertExpression(
        *makeFunction(EFn::kSetDifferenceArray,
                      noCollator(),
                      arrayC(BSON_ARRAY("A" << "b")),
                      makeC(makeArraySetWithCollator(BSON_ARRAY("a"), &collator))),
        BSON_ARRAY("A" << "b"));
}

TEST_F(SBEBuiltinSetOpTest, SetEqualsArrayManyOperands) {
    auto assertEquals = [&](const BSONArray& first,
                            const BSONArray& second,
                            const BSONArray& third,
                            bool expected) {
        auto expr = makeFunction(
            EFn::kSetEqualsArray, noCollator(), arrayC(first), arrayC(second), arrayC(third));
        auto compiledExpr = compileExpression(*expr);
        runAndAssertBoolean(compiledExpr.get(), expected);
    };

    assertEquals(
        BSON_ARRAY(1 << 2 << 3), BSON_ARRAY(3 << 1 << 2 << 1), BSON_ARRAY(2 << 2 << 3 << 1), true);
    // The last operand is missing a value.
    assertEquals(BSON_ARRAY(1 << 2 << 3), BSON_ARRAY(3 << 2 << 1), BSON_ARRAY(1 << 2 << 1), false);
    // The last operand has an extra value.
    assertEquals(BSON_ARRAY(1 << 2), BSON_ARRAY(2 << 1), BSON_ARRAY(1 << 2 << 3), false);
    assertEquals(BSONArray(), BSONArray(), BSONArray(), true);
    assertEquals(BSONArray(), BSON_ARRAY(1), BSONArray(), false);
}

TEST_F(SBEBuiltinSetOpTest, SetEqualsArrayReturnsNothingForLaterNonArray) {
    auto expr = makeFunction(EFn::kSetEqualsArray,
                             noCollator(),
                             arrayC(BSON_ARRAY(1)),
                             arrayC(BSON_ARRAY(2)),
                             makeC(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5)));
    runAndAssertNothing(*expr);
}

TEST_F(SBEBuiltinSetOpTest, SetEqualsArrayAcceptsPreparedSetsAndBsonArrays) {
    auto bson = BSON_ARRAY(3.0 << 2 << 1 << 2);
    auto expr =
        makeFunction(EFn::kSetEqualsArray,
                     noCollator(),
                     makeC(makeArraySet(BSON_ARRAY(1 << 2 << 3))),
                     makeC(value::copyValue(value::TypeTags::bsonArray,
                                            value::bitcastFrom<const char*>(bson.objdata()))),
                     makeC(makeArraySet(BSON_ARRAY(3 << 1 << 2))));
    auto compiledExpr = compileExpression(*expr);
    runAndAssertBoolean(compiledExpr.get(), true);
}

TEST_F(SBEBuiltinSetOpTest, SetEqualsArrayUsesPreparedSetsOnlyWithSameCollator) {
    CollatorInterfaceMock collator{CollatorInterfaceMock::MockType::kToLowerString};
    value::ViewOfValueAccessor collatorAccessor;
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterface*>(&collator));
    auto collatorSlot = bindAccessor(&collatorAccessor);
    auto assertEquals = [&](std::unique_ptr<EExpression> coll,
                            std::unique_ptr<EExpression> first,
                            std::unique_ptr<EExpression> second,
                            bool expected) {
        auto expr = makeFunction(
            EFn::kSetEqualsArray, std::move(coll), std::move(first), std::move(second));
        auto compiledExpr = compileExpression(*expr);
        runAndAssertBoolean(compiledExpr.get(), expected);
    };

    assertEquals(makeVariable(collatorSlot),
                 arrayC(BSON_ARRAY("a" << "B")),
                 arrayC(BSON_ARRAY("b" << "A" << "a")),
                 true);
    // Prepared sets with the same collator, as either operand.
    assertEquals(makeVariable(collatorSlot),
                 arrayC(BSON_ARRAY("A")),
                 makeC(makeArraySetWithCollator(BSON_ARRAY("a"), &collator)),
                 true);
    assertEquals(makeVariable(collatorSlot),
                 makeC(makeArraySetWithCollator(BSON_ARRAY("a"), &collator)),
                 arrayC(BSON_ARRAY("A")),
                 true);
    // Prepared sets without a collator must not be used to look up collated values. Here they
    // hold two values that are equal under the collator.
    assertEquals(makeVariable(collatorSlot),
                 arrayC(BSON_ARRAY("A")),
                 makeC(makeArraySet(BSON_ARRAY("a" << "A"))),
                 true);
    assertEquals(makeVariable(collatorSlot),
                 makeC(makeArraySet(BSON_ARRAY("a" << "A"))),
                 arrayC(BSON_ARRAY("A")),
                 true);
    // Prepared sets with a collator must not be used to look up values without one.
    assertEquals(noCollator(),
                 arrayC(BSON_ARRAY("A")),
                 makeC(makeArraySetWithCollator(BSON_ARRAY("a"), &collator)),
                 false);
    assertEquals(noCollator(),
                 makeC(makeArraySetWithCollator(BSON_ARRAY("a"), &collator)),
                 arrayC(BSON_ARRAY("A")),
                 false);
}

TEST_F(SBEBuiltinSetOpTest, SetIsSubsetArrayAcceptsPreparedSetsAndBsonArrays) {
    auto bson = BSON_ARRAY(3.0 << 2 << 2);
    auto bsonArray = [&] {
        return makeC(value::copyValue(value::TypeTags::bsonArray,
                                      value::bitcastFrom<const char*>(bson.objdata())));
    };
    auto assertIsSubset =
        [&](std::unique_ptr<EExpression> lhs, std::unique_ptr<EExpression> rhs, bool expected) {
            auto expr =
                makeFunction(EFn::kSetIsSubsetArray, noCollator(), std::move(lhs), std::move(rhs));
            auto compiledExpr = compileExpression(*expr);
            runAndAssertBoolean(compiledExpr.get(), expected);
        };

    assertIsSubset(bsonArray(), makeC(makeArraySet(BSON_ARRAY(1 << 2 << 3))), true);
    assertIsSubset(makeC(makeArraySet(BSON_ARRAY(1 << 2 << 3))), bsonArray(), false);
}

TEST_F(SBEBuiltinSetOpTest, SetIsSubsetArrayUsesPreparedRhsOnlyWithSameCollator) {
    CollatorInterfaceMock collator{CollatorInterfaceMock::MockType::kToLowerString};
    value::ViewOfValueAccessor collatorAccessor;
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterface*>(&collator));
    auto collatorSlot = bindAccessor(&collatorAccessor);
    auto assertIsSubset =
        [&](std::unique_ptr<EExpression> coll, std::unique_ptr<EExpression> rhs, bool expected) {
            auto expr = makeFunction(
                EFn::kSetIsSubsetArray, std::move(coll), arrayC(BSON_ARRAY("A")), std::move(rhs));
            auto compiledExpr = compileExpression(*expr);
            runAndAssertBoolean(compiledExpr.get(), expected);
        };

    assertIsSubset(makeVariable(collatorSlot), arrayC(BSON_ARRAY("a" << "b")), true);
    // A prepared set with the same collator.
    assertIsSubset(makeVariable(collatorSlot),
                   makeC(makeArraySetWithCollator(BSON_ARRAY("a"), &collator)),
                   true);
    // A prepared set without a collator must not be used to look up collated values.
    assertIsSubset(makeVariable(collatorSlot), makeC(makeArraySet(BSON_ARRAY("a"))), true);
    // A prepared set with a collator must not be used to look up values without one.
    assertIsSubset(
        noCollator(), makeC(makeArraySetWithCollator(BSON_ARRAY("a"), &collator)), false);
}

class SBEBuiltinSetOpDeathTest : public SBEBuiltinSetOpTest {};

DEATH_TEST_REGEX_F(SBEBuiltinSetOpDeathTest, SetUnionArrayInvalidCollator, "13889300") {
    runExpression(
        *makeFunction(EFn::kSetUnionArray, makeC(value::TypeTags::Null, 0), arrayC(BSONArray())));
}

DEATH_TEST_REGEX_F(SBEBuiltinSetOpDeathTest, SetIntersectionArrayInvalidCollator, "13889300") {
    runExpression(*makeFunction(
        EFn::kSetIntersectionArray, makeC(value::TypeTags::Null, 0), arrayC(BSONArray())));
}

DEATH_TEST_REGEX_F(SBEBuiltinSetOpDeathTest, SetDifferenceArrayInvalidCollator, "13889300") {
    runExpression(*makeFunction(EFn::kSetDifferenceArray,
                                makeC(value::TypeTags::Null, 0),
                                arrayC(BSONArray()),
                                arrayC(BSONArray())));
}

DEATH_TEST_REGEX_F(SBEBuiltinSetOpDeathTest, SetEqualsArrayInvalidCollator, "13889300") {
    runExpression(*makeFunction(EFn::kSetEqualsArray,
                                makeC(value::TypeTags::Null, 0),
                                arrayC(BSONArray()),
                                arrayC(BSONArray())));
}

DEATH_TEST_REGEX_F(SBEBuiltinSetOpDeathTest, SetIsSubsetArrayInvalidCollator, "13889300") {
    runExpression(*makeFunction(EFn::kSetIsSubsetArray,
                                makeC(value::TypeTags::Null, 0),
                                arrayC(BSONArray()),
                                arrayC(BSONArray())));
}
}  // namespace mongo::sbe
