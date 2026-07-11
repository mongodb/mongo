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

enum class StdDevOp { kAdd, kRemove };

class SBERemovableStdDevTest : public EExpressionTestFixture {
public:
    void runAndAssertExpression(
        std::vector<std::pair<value::TypeTags, value::Value>>& inputValues,
        std::vector<StdDevOp>& operations,
        std::vector<std::pair<value::TypeTags, value::Value>>& expValuesSamp,
        std::vector<std::pair<value::TypeTags, value::Value>>& expValuesPop) {
        value::ViewOfValueAccessor inputAccessor;
        auto inputSlot = bindAccessor(&inputAccessor);

        value::OwnedValueAccessor aggAccessor;
        auto aggSlot = bindAccessor(&aggAccessor);

        auto aggStdDevAdd = sbe::makeE<sbe::EFunction>(EFn::kAggRemovableStdDevAdd,
                                                       sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledStdDevAdd = compileAggExpression(*aggStdDevAdd, &aggAccessor);

        auto aggCovarianceRemove = sbe::makeE<sbe::EFunction>(
            EFn::kAggRemovableStdDevRemove, sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledStdDevRemove = compileAggExpression(*aggCovarianceRemove, &aggAccessor);

        auto aggStdDevFinalizeSamp = sbe::makeE<sbe::EFunction>(
            EFn::kAggRemovableStdDevSampFinalize, sbe::makeEs(makeE<EVariable>(aggSlot)));
        auto compiledStdDevFinalizeSamp = compileExpression(*aggStdDevFinalizeSamp);

        auto aggStdDevFinalizePop = sbe::makeE<sbe::EFunction>(
            EFn::kAggRemovableStdDevPopFinalize, sbe::makeEs(makeE<EVariable>(aggSlot)));
        auto compiledStdDevFinalizePop = compileExpression(*aggStdDevFinalizePop);

        // call StdDevOp (Add/Remove) on the inputs and call finalize() method after each op
        size_t addIdx = 0, removeIdx = 0;
        for (size_t i = 0; i < operations.size(); ++i) {
            vm::CodeFragment* compiledExpr;
            size_t idx;
            if (operations[i] == StdDevOp::kAdd) {
                compiledExpr = compiledStdDevAdd.get();
                idx = addIdx++;
            } else {
                compiledExpr = compiledStdDevRemove.get();
                idx = removeIdx++;
            }
            inputAccessor.reset(inputValues[idx].first, inputValues[idx].second);
            auto [runTag, runVal] = runCompiledExpression(compiledExpr);

            aggAccessor.reset(runTag, runVal);
            auto outSamp = runCompiledExpression(compiledStdDevFinalizeSamp.get());
            auto outPop = runCompiledExpression(compiledStdDevFinalizePop.get());

            const double precisionLimit = 0.0001;
            ASSERT_THAT(outSamp, ValueRoughEq(expValuesSamp[i], precisionLimit));
            ASSERT_THAT(outPop, ValueRoughEq(expValuesPop[i], precisionLimit));

            value::releaseValue(outSamp.first, outSamp.second);
            value::releaseValue(outPop.first, outPop.second);
            value::releaseValue(expValuesSamp[i].first, expValuesSamp[i].second);
            value::releaseValue(expValuesPop[i].first, expValuesPop[i].second);
        }
        for (size_t i = 0; i < inputValues.size(); ++i) {
            value::releaseValue(inputValues[i].first, inputValues[i].second);
        }
    }
};

TEST_F(SBERemovableStdDevTest, BasicTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(12)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(18)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(23)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(45)},
    };

    std::vector<StdDevOp> stdDevOps = {
        StdDevOp::kAdd,
        StdDevOp::kAdd,
        StdDevOp::kAdd,
        StdDevOp::kAdd,
        StdDevOp::kAdd,
        StdDevOp::kRemove,
        StdDevOp::kRemove,
        StdDevOp::kRemove,
        StdDevOp::kRemove,
        StdDevOp::kRemove,
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesSamp = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(4.9497)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.5064)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(7.7675)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(15.2086)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(14.3875)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(14.3643)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(15.5563)},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesPop = {
        {value::TypeTags::NumberInt32, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.5)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.3125)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.7268)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(13.6029)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(12.4599)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(11.7284)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(11)},
        {value::TypeTags::NumberInt32, 0},
        {value::TypeTags::Null, 0},
    };

    runAndAssertExpression(inputValues, stdDevOps, expValuesSamp, expValuesPop);
}

TEST_F(SBERemovableStdDevTest, MixedTypeTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(12)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128(18.0)).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(23.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(45.0)},
    };

    std::vector<StdDevOp> stdDevOps = {
        StdDevOp::kAdd,
        StdDevOp::kAdd,
        StdDevOp::kAdd,
        StdDevOp::kAdd,
        StdDevOp::kAdd,
        StdDevOp::kRemove,
        StdDevOp::kRemove,
        StdDevOp::kRemove,
        StdDevOp::kRemove,
        StdDevOp::kRemove,
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesSamp = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(4.9497)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.5064)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(7.7675)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(15.2086)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(14.3875)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(14.3643)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(15.5563)},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesPop = {
        {value::TypeTags::NumberInt32, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.5)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.3125)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.7268)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(13.6029)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(12.4599)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(11.7284)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(11)},
        {value::TypeTags::NumberInt32, 0},
        {value::TypeTags::Null, 0},
    };

    runAndAssertExpression(inputValues, stdDevOps, expValuesSamp, expValuesPop);
}

TEST_F(SBERemovableStdDevTest, NonNumericTypeTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(12)},
        {value::TypeTags::StringSmall, value::makeSmallString("a").second},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(18)},
    };

    std::vector<StdDevOp> stdDevOps = {
        StdDevOp::kAdd, StdDevOp::kAdd, StdDevOp::kAdd, StdDevOp::kAdd};

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesSamp = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(4.9497)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(4.9497)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.5064)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesPop = {
        {value::TypeTags::NumberInt32, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.5)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.5)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.3125)},
    };

    runAndAssertExpression(inputValues, stdDevOps, expValuesSamp, expValuesPop);
}

TEST_F(SBERemovableStdDevTest, InfTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(12)},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::infinity())},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(18)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(23)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(45)},
    };

    std::vector<StdDevOp> stdDevOps = {
        StdDevOp::kAdd,
        StdDevOp::kAdd,
        StdDevOp::kAdd,
        StdDevOp::kAdd,
        StdDevOp::kAdd,
        StdDevOp::kAdd,
        StdDevOp::kRemove,
        StdDevOp::kRemove,
        StdDevOp::kRemove,
        StdDevOp::kRemove,
        StdDevOp::kRemove,
        StdDevOp::kRemove,
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesSamp = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(4.9497)},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(14.3643)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(15.5563)},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesPop = {
        {value::TypeTags::NumberInt32, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.5)},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(11.7284)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(11)},
        {value::TypeTags::NumberInt32, 0},
        {value::TypeTags::Null, 0},
    };

    runAndAssertExpression(inputValues, stdDevOps, expValuesSamp, expValuesPop);
}
}  // namespace mongo::sbe
