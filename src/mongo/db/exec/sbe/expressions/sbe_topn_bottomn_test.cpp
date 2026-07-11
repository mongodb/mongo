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

        auto initExpr = sbe::makeE<sbe::EFunction>(
            isTopN ? EFn::kAggRemovableTopNInit : EFn::kAggRemovableBottomNInit,
            sbe::makeEs(
                makeE<EConstant>(value::TypeTags::NumberInt64, n),
                makeE<EConstant>(value::TypeTags::NumberInt32, std::numeric_limits<int>::max())));
        auto compiledInit = compileExpression(*initExpr);

        auto addExpr = sbe::makeE<sbe::EFunction>(
            isTopN ? EFn::kAggRemovableTopNAdd : EFn::kAggRemovableBottomNAdd,
            sbe::makeEs(makeE<EVariable>(keySlot), makeE<EVariable>(valueSlot)));
        auto compiledAdd = compileAggExpression(*addExpr, &aggAccessor);

        auto removeExpr = sbe::makeE<sbe::EFunction>(
            isTopN ? EFn::kAggRemovableTopNRemove : EFn::kAggRemovableBottomNRemove,
            sbe::makeEs(makeE<EVariable>(keySlot), makeE<EVariable>(valueSlot)));
        auto compiledRemove = compileAggExpression(*removeExpr, &aggAccessor);

        auto finalizeExpr = sbe::makeE<sbe::EFunction>(isTopN ? EFn::kAggRemovableTopNFinalize
                                                              : EFn::kAggRemovableBottomNFinalize,
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
            isTopN ? EFn::kAggRemovableTopNInit : EFn::kAggRemovableBottomNInit,
            sbe::makeEs(makeE<EConstant>(tag, val),
                        makeE<EConstant>(value::TypeTags::NumberInt32, 100)));
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
