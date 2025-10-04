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

enum class IntegralOp { kAdd, kRemove };

class SBEIntegralTest : public EExpressionTestFixture {
public:
    std::pair<value::TypeTags, value::Value> initQueue() {
        auto [queueTag, queueVal] = value::makeNewArray();
        auto queue = value::getArrayView(queueVal);
        auto [queueInternalArrTag, queueInternalArrVal] = value::makeNewArray();
        auto arr = value::getArrayView(queueInternalArrVal);
        arr->push_back(value::TypeTags::Null, 0);
        queue->push_back(queueInternalArrTag, queueInternalArrVal);
        queue->push_back(value::TypeTags::NumberInt64, 0);
        queue->push_back(value::TypeTags::NumberInt64, 0);
        return {queueTag, queueVal};
    }

    std::pair<value::TypeTags, value::Value> initState(boost::optional<int64_t> unitMillis,
                                                       bool isNonRemovable) {
        auto [stateTag, stateVal] = value::makeNewArray();
        auto state = value::getArrayView(stateVal);

        // input queue
        auto [inputQueueTag, inputQueueVal] = initQueue();
        state->push_back(inputQueueTag, inputQueueVal);

        // sortBy queue
        auto [sortByQueueTag, sortByQueueVal] = initQueue();
        state->push_back(sortByQueueTag, sortByQueueVal);

        // sum acc state
        auto [removableSumAccTag, removableSumAccVal] = value::makeNewArray();
        auto removableSumAcc = value::getArrayView(removableSumAccVal);
        auto [sumAccTag, sumAccVal] = value::makeNewArray();
        auto sumAcc = value::getArrayView(sumAccVal);
        // DoubleDoubleSum Acc
        sumAcc->reserve(AggSumValueElems::kMaxSizeOfArray);
        sumAcc->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0));
        sumAcc->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0));
        sumAcc->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0));
        // RemovableSum Acc
        removableSumAcc->push_back(sumAccTag, sumAccVal);
        removableSumAcc->push_back(value::TypeTags::NumberInt64, 0);
        removableSumAcc->push_back(value::TypeTags::NumberInt64, 0);
        removableSumAcc->push_back(value::TypeTags::NumberInt64, 0);
        removableSumAcc->push_back(value::TypeTags::NumberInt64, 0);
        removableSumAcc->push_back(value::TypeTags::NumberInt64, 0);
        state->push_back(removableSumAccTag, removableSumAccVal);

        // nanCount
        state->push_back(value::TypeTags::NumberInt64, 0);

        // unitMillis
        if (unitMillis) {
            state->push_back(value::TypeTags::NumberInt64,
                             value::bitcastFrom<int64_t>(*unitMillis));
        } else {
            state->push_back(value::TypeTags::Null, 0);
        }

        // isNonRemovable
        state->push_back(value::TypeTags::Boolean, value::bitcastFrom<bool>(isNonRemovable));

        return {stateTag, stateVal};
    }

    void runAndAssertExpression(boost::optional<int64_t> unitMillis,
                                std::vector<std::pair<value::TypeTags, value::Value>>& inputValues,
                                std::vector<std::pair<value::TypeTags, value::Value>>& sortByValues,
                                std::vector<IntegralOp>& operations,
                                std::vector<std::pair<value::TypeTags, value::Value>>& expValues,
                                bool isNonRemovable = false) {
        value::ViewOfValueAccessor inputAccessor;
        auto inputSlot = bindAccessor(&inputAccessor);

        value::ViewOfValueAccessor sortByAccessor;
        auto sortBySlot = bindAccessor(&sortByAccessor);

        value::OwnedValueAccessor aggAccessor;
        auto aggSlot = bindAccessor(&aggAccessor);

        auto aggIntegralAddExpr = sbe::makeE<sbe::EFunction>(
            "aggIntegralAdd",
            sbe::makeEs(makeE<EVariable>(inputSlot), makeE<EVariable>(sortBySlot)));
        auto compiledIntegralAdd = compileAggExpression(*aggIntegralAddExpr, &aggAccessor);

        auto aggIntegralRemoveExpr = sbe::makeE<sbe::EFunction>(
            "aggIntegralRemove",
            sbe::makeEs(makeE<EVariable>(inputSlot), makeE<EVariable>(sortBySlot)));
        auto compiledIntegralRemove = compileAggExpression(*aggIntegralRemoveExpr, &aggAccessor);

        auto aggIntegralFinalize = sbe::makeE<sbe::EFunction>(
            "aggIntegralFinalize", sbe::makeEs(makeE<EVariable>(aggSlot)));
        auto compiledIntegralFinalize = compileExpression(*aggIntegralFinalize);

        auto [stateTag, stateVal] = initState(unitMillis, isNonRemovable);
        aggAccessor.reset(stateTag, stateVal);

        // call IntegralOp (integralAdd/Remove) on the inputs and call finalize() method after each
        // IntegralOp
        size_t addIdx = 0, removeIdx = 0;
        for (size_t i = 0; i < operations.size(); ++i) {
            vm::CodeFragment* compiledExpr;
            size_t idx;
            if (operations[i] == IntegralOp::kAdd) {
                compiledExpr = compiledIntegralAdd.get();
                idx = addIdx++;
            } else {
                compiledExpr = compiledIntegralRemove.get();
                idx = removeIdx++;
            }
            inputAccessor.reset(inputValues[idx].first, inputValues[idx].second);
            sortByAccessor.reset(sortByValues[idx].first, sortByValues[idx].second);
            auto [runTag, runVal] = runCompiledExpression(compiledExpr);

            aggAccessor.reset(runTag, runVal);
            auto [outTag, outVal] = runCompiledExpression(compiledIntegralFinalize.get());

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
            value::releaseValue(sortByValues[i].first, sortByValues[i].second);
        }
    }

    void runAndAssertErrorCode(boost::optional<int64_t> unitMillis,
                               std::vector<std::pair<value::TypeTags, value::Value>>& inputValues,
                               std::vector<std::pair<value::TypeTags, value::Value>>& sortByValues,
                               std::vector<IntegralOp>& operations,
                               int expErrCode,
                               bool isNonRemovable = false) {
        value::ViewOfValueAccessor inputAccessor;
        auto inputSlot = bindAccessor(&inputAccessor);

        value::ViewOfValueAccessor sortByAccessor;
        auto sortBySlot = bindAccessor(&sortByAccessor);

        value::OwnedValueAccessor aggAccessor;
        auto aggSlot = bindAccessor(&aggAccessor);

        auto aggIntegralAddExpr = sbe::makeE<sbe::EFunction>(
            "aggIntegralAdd",
            sbe::makeEs(makeE<EVariable>(inputSlot), makeE<EVariable>(sortBySlot)));
        auto compiledIntegralAdd = compileAggExpression(*aggIntegralAddExpr, &aggAccessor);

        auto aggIntegralRemoveExpr = sbe::makeE<sbe::EFunction>(
            "aggIntegralRemove",
            sbe::makeEs(makeE<EVariable>(inputSlot), makeE<EVariable>(sortBySlot)));
        auto compiledIntegralRemove = compileAggExpression(*aggIntegralRemoveExpr, &aggAccessor);

        auto aggIntegralFinalize = sbe::makeE<sbe::EFunction>(
            "aggIntegralFinalize", sbe::makeEs(makeE<EVariable>(aggSlot)));
        auto compiledIntegralFinalize = compileExpression(*aggIntegralFinalize);

        auto [stateTag, stateVal] = initState(unitMillis, isNonRemovable);
        aggAccessor.reset(stateTag, stateVal);

        Status status = [&]() {
            try {
                size_t addIdx = 0, removeIdx = 0;
                for (size_t i = 0; i < operations.size(); ++i) {
                    vm::CodeFragment* compiledExpr;
                    size_t idx;
                    if (operations[i] == IntegralOp::kAdd) {
                        compiledExpr = compiledIntegralAdd.get();
                        idx = addIdx++;
                    } else {
                        compiledExpr = compiledIntegralRemove.get();
                        idx = removeIdx++;
                    }
                    inputAccessor.reset(inputValues[idx].first, inputValues[idx].second);
                    sortByAccessor.reset(sortByValues[idx].first, sortByValues[idx].second);
                    auto [runTag, runVal] = runCompiledExpression(compiledExpr);
                    aggAccessor.reset(runTag, runVal);
                }
                return Status::OK();
            } catch (AssertionException& ex) {
                return ex.toStatus();
            }
        }();
        ASSERT_FALSE(status.isOK());
        ASSERT_EQ(status.code(), expErrCode);
        for (size_t i = 0; i < inputValues.size(); ++i) {
            value::releaseValue(inputValues[i].first, inputValues[i].second);
            value::releaseValue(sortByValues[i].first, sortByValues[i].second);
        }
    }
};

