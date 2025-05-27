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
#include "mongo/platform/compiler.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <tuple>
#include <vector>

namespace mongo::sbe {

class SBELinearFillTest : public EExpressionTestFixture {
public:
    std::pair<value::TypeTags, value::Value> initState() {
        auto [stateTag, stateVal] = value::makeNewArray();
        auto state = value::getArrayView(stateVal);
        state->push_back(value::TypeTags::Null, 0);
        state->push_back(value::TypeTags::Null, 0);
        state->push_back(value::TypeTags::Null, 0);
        state->push_back(value::TypeTags::Null, 0);
        state->push_back(value::TypeTags::Null, 0);
        state->push_back(value::TypeTags::NumberInt64, 0);
        return {stateTag, stateVal};
    }

    void runAndAssertExpression(std::vector<std::pair<value::TypeTags, value::Value>>& inputValues,
                                std::vector<std::pair<value::TypeTags, value::Value>>& sortByValues,
                                std::vector<std::pair<value::TypeTags, value::Value>>& expValues) {
        value::ViewOfValueAccessor inputAccessor;
        auto inputSlot = bindAccessor(&inputAccessor);

        value::ViewOfValueAccessor sortByAccessor;
        auto sortBySlot = bindAccessor(&sortByAccessor);

        value::OwnedValueAccessor aggAccessor;
        auto aggSlot = bindAccessor(&aggAccessor);

        auto aggLinearFillCanAddExpr = sbe::makeE<sbe::EFunction>(
            "aggLinearFillCanAdd", sbe::makeEs(makeE<EVariable>(aggSlot)));
        auto compiledLinearFillCanAdd = compileExpression(*aggLinearFillCanAddExpr);

        auto aggLinearFillAddExpr = sbe::makeE<sbe::EFunction>(
            "aggLinearFillAdd",
            sbe::makeEs(makeE<EVariable>(inputSlot), makeE<EVariable>(sortBySlot)));
        auto compiledLinearFillAdd = compileAggExpression(*aggLinearFillAddExpr, &aggAccessor);

        auto aggLinearFillFinalize = sbe::makeE<sbe::EFunction>(
            "aggLinearFillFinalize",
            sbe::makeEs(makeE<EVariable>(aggSlot), makeE<EVariable>(sortBySlot)));
        auto compiledLinearFillFinalize = compileExpression(*aggLinearFillFinalize);

        MONGO_COMPILER_DIAGNOSTIC_PUSH
        MONGO_COMPILER_DIAGNOSTIC_IGNORED_TRANSITIONAL("-Wstringop-overflow")
        auto [stateTag, stateVal] = initState();
        MONGO_COMPILER_DIAGNOSTIC_POP
        aggAccessor.reset(stateTag, stateVal);

        size_t idx = 0;
        for (size_t i = 0; i < inputValues.size(); ++i) {
            while (idx < inputValues.size()) {
                auto [runTag, runVal] = runCompiledExpression(compiledLinearFillCanAdd.get());
                ASSERT_EQ(runTag, value::TypeTags::Boolean);
                auto addMore = value::bitcastTo<bool>(runVal);
                if (!addMore) {
                    break;
                }

                inputAccessor.reset(inputValues[idx].first, inputValues[idx].second);
                sortByAccessor.reset(sortByValues[idx].first, sortByValues[idx].second);
                std::tie(runTag, runVal) = runCompiledExpression(compiledLinearFillAdd.get());
                aggAccessor.reset(runTag, runVal);
                idx++;
            }

            sortByAccessor.reset(sortByValues[i].first, sortByValues[i].second);
            auto out = runCompiledExpression(compiledLinearFillFinalize.get());

            ASSERT_EQ(out.first, expValues[i].first);
            ASSERT_THAT(out, ValueEq(expValues[i]));

            value::releaseValue(out.first, out.second);
            value::releaseValue(expValues[i].first, expValues[i].second);

            value::releaseValue(inputValues[i].first, inputValues[i].second);
            value::releaseValue(sortByValues[i].first, sortByValues[i].second);
        }
    }
};

TEST_F(SBELinearFillTest, LinearFillSortedByDate) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5)},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(9)},
        {value::TypeTags::Null, 0},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::Date, 1589811030000LL},
        {value::TypeTags::Date, 1589811060000LL},
        {value::TypeTags::Date, 1589811090000LL},
        {value::TypeTags::Date, 1589811120000LL},
        {value::TypeTags::Date, 1589811150000LL},
        {value::TypeTags::Date, 1589811180000LL},
        {value::TypeTags::Date, 1589811210000LL},
        {value::TypeTags::Date, 1589811240000LL},
        {value::TypeTags::Date, 1589811270000LL},
        {value::TypeTags::Date, 1589811300000LL},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(4.0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(7.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(8.0)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(9)},
        {value::TypeTags::Null, 0},
    };

    runAndAssertExpression(inputValues, sortByValues, expValues);
}

