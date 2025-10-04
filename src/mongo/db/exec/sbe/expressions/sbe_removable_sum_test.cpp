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
#include "mongo/db/exec/sbe/accumulator_sum_value_enum.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <tuple>
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
            "aggRemovableSumAdd", sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledRemovableSumAdd = compileAggExpression(*aggRemovableSumAdd, &aggAccessor);

        auto aggRemovableSumRemove = sbe::makeE<sbe::EFunction>(
            "aggRemovableSumRemove", sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledRemovableSumRemove =
            compileAggExpression(*aggRemovableSumRemove, &aggAccessor);

        auto aggRemovableSumFinalize = sbe::makeE<sbe::EFunction>(
            "aggRemovableSumFinalize", sbe::makeEs(makeE<EVariable>(aggSlot)));
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
