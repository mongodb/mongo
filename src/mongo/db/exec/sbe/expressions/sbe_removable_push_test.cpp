// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/docval_to_sbeval.h"
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

enum class RemovablePushOp { kAdd, kRemove };

class SBERemovablePushTest : public EExpressionTestFixture {
public:
    void runAndAssertExpression(const std::vector<value::TagValueOwned>& inputValues,
                                const std::vector<RemovablePushOp>& operations,
                                const std::vector<value::TagValueOwned>& expValues) {
        value::ViewOfValueAccessor inputAccessor;
        auto inputSlot = bindAccessor(&inputAccessor);

        value::OwnedValueAccessor aggAccessor;
        auto aggSlot = bindAccessor(&aggAccessor);

        auto aggRemovablePushAdd = sbe::makeE<sbe::EFunction>(
            EFn::kAggRemovablePushAdd, sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledRemovablePushAdd = compileAggExpression(*aggRemovablePushAdd, &aggAccessor);

        auto aggRemovablePushRemove = sbe::makeE<sbe::EFunction>(
            EFn::kAggRemovablePushRemove, sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledRemovablePushRemove =
            compileAggExpression(*aggRemovablePushRemove, &aggAccessor);

        auto aggRemovablePushFinalize = sbe::makeE<sbe::EFunction>(
            EFn::kAggRemovablePushFinalize, sbe::makeEs(makeE<EVariable>(aggSlot)));
        auto compiledRemovablePushFinalize = compileExpression(*aggRemovablePushFinalize);

        // call RemovablePushOp (Add/Remove) on the inputs and call finalize() method after each op
        size_t addIdx = 0, removeIdx = 0;
        for (size_t i = 0; i < operations.size(); ++i) {
            vm::CodeFragment* compiledExpr;
            size_t idx;
            if (operations[i] == RemovablePushOp::kAdd) {
                compiledExpr = compiledRemovablePushAdd.get();
                idx = addIdx++;
            } else {
                compiledExpr = compiledRemovablePushRemove.get();
                idx = removeIdx++;
            }
            inputAccessor.reset(inputValues[idx].tag(), inputValues[idx].value());
            auto [runTag, runVal] = runCompiledExpression(compiledExpr);

            aggAccessor.reset(runTag, runVal);
            auto out = runCompiledExpression(compiledRemovablePushFinalize.get());
            value::TagValueOwned outOwned = value::TagValueOwned::fromRaw(out);

            ASSERT_EQ(out.first, expValues[i].tag());
            ASSERT_THAT(out, ValueEq(expValues[i].view()));
        }
    }
};

TEST_F(SBERemovablePushTest, BasicTest) {
    auto inputValues = makeOwnedVector({
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5)},
    });

    std::vector<RemovablePushOp> removablePushOps = {RemovablePushOp::kAdd,
                                                     RemovablePushOp::kAdd,
                                                     RemovablePushOp::kAdd,
                                                     RemovablePushOp::kRemove,
                                                     RemovablePushOp::kAdd,
                                                     RemovablePushOp::kAdd,
                                                     RemovablePushOp::kRemove,
                                                     RemovablePushOp::kRemove,
                                                     RemovablePushOp::kRemove,
                                                     RemovablePushOp::kRemove};

    auto expValues = makeOwnedVector({
        value::makeValue(Value(BSON_ARRAY(1))),
        value::makeValue(Value(BSON_ARRAY(1 << 2))),
        value::makeValue(Value(BSON_ARRAY(1 << 2 << 3))),
        value::makeValue(Value(BSON_ARRAY(2 << 3))),
        value::makeValue(Value(BSON_ARRAY(2 << 3 << 4))),
        value::makeValue(Value(BSON_ARRAY(2 << 3 << 4 << 5))),
        value::makeValue(Value(BSON_ARRAY(3 << 4 << 5))),
        value::makeValue(Value(BSON_ARRAY(4 << 5))),
        value::makeValue(Value(BSON_ARRAY(5))),
        value::makeNewArray(),
    });

    runAndAssertExpression(inputValues, removablePushOps, expValues);
}

TEST_F(SBERemovablePushTest, TestWithEmptyFields) {
    auto inputValues = makeOwnedVector({
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::Nothing, 0},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
    });

    std::vector<RemovablePushOp> removablePushOps = {RemovablePushOp::kAdd,
                                                     RemovablePushOp::kAdd,
                                                     RemovablePushOp::kAdd,
                                                     RemovablePushOp::kRemove,
                                                     RemovablePushOp::kRemove,
                                                     RemovablePushOp::kRemove};

    auto expValues = makeOwnedVector({
        value::makeValue(Value(BSON_ARRAY(1))),
        value::makeValue(Value(BSON_ARRAY(1))),
        value::makeValue(Value(BSON_ARRAY(1 << 2))),
        value::makeValue(Value(BSON_ARRAY(2))),
        value::makeValue(Value(BSON_ARRAY(2))),
        value::makeNewArray(),
    });

    runAndAssertExpression(inputValues, removablePushOps, expValues);
}
}  // namespace mongo::sbe
