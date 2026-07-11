// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mongo::sbe {

enum class RemovableSumOp { kAdd, kRemove };

class SBERemovableSumTest : public EExpressionTestFixture {
public:
    void runAndAssertExpression(std::vector<std::pair<value::TypeTags, value::Value>>& inputValues,
                                std::vector<RemovableSumOp>& operations,
                                std::vector<std::pair<value::TypeTags, value::Value>>& expValues) {
        value::ViewOfValueAccessor inputAccessor;
        auto inputSlot = bindAccessor(&inputAccessor);

        value::OwnedValueAccessor aggAccessor;
        auto aggSlot = bindAccessor(&aggAccessor);

        auto aggRemovableSumAdd = sbe::makeE<sbe::EFunction>(
            EFn::kAggRemovableSumAdd, sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledRemovableSumAdd = compileAggExpression(*aggRemovableSumAdd, &aggAccessor);

        auto aggRemovableSumRemove = sbe::makeE<sbe::EFunction>(
            EFn::kAggRemovableSumRemove, sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledRemovableSumRemove =
            compileAggExpression(*aggRemovableSumRemove, &aggAccessor);

        auto aggRemovableSumFinalize = sbe::makeE<sbe::EFunction>(
            EFn::kAggRemovableSumFinalize, sbe::makeEs(makeE<EVariable>(aggSlot)));
        auto compiledRemovableSumFinalize = compileExpression(*aggRemovableSumFinalize);

        // call RemovableSumOp (Add/Remove) on the inputs and call finalize() method after each op
        size_t addIdx = 0, removeIdx = 0;
        for (size_t i = 0; i < operations.size(); ++i) {
            if (operations[i] == RemovableSumOp::kAdd) {
                inputAccessor.reset(inputValues[addIdx].first, inputValues[addIdx].second);
                auto [runTag, runVal] = runCompiledExpression(compiledRemovableSumAdd.get());

                aggAccessor.reset(runTag, runVal);
                auto [outTag, outVal] = runCompiledExpression(compiledRemovableSumFinalize.get());

                ASSERT_EQ(expValues[i].first, outTag);
                auto [compareTag, compareVal] =
                    value::compareValue(expValues[i].first, expValues[i].second, outTag, outVal);
                ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
                ASSERT_EQ(value::bitcastTo<int32_t>(compareVal), 0);
                value::releaseValue(outTag, outVal);
                value::releaseValue(expValues[i].first, expValues[i].second);
                ++addIdx;
            } else {
                inputAccessor.reset(inputValues[removeIdx].first, inputValues[removeIdx].second);
                auto [runTag, runVal] = runCompiledExpression(compiledRemovableSumRemove.get());

                aggAccessor.reset(runTag, runVal);
                auto [outTag, outVal] = runCompiledExpression(compiledRemovableSumFinalize.get());

                ASSERT_EQ(expValues[i].first, outTag);
                auto [compareTag, compareVal] =
                    value::compareValue(expValues[i].first, expValues[i].second, outTag, outVal);
                ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
                ASSERT_EQ(value::bitcastTo<int32_t>(compareVal), 0);
                value::releaseValue(outTag, outVal);
                value::releaseValue(expValues[i].first, expValues[i].second);
                ++removeIdx;
            }
        }
        for (size_t i = 0; i < inputValues.size(); ++i) {
            value::releaseValue(inputValues[i].first, inputValues[i].second);
        }
    }
};

TEST_F(SBERemovableSumTest, SumWithMixedTypes1) {  // int, long, double, decimal
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-10)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-10ll)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::Boolean, value::bitcastFrom<bool>(true)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-10.0}).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-10LL)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-10)},
    };

    std::vector<RemovableSumOp> removableSumOps = {RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-10)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-20)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-30.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-30.0)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-40.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-50.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-60.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-70.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-60.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-50.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-40.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-40.0}).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-30.0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-20)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-10)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
    };

    runAndAssertExpression(inputValues, removableSumOps, expValues);
}

TEST_F(SBERemovableSumTest, SumWithMixedTypes2) {  // int, long, double
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-10)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-10ll)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::Boolean, value::bitcastFrom<bool>(true)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-10LL)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-10)},
    };

    std::vector<RemovableSumOp> removableSumOps = {RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-10)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-20)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-30.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-30.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-40.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-50.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-40.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-30.0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-20)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-20)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-10)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
    };

    runAndAssertExpression(inputValues, removableSumOps, expValues);
}

TEST_F(SBERemovableSumTest, SumWithMixedTypes3) {  // int, long
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2147483647)},
        {value::TypeTags::NumberInt64,
         value::bitcastFrom<int64_t>(std::numeric_limits<int64_t>::max())},
    };

    std::vector<RemovableSumOp> removableSumOps = {RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2147483648ll)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2147483647)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(9.2233720390022595e+18L)},
        {value::TypeTags::NumberInt64,
         value::bitcastFrom<int64_t>(std::numeric_limits<int64_t>::max())},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
    };

    runAndAssertExpression(inputValues, removableSumOps, expValues);
}

TEST_F(SBERemovableSumTest, SumWithOverflow) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32,
         value::bitcastFrom<int32_t>(std::numeric_limits<int32_t>::min())},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)},
        {value::TypeTags::NumberInt64,
         value::bitcastFrom<int64_t>(std::numeric_limits<int64_t>::min())},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)},
    };

    std::vector<RemovableSumOp> removableSumOps = {RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberInt32,
         value::bitcastFrom<int32_t>(std::numeric_limits<int32_t>::min())},
        {value::TypeTags::NumberInt64,
         value::bitcastFrom<int64_t>(static_cast<long long>(std::numeric_limits<int32_t>::min()) -
                                     1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt64,
         value::bitcastFrom<int64_t>(std::numeric_limits<int64_t>::min())},
        {value::TypeTags::NumberInt64,
         value::bitcastFrom<int64_t>(std::numeric_limits<int64_t>::min())},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
    };

    runAndAssertExpression(inputValues, removableSumOps, expValues);
}

TEST_F(SBERemovableSumTest, SumWithNaNAndInfinityValues) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(10)},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::infinity())},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(-std::numeric_limits<double>::infinity())},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(20)},
    };

    std::vector<RemovableSumOp> removableSumOps = {RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kAdd,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove,
                                                   RemovableSumOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(10)},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(-std::numeric_limits<double>::infinity())},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(20)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
    };

    runAndAssertExpression(inputValues, removableSumOps, expValues);
}

}  // namespace mongo::sbe