TEST_F(SBEIntegralTest, IntegralAddRemoveOverDate) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.95)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.7)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.6)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.98)}};
    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::Date, 1589811030000LL},
        {value::TypeTags::Date, 1589811060000LL},
        {value::TypeTags::Date, 1589811090000LL},
        {value::TypeTags::Date, 1589811120000LL}};

    std::vector<IntegralOp> integralOps = {IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kRemove,
                                           IntegralOp::kRemove,
                                           IntegralOp::kRemove,
                                           IntegralOp::kRemove};
    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.023541666666666666)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.045625)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.068875)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.045333333333333337)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.02325)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(0)},
        {value::TypeTags::Null, 0}};

    boost::optional<int64_t> unitMillis = 60LL * 60LL * 1000LL;  // hour unit
    runAndAssertExpression(unitMillis, inputValues, sortByValues, integralOps, expValues);
}

TEST_F(SBEIntegralTest, IntegralWithMixedTypes) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(10)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10ll)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(10.0)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{10.0}).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(10.0)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10ll)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(10)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2l)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{4.0}).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.0)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(6ll)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(7)},
    };

    std::vector<IntegralOp> integralOps = {IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kRemove,
                                           IntegralOp::kRemove,
                                           IntegralOp::kRemove,
                                           IntegralOp::kRemove,
                                           IntegralOp::kRemove,
                                           IntegralOp::kRemove,
                                           IntegralOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberInt32, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(20.0)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{30.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{40.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{50.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{60.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{50.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{40.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{30.0}).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(20.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(10.0)},
        {value::TypeTags::NumberInt32, 0},
        {value::TypeTags::Null, 0},
    };

    runAndAssertExpression(boost::none, inputValues, sortByValues, integralOps, expValues);
}

TEST_F(SBEIntegralTest, IntegralWithMixedTypesNonRemovable) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(10)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10ll)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(10.0)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{10.0}).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(10.0)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10ll)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(10)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2l)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{4.0}).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.0)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(6ll)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(7)},
    };

    std::vector<IntegralOp> integralOps = {IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberInt32, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(20.0)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{30.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{40.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{50.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{60.0}).second},
    };

    runAndAssertExpression(boost::none, inputValues, sortByValues, integralOps, expValues, true);
}

