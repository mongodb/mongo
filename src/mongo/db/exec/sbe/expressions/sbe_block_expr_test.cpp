/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <cstdint>
#include <memory>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/unittest/assert.h"

namespace mongo::sbe {

class SBEBlockExpressionTest : public EExpressionTestFixture {
public:
    void assertBlockOfBool(value::TypeTags tag, value::Value val, std::vector<bool> expected) {
        std::vector<std::pair<value::TypeTags, value::Value>> tvPairs;
        for (auto b : expected) {
            tvPairs.push_back({value::TypeTags::Boolean, value::bitcastFrom<bool>(b)});
        }
        assertBlockEq(tag, val, tvPairs);
    }

    std::unique_ptr<value::ValueBlock> makeBoolBlock(std::vector<bool> bools) {
        std::unique_ptr<value::ValueBlock> block = std::make_unique<value::HeterogeneousBlock>();
        for (auto b : bools) {
            auto [t, v] = makeBool(b);
            static_cast<value::HeterogeneousBlock*>(block.get())->push_back(t, v);
        }
        return block;
    }

    void assertBlockEq(value::TypeTags blockTag,
                       value::Value blockVal,
                       const std::vector<std::pair<value::TypeTags, value::Value>>& expected) {
        ASSERT_EQ(blockTag, value::TypeTags::valueBlock);
        auto* block = value::bitcastTo<value::ValueBlock*>(blockVal);
        auto extracted = block->extract();
        ASSERT_EQ(expected.size(), extracted.count);

        for (size_t i = 0; i < extracted.count; ++i) {
            auto [t, v] = value::compareValue(
                extracted.tags[i], extracted.vals[i], expected[i].first, expected[i].second);
            ASSERT_EQ(t, value::TypeTags::NumberInt32) << extracted;
            ASSERT_EQ(value::bitcastTo<int32_t>(v), 0)
                << "Got " << extracted[i] << " expected " << expected[i] << " full extracted output"
                << extracted;
        }
    }
};

TEST_F(SBEBlockExpressionTest, BlockExistsTest) {
    value::ViewOfValueAccessor blockAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);
    auto existsExpr =
        sbe::makeE<sbe::EFunction>("valueBlockExists", sbe::makeEs(makeE<EVariable>(blockSlot)));
    auto compiledExpr = compileExpression(*existsExpr);

    value::HeterogeneousBlock block;
    block.push_back(makeInt32(42));
    block.push_back(makeInt32(43));
    block.push_back(makeInt32(44));
    block.push_back(makeNothing());
    block.push_back(makeInt32(46));

    blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(&block));
    auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(runTag, runVal);

    assertBlockOfBool(runTag, runVal, std::vector{true, true, true, false, true});
}

TEST_F(SBEBlockExpressionTest, BlockApplyLambdaTest) {
    value::ViewOfValueAccessor blockAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);

    FrameId frame = 10;
    // Multiply each value by two.
    auto expr = makeE<sbe::EFunction>(
        "valueBlockApplyLambda",
        sbe::makeEs(makeE<EVariable>(blockSlot),
                    makeE<ELocalLambda>(frame,
                                        makeE<EPrimBinary>(EPrimBinary::Op::mul,
                                                           makeE<EVariable>(frame, 0),
                                                           makeC(makeInt32(2))))));
    auto compiledExpr = compileExpression(*expr);

    value::HeterogeneousBlock block;
    block.push_back(makeInt32(42));
    block.push_back(makeInt32(43));
    block.push_back(makeInt32(44));
    block.push_back(makeNothing());
    block.push_back(makeInt32(46));

    blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(&block));
    auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(runTag, runVal);

    assertBlockEq(runTag,
                  runVal,
                  std::vector<std::pair<value::TypeTags, value::Value>>{
                      makeInt32(84), makeInt32(86), makeInt32(88), makeNothing(), makeInt32(92)});
}

TEST_F(SBEBlockExpressionTest, BlockLogicAndOrTest) {
    value::ViewOfValueAccessor blockAccessorLeft;
    value::ViewOfValueAccessor blockAccessorRight;
    auto blockLeftSlot = bindAccessor(&blockAccessorLeft);
    auto blockRightSlot = bindAccessor(&blockAccessorRight);

    auto leftBlock = makeBoolBlock({true, false, true, false});
    blockAccessorLeft.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(leftBlock.get()));

    auto rightBlock = makeBoolBlock({true, true, false, false});
    blockAccessorRight.reset(sbe::value::TypeTags::valueBlock,
                             value::bitcastFrom<value::ValueBlock*>(rightBlock.get()));

    {
        auto expr = makeE<sbe::EFunction>(
            "valueBlockLogicalAnd",
            sbe::makeEs(makeE<EVariable>(blockLeftSlot), makeE<EVariable>(blockRightSlot)));
        auto compiledExpr = compileExpression(*expr);

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        assertBlockOfBool(runTag, runVal, {true, false, false, false});
    }

    {
        auto expr = makeE<sbe::EFunction>(
            "valueBlockLogicalOr",
            sbe::makeEs(makeE<EVariable>(blockLeftSlot), makeE<EVariable>(blockRightSlot)));
        auto compiledExpr = compileExpression(*expr);

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        assertBlockOfBool(runTag, runVal, {true, true, true, false});
    }
}

}  // namespace mongo::sbe