TEST_F(SBELinearFillTest, LinearFillSortedByNumericType) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.0)},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{9.0}).second},
        {value::TypeTags::Null, 0},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt64, 1LL},
        {value::TypeTags::NumberInt64, 2LL},
        {value::TypeTags::NumberInt64, 3LL},
        {value::TypeTags::NumberInt64, 4LL},
        {value::TypeTags::NumberInt64, 5LL},
        {value::TypeTags::NumberInt64, 6LL},
        {value::TypeTags::NumberInt64, 7LL},
        {value::TypeTags::NumberInt64, 8LL},
        {value::TypeTags::NumberInt64, 9LL},
        {value::TypeTags::NumberInt64, 10LL},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(4.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.0)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{6.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{7.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{8.0}).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{9.0}).second},
        {value::TypeTags::Null, 0},
    };

    runAndAssertExpression(inputValues, sortByValues, expValues);
}

TEST_F(SBELinearFillTest, LinearFillAllNull) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt64, 1LL},
        {value::TypeTags::NumberInt64, 2LL},
        {value::TypeTags::NumberInt64, 3LL},
        {value::TypeTags::NumberInt64, 4LL},
        {value::TypeTags::NumberInt64, 5LL},
        {value::TypeTags::NumberInt64, 6LL},
        {value::TypeTags::NumberInt64, 7LL},
        {value::TypeTags::NumberInt64, 8LL},
        {value::TypeTags::NumberInt64, 9LL},
        {value::TypeTags::NumberInt64, 10LL},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
    };

    runAndAssertExpression(inputValues, sortByValues, expValues);
}

TEST_F(SBELinearFillTest, LinearFillAllNonNull) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt64, 1},
        {value::TypeTags::NumberInt64, 2},
        {value::TypeTags::NumberInt64, 3},
        {value::TypeTags::NumberInt64, 4},
        {value::TypeTags::NumberInt64, 5},
        {value::TypeTags::NumberInt64, 6},
        {value::TypeTags::NumberInt64, 7},
        {value::TypeTags::NumberInt64, 8},
        {value::TypeTags::NumberInt64, 9},
        {value::TypeTags::NumberInt64, 10},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt64, 1LL},
        {value::TypeTags::NumberInt64, 2LL},
        {value::TypeTags::NumberInt64, 3LL},
        {value::TypeTags::NumberInt64, 4LL},
        {value::TypeTags::NumberInt64, 5LL},
        {value::TypeTags::NumberInt64, 6LL},
        {value::TypeTags::NumberInt64, 7LL},
        {value::TypeTags::NumberInt64, 8LL},
        {value::TypeTags::NumberInt64, 9LL},
        {value::TypeTags::NumberInt64, 10LL},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::NumberInt64, 1},
        {value::TypeTags::NumberInt64, 2},
        {value::TypeTags::NumberInt64, 3},
        {value::TypeTags::NumberInt64, 4},
        {value::TypeTags::NumberInt64, 5},
        {value::TypeTags::NumberInt64, 6},
        {value::TypeTags::NumberInt64, 7},
        {value::TypeTags::NumberInt64, 8},
        {value::TypeTags::NumberInt64, 9},
        {value::TypeTags::NumberInt64, 10},
    };

    runAndAssertExpression(inputValues, sortByValues, expValues);
}

TEST_F(SBELinearFillTest, LinearFillOnlyOneNonNull) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberInt64, 10},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt64, 1LL},
        {value::TypeTags::NumberInt64, 2LL},
        {value::TypeTags::NumberInt64, 3LL},
        {value::TypeTags::NumberInt64, 4LL},
        {value::TypeTags::NumberInt64, 5LL},
        {value::TypeTags::NumberInt64, 6LL},
        {value::TypeTags::NumberInt64, 7LL},
        {value::TypeTags::NumberInt64, 8LL},
        {value::TypeTags::NumberInt64, 9LL},
        {value::TypeTags::NumberInt64, 10LL},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberInt64, 10},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
    };

    runAndAssertExpression(inputValues, sortByValues, expValues);
}
}  // namespace mongo::sbe
