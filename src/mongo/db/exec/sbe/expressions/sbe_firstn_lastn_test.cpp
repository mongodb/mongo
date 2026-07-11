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

        auto initExpr = sbe::makeE<sbe::EFunction>(
            isFirstN ? EFn::kAggRemovableFirstNInit : EFn::kAggRemovableLastNInit,
            sbe::makeEs(makeE<EConstant>(value::TypeTags::NumberInt32, n)));
        auto compiledInit = compileExpression(*initExpr);

        auto addExpr = sbe::makeE<sbe::EFunction>(isFirstN ? EFn::kAggRemovableFirstNAdd
                                                           : EFn::kAggRemovableLastNAdd,
                                                  sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledAdd = compileAggExpression(*addExpr, &aggAccessor);

        auto removeExpr = sbe::makeE<sbe::EFunction>(isFirstN ? EFn::kAggRemovableFirstNRemove
                                                              : EFn::kAggRemovableLastNRemove,
                                                     sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledRemove = compileAggExpression(*removeExpr, &aggAccessor);

        auto finalizeExpr = sbe::makeE<sbe::EFunction>(isFirstN ? EFn::kAggRemovableFirstNFinalize
                                                                : EFn::kAggRemovableLastNFinalize,
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
        auto initExpr = sbe::makeE<sbe::EFunction>(isFirstN ? EFn::kAggRemovableFirstNInit
                                                            : EFn::kAggRemovableLastNInit,
                                                   sbe::makeEs(makeE<EConstant>(tag, val)));
        auto compiledInit = compileExpression(*initExpr);

        Status status = [&]() {
            try {
                value::TagValueOwned state =
                    value::TagValueOwned::fromRaw(runCompiledExpression(compiledInit.get()));
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
