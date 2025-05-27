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

enum class TopBottomNOp { kAdd, kRemove };

class SBETopBottomNTest : public EExpressionTestFixture {
public:
    void runAndAssertExpression(bool isTopN,
                                int64_t n,
                                std::vector<std::pair<value::TypeTags, value::Value>>& keys,
                                std::vector<std::pair<value::TypeTags, value::Value>>& values,
                                std::vector<TopBottomNOp>& operations,
                                std::vector<std::pair<value::TypeTags, value::Value>>& expValues) {
        value::ViewOfValueAccessor keyAccessor;
        auto keySlot = bindAccessor(&keyAccessor);

        value::ViewOfValueAccessor valueAccessor;
        auto valueSlot = bindAccessor(&valueAccessor);

        value::OwnedValueAccessor aggAccessor;
        auto aggSlot = bindAccessor(&aggAccessor);

        auto exprNamePrefix = "aggRemovable" + std::string(isTopN ? "TopN" : "BottomN");

        auto initExpr = sbe::makeE<sbe::EFunction>(
            exprNamePrefix + "Init",
            sbe::makeEs(
                makeE<EConstant>(value::TypeTags::NumberInt64, n),
                makeE<EConstant>(value::TypeTags::NumberInt32, std::numeric_limits<int>::max())));
        auto compiledInit = compileExpression(*initExpr);

        auto addExpr = sbe::makeE<sbe::EFunction>(
            exprNamePrefix + "Add",
            sbe::makeEs(makeE<EVariable>(keySlot), makeE<EVariable>(valueSlot)));
        auto compiledAdd = compileAggExpression(*addExpr, &aggAccessor);

        auto removeExpr = sbe::makeE<sbe::EFunction>(
            exprNamePrefix + "Remove",
            sbe::makeEs(makeE<EVariable>(keySlot), makeE<EVariable>(valueSlot)));
        auto compiledRemove = compileAggExpression(*removeExpr, &aggAccessor);

        auto finalizeExpr = sbe::makeE<sbe::EFunction>(exprNamePrefix + "Finalize",
                                                       sbe::makeEs(makeE<EVariable>(aggSlot)));
        auto compiledFinalize = compileExpression(*finalizeExpr);

        auto [stateTag, stateVal] = runCompiledExpression(compiledInit.get());
        aggAccessor.reset(stateTag, stateVal);

        // call TopBottomNOp (Add/Remove) on the inputs and call finalize() method after each
        // TopBottomNOp
        size_t addIdx = 0, removeIdx = 0;
        for (size_t i = 0; i < operations.size(); ++i) {
            vm::CodeFragment* compiledExpr;
            size_t idx;
            if (operations[i] == TopBottomNOp::kAdd) {
                compiledExpr = compiledAdd.get();
                idx = addIdx++;
            } else {
                compiledExpr = compiledRemove.get();
                idx = removeIdx++;
            }
            keyAccessor.reset(keys[idx].first, keys[idx].second);
            valueAccessor.reset(values[idx].first, values[idx].second);
            auto [runTag, runVal] = runCompiledExpression(compiledExpr);

            aggAccessor.reset(runTag, runVal);
            auto out = runCompiledExpression(compiledFinalize.get());

            ASSERT_THAT(out, ValueEq(expValues[i]));

            value::releaseValue(out.first, out.second);
            value::releaseValue(expValues[i].first, expValues[i].second);
        }
        for (size_t i = 0; i < keys.size(); ++i) {
            value::releaseValue(keys[i].first, keys[i].second);
            value::releaseValue(values[i].first, values[i].second);
        }
    }

    void runAndAssertErrorDuringInit(bool isTopN,
                                     value::TypeTags tag,
                                     value::Value val,
                                     int expErrCode) {
        auto initExpr = sbe::makeE<sbe::EFunction>(
            "aggRemovable" + std::string(isTopN ? "TopN" : "BottomN") + "Init",
            sbe::makeEs(makeE<EConstant>(tag, val),
                        makeE<EConstant>(value::TypeTags::NumberInt32, 100)));
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
        ASSERT_EQ(status.code(), expErrCode);
    }
};

