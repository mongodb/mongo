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

enum class RemovableOp { kAdd, kRemove };

class SBERemovableAvgTest : public EExpressionTestFixture {
public:
    void assertValues(std::pair<value::TypeTags, value::Value>& exprected,
                      std::pair<value::TypeTags, value::Value>& result) {
        ASSERT_EQ(exprected.first, result.first);
        if (result.first == value::TypeTags::NumberDecimal) {
            ASSERT(value::bitcastTo<Decimal128>(exprected.second)
                       .subtract(value::bitcastTo<Decimal128>(result.second))
                       .toAbs()
                       .isLess(Decimal128{"0.00001"}));
        } else {
            if (std::isnan(value::bitcastTo<double>(exprected.second)) ||
                std::isinf(value::bitcastTo<double>(exprected.second))) {
                auto [compareTag, compareVal] = value::compareValue(
                    exprected.first, exprected.second, result.first, result.second);
                ASSERT_EQ(compareVal, 0);
            } else {
                ASSERT(std::abs(value::bitcastTo<double>(exprected.second) -
                                value::bitcastTo<double>(result.second)) < 0.001e25);
            }
        }
    }

    void runAndAssertExpression(std::vector<std::pair<value::TypeTags, value::Value>>& inputValues,
                                std::vector<RemovableOp>& operations,
                                std::vector<std::pair<value::TypeTags, value::Value>>& expValues) {
        value::ViewOfValueAccessor inputAccessor;
        auto inputSlot = bindAccessor(&inputAccessor);

        value::OwnedValueAccessor sumAccessor;
        auto sumSlot = bindAccessor(&sumAccessor);

        value::OwnedValueAccessor countAccessor;
        auto countSlot = bindAccessor(&countAccessor);

        auto aggRemovableSumAdd = sbe::makeE<sbe::EFunction>(
            EFn::kAggRemovableSumAdd, sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledRemovableSumAdd = compileAggExpression(*aggRemovableSumAdd, &sumAccessor);

        auto aggRemovableSumRemove = sbe::makeE<sbe::EFunction>(
            EFn::kAggRemovableSumRemove, sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledRemovableSumRemove =
            compileAggExpression(*aggRemovableSumRemove, &sumAccessor);

        auto aggRemovableAvgFinalize = sbe::makeE<sbe::EFunction>(
            EFn::kAggRemovableAvgFinalize,
            sbe::makeEs(makeE<EVariable>(sumSlot), makeE<EVariable>(countSlot)));
        auto compiledRemovableAvgFinalize = compileExpression(*aggRemovableAvgFinalize);

        auto isNullOrMissingExpr = sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::fillEmpty,
            sbe::makeE<sbe::EFunction>(
                EFn::kTypeMatch,
                sbe::makeEs(sbe::makeE<EVariable>(inputSlot),
                            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                       getBSONTypeMask(BSONType::null) |
                                                           getBSONTypeMask(BSONType::undefined)))),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, true));

        auto isNotNumberExpr = sbe::makeE<sbe::EPrimUnary>(
            sbe::EPrimUnary::logicNot,
            sbe::makeE<sbe::EFunction>(EFn::kIsNumber, sbe::makeEs(makeE<EVariable>(inputSlot))));

        auto aggRemovableCountAdd = sbe::makeE<sbe::EIf>(
            sbe::makeE<sbe::EPrimBinary>(
                sbe::EPrimBinary::logicOr, isNullOrMissingExpr->clone(), isNotNumberExpr->clone()),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, 0),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, 1));
        auto compiledRemovableCountAdd =
            compileAggExpression(*aggRemovableCountAdd, &countAccessor);

        auto aggRemovableCountRemove = sbe::makeE<sbe::EIf>(
            sbe::makeE<sbe::EPrimBinary>(
                sbe::EPrimBinary::logicOr, isNullOrMissingExpr->clone(), isNotNumberExpr->clone()),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, 0),
            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, -1));
        auto compiledRemovableCountRemove =
            compileAggExpression(*aggRemovableCountRemove, &countAccessor);

        // call RemovableOp (Add/Remove) on the inputs and call finalize() method after each op
        size_t addIdx = 0, removeIdx = 0;
        size_t totalCount = 0;
        for (size_t i = 0; i < operations.size(); ++i) {
            vm::CodeFragment* sumCompiledExpr;
            vm::CodeFragment* countCompiledExpr;
            size_t idx;
            if (operations[i] == RemovableOp::kAdd) {
                sumCompiledExpr = compiledRemovableSumAdd.get();
                countCompiledExpr = compiledRemovableCountAdd.get();
                idx = addIdx++;
            } else {
                sumCompiledExpr = compiledRemovableSumRemove.get();
                countCompiledExpr = compiledRemovableCountRemove.get();
                idx = removeIdx++;
            }

            inputAccessor.reset(inputValues[idx].first, inputValues[idx].second);
            auto [sumTag, sumVal] = runCompiledExpression(sumCompiledExpr);
            auto [countTag, countVal] = runCompiledExpression(countCompiledExpr);

            sumAccessor.reset(sumTag, sumVal);
            totalCount += countVal;
            countAccessor.reset(countTag, totalCount);
            auto result = runCompiledExpression(compiledRemovableAvgFinalize.get());

            assertValues(expValues[i], result);

            value::releaseValue(result.first, result.second);
            value::releaseValue(expValues[i].first, expValues[i].second);
        }
        for (size_t i = 0; i < inputValues.size(); ++i) {
            value::releaseValue(inputValues[i].first, inputValues[i].second);
        }
    }
};

