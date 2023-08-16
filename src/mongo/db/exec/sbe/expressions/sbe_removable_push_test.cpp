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
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/exec/sbe/accumulator_sum_value_enum.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::sbe {

enum class RemovablePushOp { kAdd, kRemove };

class SBERemovablePushTest : public EExpressionTestFixture {
public:
    void runAndAssertExpression(std::vector<std::pair<value::TypeTags, value::Value>>& inputValues,
                                std::vector<RemovablePushOp>& operations,
                                std::vector<std::pair<value::TypeTags, value::Value>>& expValues) {
        value::ViewOfValueAccessor inputAccessor;
        auto inputSlot = bindAccessor(&inputAccessor);

        value::OwnedValueAccessor aggAccessor;
        auto aggSlot = bindAccessor(&aggAccessor);

        auto aggRemovablePushAdd = sbe::makeE<sbe::EFunction>(
            "aggRemovablePushAdd", sbe::makeEs(makeE<EVariable>(inputSlot)));
        auto compiledRemovablePushAdd = compileAggExpression(*aggRemovablePushAdd, &aggAccessor);

        auto aggRemovablePushRemove =
            sbe::makeE<sbe::EFunction>("aggRemovablePushRemove", sbe::makeEs());
        auto compiledRemovablePushRemove =
            compileAggExpression(*aggRemovablePushRemove, &aggAccessor);

        auto aggRemovablePushFinalize = sbe::makeE<sbe::EFunction>(
            "aggRemovablePushFinalize", sbe::makeEs(makeE<EVariable>(aggSlot)));
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
            inputAccessor.reset(inputValues[idx].first, inputValues[idx].second);
            auto [runTag, runVal] = runCompiledExpression(compiledExpr);

            aggAccessor.reset(runTag, runVal);
            auto out = runCompiledExpression(compiledRemovablePushFinalize.get());

            ASSERT_EQ(out.first, expValues[i].first);
            ASSERT_THAT(out, ValueEq(expValues[i]));

            value::releaseValue(out.first, out.second);
            value::releaseValue(expValues[i].first, expValues[i].second);
        }
        for (size_t i = 0; i < inputValues.size(); ++i) {
            value::releaseValue(inputValues[i].first, inputValues[i].second);
        }
    }
};

TEST_F(SBERemovablePushTest, BasicTest) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(5)},
    };

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

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
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
    };

    runAndAssertExpression(inputValues, removablePushOps, expValues);
}
}  // namespace mongo::sbe
