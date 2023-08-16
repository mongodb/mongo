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

enum class CovarianceOp { kAdd, kRemove };

class SBECovarianceTest : public EExpressionTestFixture {
public:
    void runAndAssertExpression(
        std::vector<std::pair<value::TypeTags, value::Value>>& inputValuesX,
        std::vector<std::pair<value::TypeTags, value::Value>>& inputValuesY,
        std::vector<CovarianceOp>& operations,
        std::vector<std::pair<value::TypeTags, value::Value>>& expValuesSamp,
        std::vector<std::pair<value::TypeTags, value::Value>>& expValuesPop) {
        value::ViewOfValueAccessor inputAccessorX;
        auto inputSlotX = bindAccessor(&inputAccessorX);

        value::ViewOfValueAccessor inputAccessorY;
        auto inputSlotY = bindAccessor(&inputAccessorY);

        value::OwnedValueAccessor aggAccessor;
        auto aggSlot = bindAccessor(&aggAccessor);

        auto aggCovarianceAdd = sbe::makeE<sbe::EFunction>(
            "aggCovarianceAdd",
            sbe::makeEs(makeE<EVariable>(inputSlotX), makeE<EVariable>(inputSlotY)));
        auto compiledCovarianceAdd = compileAggExpression(*aggCovarianceAdd, &aggAccessor);

        auto aggCovarianceRemove = sbe::makeE<sbe::EFunction>(
            "aggCovarianceRemove",
            sbe::makeEs(makeE<EVariable>(inputSlotX), makeE<EVariable>(inputSlotY)));
        auto compiledCovarianceRemove = compileAggExpression(*aggCovarianceRemove, &aggAccessor);

        auto aggCovarianceFinalizeSamp = sbe::makeE<sbe::EFunction>(
            "aggCovarianceSampFinalize", sbe::makeEs(makeE<EVariable>(aggSlot)));
        auto compiledCovarianceFinalizeSamp = compileExpression(*aggCovarianceFinalizeSamp);

        auto aggCovarianceFinalizePop = sbe::makeE<sbe::EFunction>(
            "aggCovariancePopFinalize", sbe::makeEs(makeE<EVariable>(aggSlot)));
        auto compiledCovarianceFinalizePop = compileExpression(*aggCovarianceFinalizePop);

        // call CovarianceOp (Add/Remove) on the inputs and call finalize() method after each op
        size_t addIdx = 0, removeIdx = 0;
        for (size_t i = 0; i < operations.size(); ++i) {
            vm::CodeFragment* compiledExpr;
            size_t idx;
            if (operations[i] == CovarianceOp::kAdd) {
                compiledExpr = compiledCovarianceAdd.get();
                idx = addIdx++;
            } else {
                compiledExpr = compiledCovarianceRemove.get();
                idx = removeIdx++;
            }
            inputAccessorX.reset(inputValuesX[idx].first, inputValuesX[idx].second);
            inputAccessorY.reset(inputValuesY[idx].first, inputValuesY[idx].second);
            auto [runTag, runVal] = runCompiledExpression(compiledExpr);

            aggAccessor.reset(runTag, runVal);
            auto outSamp = runCompiledExpression(compiledCovarianceFinalizeSamp.get());
            auto outPop = runCompiledExpression(compiledCovarianceFinalizePop.get());

            const double precisionLimit = 0.0001;
            ASSERT_THAT(outSamp, ValueRoughEq(expValuesSamp[i], precisionLimit));
            ASSERT_THAT(outPop, ValueRoughEq(expValuesPop[i], precisionLimit));

            value::releaseValue(outSamp.first, outSamp.second);
            value::releaseValue(outPop.first, outPop.second);
            value::releaseValue(expValuesSamp[i].first, expValuesSamp[i].second);
            value::releaseValue(expValuesPop[i].first, expValuesPop[i].second);
        }
        for (size_t i = 0; i < inputValuesX.size(); ++i) {
            value::releaseValue(inputValuesX[i].first, inputValuesX[i].second);
            value::releaseValue(inputValuesY[i].first, inputValuesY[i].second);
        }
    }
};

TEST_F(SBECovarianceTest, BasicTest1) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValuesX = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> inputValuesY = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
    };

    std::vector<CovarianceOp> covarianceOps = {
        CovarianceOp::kAdd, CovarianceOp::kAdd, CovarianceOp::kRemove, CovarianceOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesSamp = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.0)},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesPop = {
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(1.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0)},
        {value::TypeTags::Null, 0},
    };

    runAndAssertExpression(inputValuesX, inputValuesY, covarianceOps, expValuesSamp, expValuesPop);
}