TEST_F(SBERemovableAvgTest, SumWithMixedTypes1) {  // int, long, double, decimal
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

    std::vector<RemovableOp> removableOps = {RemovableOp::kAdd,
                                             RemovableOp::kAdd,
                                             RemovableOp::kAdd,
                                             RemovableOp::kAdd,
                                             RemovableOp::kAdd,
                                             RemovableOp::kAdd,
                                             RemovableOp::kAdd,
                                             RemovableOp::kAdd,
                                             RemovableOp::kRemove,
                                             RemovableOp::kRemove,
                                             RemovableOp::kRemove,
                                             RemovableOp::kRemove,
                                             RemovableOp::kRemove,
                                             RemovableOp::kRemove,
                                             RemovableOp::kRemove,
                                             RemovableOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-10.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-10.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-10.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-10.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-10.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-10.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-10.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-10.0}).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)},
    };

    runAndAssertExpression(inputValues, removableOps, expValues);
}

TEST_F(SBERemovableAvgTest, SumWithMixedTypes2) {  // int, long, double
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-10)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-10ll)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::Boolean, value::bitcastFrom<bool>(true)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-10LL)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-10)},
    };

    std::vector<RemovableOp> removableOps = {RemovableOp::kAdd,
                                             RemovableOp::kAdd,
                                             RemovableOp::kAdd,
                                             RemovableOp::kAdd,
                                             RemovableOp::kAdd,
                                             RemovableOp::kAdd,
                                             RemovableOp::kRemove,
                                             RemovableOp::kRemove,
                                             RemovableOp::kRemove,
                                             RemovableOp::kRemove,
                                             RemovableOp::kRemove,
                                             RemovableOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)},
    };

    runAndAssertExpression(inputValues, removableOps, expValues);
}

TEST_F(SBERemovableAvgTest, SumWithMixedTypes3) {  // int, long
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2147483647)},
        {value::TypeTags::NumberInt64,
         value::bitcastFrom<int64_t>(std::numeric_limits<int64_t>::max())},
    };

    std::vector<RemovableOp> removableOps = {RemovableOp::kAdd,
                                             RemovableOp::kAdd,
                                             RemovableOp::kRemove,
                                             RemovableOp::kAdd,
                                             RemovableOp::kRemove,
                                             RemovableOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(1)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(1073741824)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2147483647)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(4.61169e+18L)},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(static_cast<double>(std::numeric_limits<int64_t>::max()))},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)},
    };

    runAndAssertExpression(inputValues, removableOps, expValues);
}

TEST_F(SBERemovableAvgTest, SumWithOverflow) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32,
         value::bitcastFrom<int32_t>(std::numeric_limits<int32_t>::min())},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)},
        {value::TypeTags::NumberInt64,
         value::bitcastFrom<int64_t>(std::numeric_limits<int64_t>::min())},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)},
    };

    std::vector<RemovableOp> removableSumOps = {RemovableOp::kAdd,
                                                RemovableOp::kAdd,
                                                RemovableOp::kRemove,
                                                RemovableOp::kRemove,
                                                RemovableOp::kAdd,
                                                RemovableOp::kAdd,
                                                RemovableOp::kRemove,
                                                RemovableOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(static_cast<double>(std::numeric_limits<int32_t>::min()))},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(
             static_cast<double>(static_cast<long long>(std::numeric_limits<int32_t>::min()) - 1) /
             2.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-1.0)},
        {value::TypeTags::Null, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(static_cast<double>(std::numeric_limits<int64_t>::min()))},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(static_cast<double>(std::numeric_limits<int64_t>::min()) / 2.0 -
                                    1.0 / 2.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-1.0)},
        {value::TypeTags::Null, value::bitcastFrom<double>(0)},
    };

    runAndAssertExpression(inputValues, removableSumOps, expValues);
}

TEST_F(SBERemovableAvgTest, SumWithNaNAndInfinityValues) {
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

    std::vector<RemovableOp> removableSumOps = {RemovableOp::kAdd,
                                                RemovableOp::kAdd,
                                                RemovableOp::kAdd,
                                                RemovableOp::kAdd,
                                                RemovableOp::kAdd,
                                                RemovableOp::kRemove,
                                                RemovableOp::kRemove,
                                                RemovableOp::kRemove,
                                                RemovableOp::kRemove,
                                                RemovableOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(10.0)},
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
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(20.0)},
        {value::TypeTags::Null, value::bitcastFrom<double>(0.0)},
    };

    runAndAssertExpression(inputValues, removableSumOps, expValues);
}

}  // namespace mongo::sbe
