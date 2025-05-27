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

enum class Op { kAdd, kRemove };

class SBEMinMaxTest : public EExpressionTestFixture {
public:
    template <bool Collation>
    void runAndAssertExpression(bool isMin,
                                std::vector<std::pair<value::TypeTags, value::Value>>& inputValues,
                                std::vector<Op>& operations,
                                std::vector<std::pair<value::TypeTags, value::Value>>& expValues) {
        value::ViewOfValueAccessor inputAccessor;
        auto inputSlot = bindAccessor(&inputAccessor);

        value::OwnedValueAccessor aggAccessor;
        auto aggSlot = bindAccessor(&aggAccessor);

        auto exprNamePrefix = std::string{"aggRemovableMinMaxN"};

        auto cap = internalQueryTopNAccumulatorBytes.load();

        auto initExpr = [&]() {
            if constexpr (Collation) {
                return sbe::makeE<sbe::EFunction>(
                    "aggRemovableMinMaxNCollInit",
                    sbe::makeEs(
                        makeE<EConstant>(value::TypeTags::NumberInt64, 1),
                        makeE<EConstant>(value::TypeTags::NumberInt32, cap),
                        makeE<EConstant>(
                            value::TypeTags::collator,
                            value::bitcastFrom<CollatorInterface*>(new CollatorInterfaceMock(
                                CollatorInterfaceMock::MockType::kReverseString)))));
            } else {
                return sbe::makeE<sbe::EFunction>(
                    "aggRemovableMinMaxNInit",
                    sbe::makeEs(makeE<EConstant>(value::TypeTags::NumberInt64, 1),
                                makeE<EConstant>(value::TypeTags::NumberInt32, cap)));
            }
        }();
        auto compiledInit = compileExpression(*initExpr);

        auto addExpr = sbe::makeE<sbe::EFunction>(exprNamePrefix + "Add",
                                                  sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledAdd = compileAggExpression(*addExpr, &aggAccessor);

        auto removeExpr = sbe::makeE<sbe::EFunction>(exprNamePrefix + "Remove",
                                                     sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledRemove = compileAggExpression(*removeExpr, &aggAccessor);

        auto finalizeExpr = sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::fillEmpty,
            sbe::makeE<sbe::EFunction>(
                "getElement",
                sbe::makeEs(sbe::makeE<sbe::EFunction>(
                                "aggRemovable" + std::string(isMin ? "MinN" : "MaxN") + "Finalize",
                                sbe::makeEs(makeE<EVariable>(aggSlot))),
                            makeE<EConstant>(value::TypeTags::NumberInt32, 0))),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0));
        auto compiledFinalize = compileExpression(*finalizeExpr);

        auto [stateTag, stateVal] = runCompiledExpression(compiledInit.get());
        aggAccessor.reset(stateTag, stateVal);

        // call Op (Add/Remove) on the inputs and call finalize() method after each Op
        size_t addIdx = 0, removeIdx = 0;
        for (size_t i = 0; i < operations.size(); ++i) {
            vm::CodeFragment* compiledExpr;
            size_t idx;
            if (operations[i] == Op::kAdd) {
                compiledExpr = compiledAdd.get();
                idx = addIdx++;
            } else {
                compiledExpr = compiledRemove.get();
                idx = removeIdx++;
            }
            inputAccessor.reset(inputValues[idx].first, inputValues[idx].second);
            auto [runTag, runVal] = runCompiledExpression(compiledExpr);

            aggAccessor.reset(runTag, runVal);
            auto [outTag, outVal] = runCompiledExpression(compiledFinalize.get());

            ASSERT_EQ(expValues[i].first, outTag);
            auto [compareTag, compareVal] =
                value::compareValue(expValues[i].first, expValues[i].second, outTag, outVal);
            ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
            ASSERT_EQ(value::bitcastTo<int32_t>(compareVal), 0);
            value::releaseValue(outTag, outVal);
            value::releaseValue(expValues[i].first, expValues[i].second);
        }
        for (size_t i = 0; i < inputValues.size(); ++i) {
            value::releaseValue(inputValues[i].first, inputValues[i].second);
        }
    }
};

TEST_F(SBEMinMaxTest, MinTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(3)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(4.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.0)}};

    std::vector<Op> operations = {Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(3)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(4.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.0)},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)}};

    runAndAssertExpression<false>(true, inputValues, operations, expValues);
}

TEST_F(SBEMinMaxTest, MaxTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(3)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(4.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.0)}};

    std::vector<Op> operations = {Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(3)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(4.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.0)},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)}};

    runAndAssertExpression<false>(false, inputValues, operations, expValues);
}