TEST_F(SBECovarianceTest, BasicTest2) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValuesX = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(12)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(18)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(23)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(45)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> inputValuesY = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(8)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(18)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(20)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(28)},
    };

    std::vector<CovarianceOp> covarianceOps = {
        CovarianceOp::kAdd,
        CovarianceOp::kAdd,
        CovarianceOp::kAdd,
        CovarianceOp::kAdd,
        CovarianceOp::kAdd,
        CovarianceOp::kRemove,
        CovarianceOp::kRemove,
        CovarianceOp::kRemove,
        CovarianceOp::kRemove,
        CovarianceOp::kRemove,
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesSamp = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(21)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(51.6667)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(64.6667)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(146.1)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(109)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(76)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(88)},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesPop = {
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(10.5)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(34.4444)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(48.5)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(116.88)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(81.75)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(50.6667)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(44)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0)},
        {value::TypeTags::Null, 0},
    };

    runAndAssertExpression(inputValuesX, inputValuesY, covarianceOps, expValuesSamp, expValuesPop);
}

TEST_F(SBECovarianceTest, MixedTypeTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValuesX = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(12)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128(18.0)).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(23.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(45.0)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> inputValuesY = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(8)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128(18.0)).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(20.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(28.0)},
    };

    std::vector<CovarianceOp> covarianceOps = {
        CovarianceOp::kAdd,
        CovarianceOp::kAdd,
        CovarianceOp::kAdd,
        CovarianceOp::kAdd,
        CovarianceOp::kAdd,
        CovarianceOp::kRemove,
        CovarianceOp::kRemove,
        CovarianceOp::kRemove,
        CovarianceOp::kRemove,
        CovarianceOp::kRemove,
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesSamp = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(21)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128(51.6667)).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128(64.6667)).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128(146.1)).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128(109)).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128(76)).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(88)},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesPop = {
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(10.5)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128(34.4444)).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128(48.5)).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128(116.88)).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128(81.75)).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128(50.6667)).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(44)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0)},
        {value::TypeTags::Null, 0},
    };

    runAndAssertExpression(inputValuesX, inputValuesY, covarianceOps, expValuesSamp, expValuesPop);
}

TEST_F(SBECovarianceTest, NonNumericTypeTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValuesX = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::StringSmall, value::makeSmallString("a").second},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> inputValuesY = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4)},
        {value::TypeTags::StringSmall, value::makeSmallString("a").second},
    };

    std::vector<CovarianceOp> covarianceOps = {
        CovarianceOp::kAdd, CovarianceOp::kAdd, CovarianceOp::kAdd, CovarianceOp::kAdd};

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesSamp = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.0)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesPop = {
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(1.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(1.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(1.0)},
    };

    runAndAssertExpression(inputValuesX, inputValuesY, covarianceOps, expValuesSamp, expValuesPop);
}

TEST_F(SBECovarianceTest, InfNanTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValuesX = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::infinity())},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> inputValuesY = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(-std::numeric_limits<double>::infinity())},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
    };

    std::vector<CovarianceOp> covarianceOps = {
        CovarianceOp::kAdd,
        CovarianceOp::kAdd,
        CovarianceOp::kAdd,
        CovarianceOp::kAdd,
        CovarianceOp::kAdd,
        CovarianceOp::kAdd,
        CovarianceOp::kRemove,
        CovarianceOp::kRemove,
        CovarianceOp::kRemove,
        CovarianceOp::kRemove,
        CovarianceOp::kAdd,
        CovarianceOp::kAdd,
        CovarianceOp::kAdd,
        CovarianceOp::kAdd,
        CovarianceOp::kRemove,
        CovarianceOp::kRemove,
        CovarianceOp::kRemove,
        CovarianceOp::kRemove,
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesSamp = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.0)},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::infinity())},
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
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.0)},
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
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.0)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValuesPop = {
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(0.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(1.0)},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::infinity())},
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
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(1.0)},
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
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(1.0)},
    };

    runAndAssertExpression(inputValuesX, inputValuesY, covarianceOps, expValuesSamp, expValuesPop);
}
}  // namespace mongo::sbe
