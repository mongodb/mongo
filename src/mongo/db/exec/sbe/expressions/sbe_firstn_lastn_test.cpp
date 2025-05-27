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

enum class FirstLastNOp { kAdd, kRemove };

class SBEFirstLastNTest : public EExpressionTestFixture {
public:
    void runAndAssertExpression(bool isFirstN,
                                int64_t n,
                                std::vector<std::pair<value::TypeTags, value::Value>>& inputValues,
                                std::vector<FirstLastNOp>& operations,
                                std::vector<std::pair<value::TypeTags, value::Value>>& expValues) {
        value::ViewOfValueAccessor inputAccessor;
        auto inputSlot = bindAccessor(&inputAccessor);

        value::OwnedValueAccessor aggAccessor;
        auto aggSlot = bindAccessor(&aggAccessor);

        auto exprNamePrefix = "aggRemovable" + std::string(isFirstN ? "FirstN" : "LastN");

        auto initExpr = sbe::makeE<sbe::EFunction>(
            exprNamePrefix + "Init",
            sbe::makeEs(makeE<EConstant>(value::TypeTags::NumberInt32, n)));
        auto compiledInit = compileExpression(*initExpr);

        auto addExpr = sbe::makeE<sbe::EFunction>(exprNamePrefix + "Add",
                                                  sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledAdd = compileAggExpression(*addExpr, &aggAccessor);

        auto removeExpr = sbe::makeE<sbe::EFunction>(exprNamePrefix + "Remove",
                                                     sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledRemove = compileAggExpression(*removeExpr, &aggAccessor);

        auto finalizeExpr = sbe::makeE<sbe::EFunction>(exprNamePrefix + "Finalize",
                                                       sbe::makeEs(makeE<EVariable>(aggSlot)));
        auto compiledFinalize = compileExpression(*finalizeExpr);

        auto [stateTag, stateVal] = runCompiledExpression(compiledInit.get());
        aggAccessor.reset(stateTag, stateVal);

        // call FirstLastNOp (firstNAdd/Remove) on the inputs and call finalize() method after each
        // FirstLastNOp
        size_t addIdx = 0, removeIdx = 0;
        for (size_t i = 0; i < operations.size(); ++i) {
            vm::CodeFragment* compiledExpr;
            size_t idx;
            if (operations[i] == FirstLastNOp::kAdd) {
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

    void runAndAssertErrorDuringInit(bool isFirstN,
                                     value::TypeTags tag,
                                     value::Value val,
                                     int expErrCode) {
        auto initExpr = sbe::makeE<sbe::EFunction>(
            "aggRemovable" + std::string(isFirstN ? "FirstN" : "LastN") + "Init",
            sbe::makeEs(makeE<EConstant>(tag, val)));
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

TEST_F(SBEFirstLastNTest, FirstNTestWithPositiveN) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(3)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(4.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.0)}};

    std::vector<FirstLastNOp> operations = {FirstLastNOp::kAdd,
                                            FirstLastNOp::kAdd,
                                            FirstLastNOp::kAdd,
                                            FirstLastNOp::kAdd,
                                            FirstLastNOp::kAdd,
                                            FirstLastNOp::kAdd,
                                            FirstLastNOp::kRemove,
                                            FirstLastNOp::kRemove,
                                            FirstLastNOp::kRemove,
                                            FirstLastNOp::kRemove,
                                            FirstLastNOp::kRemove,
                                            FirstLastNOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        makeArray(BSON_ARRAY(1LL)),
        makeArray(BSON_ARRAY(1LL << 2LL)),
        makeArray(BSON_ARRAY(1LL << 2LL << 3LL)),
        makeArray(BSON_ARRAY(1LL << 2LL << 3LL << 4.0)),
        makeArray(BSON_ARRAY(1LL << 2LL << 3LL << 4.0)),
        makeArray(BSON_ARRAY(1LL << 2LL << 3LL << 4.0)),
        makeArray(BSON_ARRAY(2LL << 3LL << 4.0 << 5.0)),
        makeArray(BSON_ARRAY(3LL << 4.0 << 5.0 << 6.0)),
        makeArray(BSON_ARRAY(4.0 << 5.0 << 6.0)),
        makeArray(BSON_ARRAY(5.0 << 6.0)),
        makeArray(BSON_ARRAY(6.0)),
        makeArray(BSONArrayBuilder().arr())};

    runAndAssertExpression(true, 4, inputValues, operations, expValues);
}

TEST_F(SBEFirstLastNTest, LastNTestWithPositiveN) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(3)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(4.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(6.0)}};

    std::vector<FirstLastNOp> operations = {FirstLastNOp::kAdd,
                                            FirstLastNOp::kAdd,
                                            FirstLastNOp::kAdd,
                                            FirstLastNOp::kAdd,
                                            FirstLastNOp::kAdd,
                                            FirstLastNOp::kAdd,
                                            FirstLastNOp::kRemove,
                                            FirstLastNOp::kRemove,
                                            FirstLastNOp::kRemove,
                                            FirstLastNOp::kRemove,
                                            FirstLastNOp::kRemove,
                                            FirstLastNOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        makeArray(BSON_ARRAY(1LL)),
        makeArray(BSON_ARRAY(1LL << 2LL)),
        makeArray(BSON_ARRAY(1LL << 2LL << 3LL)),
        makeArray(BSON_ARRAY(1LL << 2LL << 3LL << 4.0)),
        makeArray(BSON_ARRAY(2LL << 3LL << 4.0 << 5.0)),
        makeArray(BSON_ARRAY(3LL << 4.0 << 5.0 << 6.0)),
        makeArray(BSON_ARRAY(3LL << 4.0 << 5.0 << 6.0)),
        makeArray(BSON_ARRAY(3LL << 4.0 << 5.0 << 6.0)),
        makeArray(BSON_ARRAY(4.0 << 5.0 << 6.0)),
        makeArray(BSON_ARRAY(5.0 << 6.0)),
        makeArray(BSON_ARRAY(6.0)),
        makeArray(BSONArrayBuilder().arr())};

    runAndAssertExpression(false, 4.0, inputValues, operations, expValues);
}

TEST_F(SBEFirstLastNTest, FirstNTestWithNegativeN) {
    runAndAssertErrorDuringInit(
        true, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-100), 8070608);
}

TEST_F(SBEFirstLastNTest, FirstNTestWithZeroN) {
    runAndAssertErrorDuringInit(
        true, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0), 8070608);
}

TEST_F(SBEFirstLastNTest, FirstNTestWithNonIntegralN) {
    runAndAssertErrorDuringInit(
        true, value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.4), 8070607);
}

}  // namespace mongo::sbe