TEST_F(SBEMinMaxTest, MinStringTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("ev").second},
        {value::TypeTags::StringSmall, value::makeSmallString("dw").second},
        {value::TypeTags::StringSmall, value::makeSmallString("cx").second},
        {value::TypeTags::StringSmall, value::makeSmallString("by").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
    };

    std::vector<Op> operations = {Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("ev").second},
        {value::TypeTags::StringSmall, value::makeSmallString("dw").second},
        {value::TypeTags::StringSmall, value::makeSmallString("cx").second},
        {value::TypeTags::StringSmall, value::makeSmallString("by").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)}};

    runAndAssertExpression<false>(true, inputValues, operations, expValues);
}

TEST_F(SBEMinMaxTest, MaxStringTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("ev").second},
        {value::TypeTags::StringSmall, value::makeSmallString("dw").second},
        {value::TypeTags::StringSmall, value::makeSmallString("cx").second},
        {value::TypeTags::StringSmall, value::makeSmallString("by").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
    };

    std::vector<Op> operations = {Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("ev").second},
        {value::TypeTags::StringSmall, value::makeSmallString("dw").second},
        {value::TypeTags::StringSmall, value::makeSmallString("cx").second},
        {value::TypeTags::StringSmall, value::makeSmallString("by").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)}};

    runAndAssertExpression<false>(false, inputValues, operations, expValues);
}

TEST_F(SBEMinMaxTest, MinStringCollationTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("ev").second},
        {value::TypeTags::StringSmall, value::makeSmallString("dw").second},
        {value::TypeTags::StringSmall, value::makeSmallString("cx").second},
        {value::TypeTags::StringSmall, value::makeSmallString("by").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
    };

    std::vector<Op> operations = {Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("ev").second},
        {value::TypeTags::StringSmall, value::makeSmallString("dw").second},
        {value::TypeTags::StringSmall, value::makeSmallString("cx").second},
        {value::TypeTags::StringSmall, value::makeSmallString("by").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)}};

    runAndAssertExpression<true>(true, inputValues, operations, expValues);
}

TEST_F(SBEMinMaxTest, MaxStringCollationTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("ev").second},
        {value::TypeTags::StringSmall, value::makeSmallString("dw").second},
        {value::TypeTags::StringSmall, value::makeSmallString("cx").second},
        {value::TypeTags::StringSmall, value::makeSmallString("by").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
    };

    std::vector<Op> operations = {Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::StringSmall, value::makeSmallString("fu").second},
        {value::TypeTags::StringSmall, value::makeSmallString("ev").second},
        {value::TypeTags::StringSmall, value::makeSmallString("dw").second},
        {value::TypeTags::StringSmall, value::makeSmallString("cx").second},
        {value::TypeTags::StringSmall, value::makeSmallString("by").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
        {value::TypeTags::StringSmall, value::makeSmallString("az").second},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)}};

    runAndAssertExpression<true>(false, inputValues, operations, expValues);
}

TEST_F(SBEMinMaxTest, MinNullNothingTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::Nothing, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::Nothing, value::bitcastFrom<int32_t>(0)}};

    std::vector<Op> operations = {Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kRemove,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)}};

    runAndAssertExpression<false>(true, inputValues, operations, expValues);
}

TEST_F(SBEMinMaxTest, MaxNullNothingTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::Nothing, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::Nothing, value::bitcastFrom<int32_t>(0)}};

    std::vector<Op> operations = {Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kRemove,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)}};

    runAndAssertExpression<false>(false, inputValues, operations, expValues);
}

TEST_F(SBEMinMaxTest, MinMultipleTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)}};

    std::vector<Op> operations = {Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kRemove,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)}};

    runAndAssertExpression<false>(true, inputValues, operations, expValues);
}

TEST_F(SBEMinMaxTest, MaxMultipleTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)}};

    std::vector<Op> operations = {Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kRemove,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kAdd,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove,
                                  Op::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)}};

    runAndAssertExpression<false>(false, inputValues, operations, expValues);
}

TEST_F(SBEMinMaxTest, MinMaxCapTest) {
    auto cap = internalQueryTopNAccumulatorBytes.load();

    auto initExpr = sbe::makeE<sbe::EFunction>(
        "aggRemovableMinMaxNInit",
        sbe::makeEs(makeE<EConstant>(value::TypeTags::NumberInt32, 1),
                    makeE<EConstant>(value::TypeTags::NumberDouble, cap)));
    auto compiledInit = compileExpression(*initExpr);

    Status status = [&]() {
        try {
            auto [stateTag, stateVal] = runCompiledExpression(compiledInit.get());
            value::ValueGuard guard{stateTag, stateVal};
            return Status::OK();
        } catch (AssertionException& ex) {
            return ex.toStatus();
        }
    }();
    ASSERT_FALSE(status.isOK());
    ASSERT_EQ(status.code(), 8178109);
}

}  // namespace mongo::sbe
