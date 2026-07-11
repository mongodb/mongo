// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/platform/decimal128.h"

namespace mongo::sbe {

class SBEExpMovingAvgTest : public EExpressionTestFixture {
public:
    void runAndAssertExpression(double alpha,
                                std::vector<std::pair<value::TypeTags, value::Value>>& inputs,
                                std::vector<std::pair<value::TypeTags, value::Value>>& expValues) {
        value::OwnedValueAccessor inputAccessor;
        auto inputSlot = bindAccessor(&inputAccessor);

        value::OwnedValueAccessor aggAccessor;
        auto aggSlot = bindAccessor(&aggAccessor);

        auto expMovingAvgExpr = sbe::makeE<sbe::EFunction>(
            EFn::kAggExpMovingAvg, sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledExpMovingAvg = compileAggExpression(*expMovingAvgExpr, &aggAccessor);

        auto expMovingAvgExprFinalize = sbe::makeE<sbe::EFunction>(
            EFn::kAggExpMovingAvgFinalize, sbe::makeEs(makeE<EVariable>(aggSlot)));
        auto compiledExpMovingAvgFinalize = compileExpression(*expMovingAvgExprFinalize);

        auto [stateArrayTag, stateArrayVal] = value::makeNewArray();
        auto stateArray = value::getArrayView(stateArrayVal);
        stateArray->push_back_raw(value::TypeTags::Null, 0);
        stateArray->push_back_raw(value::TypeTags::NumberDecimal,
                                  value::makeCopyDecimal(Decimal128{alpha}).second);
        stateArray->push_back_raw(value::TypeTags::Boolean, value::bitcastFrom<bool>(false));

        aggAccessor.reset(stateArrayTag, stateArrayVal);

        for (size_t i = 0; i < inputs.size(); ++i) {
            inputAccessor.reset(inputs[i].first, inputs[i].second);
            auto [runTag, runVal] = runCompiledExpression(compiledExpMovingAvg.get());

            aggAccessor.reset(runTag, runVal);
            auto [emaTag, emaVal] = runCompiledExpression(compiledExpMovingAvgFinalize.get());

            ASSERT_EQ(expValues[i].first, emaTag);
            auto [compareTag, compareVal] =
                value::compareValue(expValues[i].first, expValues[i].second, emaTag, emaVal);
            ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
            ASSERT_EQ(value::bitcastTo<int32_t>(compareVal), 0);
            value::releaseValue(emaTag, emaVal);
            value::releaseValue(expValues[i].first, expValues[i].second);
        }
    }
};

TEST_F(SBEExpMovingAvgTest, ExpMovingAvgTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputs;
    std::vector<std::pair<value::TypeTags, value::Value>> expValues;

    inputs.push_back({value::TypeTags::Null, 0});
    expValues.push_back({value::TypeTags::Null, 0});

    inputs.push_back({value::TypeTags::NumberInt64, 13});
    expValues.push_back({value::TypeTags::NumberDouble, value::bitcastFrom<double>(13.0)});

    inputs.push_back({value::TypeTags::NumberDouble, value::bitcastFrom<double>(15.4)});
    expValues.push_back({value::TypeTags::NumberDouble, value::bitcastFrom<double>(14.8)});

    inputs.push_back({value::TypeTags::Null, 0});
    expValues.push_back({value::TypeTags::NumberDouble, value::bitcastFrom<double>(14.8)});

    inputs.push_back({value::TypeTags::NumberInt32, 12});
    expValues.push_back({value::TypeTags::NumberDouble, value::bitcastFrom<double>(12.7)});

    inputs.push_back(
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{11.7}).second});
    expValues.push_back(
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{11.95}).second});

    inputs.push_back({value::TypeTags::NumberInt64, 82});
    expValues.push_back(
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{64.4875}).second});

    runAndAssertExpression(0.75, inputs, expValues);
}
}  // namespace mongo::sbe
