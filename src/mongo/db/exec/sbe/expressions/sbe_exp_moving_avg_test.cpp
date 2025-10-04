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

#include "mongo/db/exec/sbe/expression_test_base.h"
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

        auto expMovingAvgExpr =
            sbe::makeE<sbe::EFunction>("aggExpMovingAvg", sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledExpMovingAvg = compileAggExpression(*expMovingAvgExpr, &aggAccessor);

        auto expMovingAvgExprFinalize = sbe::makeE<sbe::EFunction>(
            "aggExpMovingAvgFinalize", sbe::makeEs(makeE<EVariable>(aggSlot)));
        auto compiledExpMovingAvgFinalize = compileExpression(*expMovingAvgExprFinalize);

        auto [stateArrayTag, stateArrayVal] = value::makeNewArray();
        auto stateArray = value::getArrayView(stateArrayVal);
        stateArray->push_back(value::TypeTags::Null, 0);
        stateArray->push_back(value::TypeTags::NumberDecimal,
                              value::makeCopyDecimal(Decimal128{alpha}).second);
        stateArray->push_back(value::TypeTags::Boolean, value::bitcastFrom<bool>(false));

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