TEST_F(SBETopBottomNTest, TopNTestWithPositiveN) {
    std::vector<std::pair<value::TypeTags, value::Value>> keys = {
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.0)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(3)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(4.0)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> values = {
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(20)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(10)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(60.0)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(30)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(50.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(40.0)},
    };

    std::vector<TopBottomNOp> operations = {TopBottomNOp::kAdd,
                                            TopBottomNOp::kAdd,
                                            TopBottomNOp::kAdd,
                                            TopBottomNOp::kAdd,
                                            TopBottomNOp::kAdd,
                                            TopBottomNOp::kAdd,
                                            TopBottomNOp::kRemove,
                                            TopBottomNOp::kRemove,
                                            TopBottomNOp::kRemove,
                                            TopBottomNOp::kRemove,
                                            TopBottomNOp::kRemove,
                                            TopBottomNOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        makeArray(BSON_ARRAY(20)),
        makeArray(BSON_ARRAY(10 << 20)),
        makeArray(BSON_ARRAY(10 << 20 << 60.0)),
        makeArray(BSON_ARRAY(10 << 20 << 30 << 60.0)),
        makeArray(BSON_ARRAY(10 << 20 << 30 << 50.0)),
        makeArray(BSON_ARRAY(10 << 20 << 30 << 40.0)),
        makeArray(BSON_ARRAY(10 << 30 << 40.0 << 50.0)),
        makeArray(BSON_ARRAY(30 << 40.0 << 50.0 << 60.0)),
        makeArray(BSON_ARRAY(30 << 40.0 << 50.0)),
        makeArray(BSON_ARRAY(40.0 << 50.0)),
        makeArray(BSON_ARRAY(40.0)),
        makeArray(BSONArrayBuilder().arr())};

    runAndAssertExpression(true, 4, keys, values, operations, expValues);
}

TEST_F(SBETopBottomNTest, BottomNTestWithPositiveN) {
    std::vector<std::pair<value::TypeTags, value::Value>> keys = {
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.0)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(3)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(4.0)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> values = {
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(20)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(10)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(60.0)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(30)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(50.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(40.0)},
    };

    std::vector<TopBottomNOp> operations = {TopBottomNOp::kAdd,
                                            TopBottomNOp::kAdd,
                                            TopBottomNOp::kAdd,
                                            TopBottomNOp::kAdd,
                                            TopBottomNOp::kAdd,
                                            TopBottomNOp::kAdd,
                                            TopBottomNOp::kRemove,
                                            TopBottomNOp::kRemove,
                                            TopBottomNOp::kRemove,
                                            TopBottomNOp::kRemove,
                                            TopBottomNOp::kRemove,
                                            TopBottomNOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        makeArray(BSON_ARRAY(20)),
        makeArray(BSON_ARRAY(10 << 20)),
        makeArray(BSON_ARRAY(10 << 20 << 60.0)),
        makeArray(BSON_ARRAY(10 << 20 << 30 << 60.0)),
        makeArray(BSON_ARRAY(20 << 30 << 50.0 << 60.0)),
        makeArray(BSON_ARRAY(30 << 40.0 << 50.0 << 60.0)),
        makeArray(BSON_ARRAY(30 << 40.0 << 50.0 << 60.0)),
        makeArray(BSON_ARRAY(30 << 40.0 << 50.0 << 60.0)),
        makeArray(BSON_ARRAY(30 << 40.0 << 50.0)),
        makeArray(BSON_ARRAY(40.0 << 50.0)),
        makeArray(BSON_ARRAY(40.0)),
        makeArray(BSONArrayBuilder().arr())};

    runAndAssertExpression(false, 4.0, keys, values, operations, expValues);
}

TEST_F(SBETopBottomNTest, TopNTestWithNegativeN) {
    runAndAssertErrorDuringInit(
        true, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-100), 8155708);
}

TEST_F(SBETopBottomNTest, TopNTestWithZeroN) {
    runAndAssertErrorDuringInit(
        true, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0), 8155708);
}

TEST_F(SBETopBottomNTest, TopNTestWithNonIntegralN) {
    runAndAssertErrorDuringInit(
        true, value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.4), 8155711);
}

}  // namespace mongo::sbe
