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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <tuple>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/accumulator_sum_value_enum.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

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

        auto aggStdDevAdd = sbe::makeE<sbe::EFunction>("aggRemovableStdDevAdd",
                                                       sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledStdDevAdd = compileAggExpression(*aggStdDevAdd, &aggAccessor);

        auto aggCovarianceRemove = sbe::makeE<sbe::EFunction>(
            "aggRemovableStdDevRemove", sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledStdDevRemove = compileAggExpression(*aggCovarianceRemove, &aggAccessor);

        auto aggStdDevFinalizeSamp = sbe::makeE<sbe::EFunction>(
            "aggRemovableStdDevSampFinalize", sbe::makeEs(makeE<EVariable>(aggSlot)));
        auto compiledStdDevFinalizeSamp = compileExpression(*aggStdDevFinalizeSamp);

        auto aggStdDevFinalizePop = sbe::makeE<sbe::EFunction>(
            "aggRemovableStdDevPopFinalize", sbe::makeEs(makeE<EVariable>(aggSlot)));
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
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(22.1454)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(28.4634)},
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
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(18.0816)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(20.1267)},
        {value::TypeTags::NumberInt32, 0},
        {value::TypeTags::Null, 0},
    };

    runAndAssertExpression(inputValues, stdDevOps, expValuesSamp, expValuesPop);
}
}  // namespace mongo::sbe