TEST_F(SBEIntegralTest, IntegralWithNaNAndInfinityValues) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt64, 10},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128::kNegativeNaN).second},
        {value::TypeTags::NumberInt64, 20},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::infinity())},
        {value::TypeTags::NumberDecimal,
         value::makeCopyDecimal(Decimal128::kNegativeInfinity).second},
        {value::TypeTags::NumberInt64, 30},
        {value::TypeTags::NumberInt64, 40},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(3)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(4)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(5)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(6)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(7)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(8)},
    };

    std::vector<IntegralOp> integralOps = {IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kAdd,
                                           IntegralOp::kRemove,
                                           IntegralOp::kRemove,
                                           IntegralOp::kRemove,
                                           IntegralOp::kRemove,
                                           IntegralOp::kRemove,
                                           IntegralOp::kRemove,
                                           IntegralOp::kRemove,
                                           IntegralOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberInt32, 0},
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
         value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128::kPositiveNaN).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128::kPositiveNaN).second},
        {value::TypeTags::NumberDecimal,
         value::makeCopyDecimal(Decimal128::kNegativeInfinity).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(35.0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::Null, 0},
    };

    runAndAssertExpression(boost::none, inputValues, sortByValues, integralOps, expValues);
}

TEST_F(SBEIntegralTest, IntegralWithDatesAndNoUnit) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.95)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.98)}};
    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::Date, 1589811030000LL}, {value::TypeTags::Date, 1589811060000LL}};

    std::vector<IntegralOp> integralOps = {
        IntegralOp::kAdd, IntegralOp::kAdd, IntegralOp::kRemove, IntegralOp::kRemove};

    runAndAssertErrorCode(boost::none, inputValues, sortByValues, integralOps, 7821111);
}

TEST_F(SBEIntegralTest, IntegralWithNumbersAndUnit) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(10)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10ll)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2l)},
    };

    std::vector<IntegralOp> integralOps = {
        IntegralOp::kAdd, IntegralOp::kAdd, IntegralOp::kRemove, IntegralOp::kRemove};

    boost::optional<int64_t> unitMillis = 60LL * 60LL * 1000LL;
    runAndAssertErrorCode(unitMillis, inputValues, sortByValues, integralOps, 7821110);
}

TEST_F(SBEIntegralTest, IntegralWithIncorrectTypes1) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::StringSmall, value::makeSmallString("a").second},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
    };

    std::vector<IntegralOp> integralOps = {IntegralOp::kAdd};

    runAndAssertErrorCode(boost::none, inputValues, sortByValues, integralOps, 7821109);
}

TEST_F(SBEIntegralTest, IntegralWithIncorrectTypes2) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::StringSmall, value::makeSmallString("a").second},
    };

    std::vector<IntegralOp> integralOps = {IntegralOp::kAdd};

    runAndAssertErrorCode(boost::none, inputValues, sortByValues, integralOps, 7821111);
}

TEST_F(SBEIntegralTest, IntegralRemoveWithNonRemovable) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(10)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10ll)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2l)},
    };

    std::vector<IntegralOp> integralOps = {
        IntegralOp::kAdd, IntegralOp::kAdd, IntegralOp::kRemove, IntegralOp::kRemove};

    runAndAssertErrorCode(boost::none, inputValues, sortByValues, integralOps, 7996801, true);
}
}  // namespace mongo::sbe
