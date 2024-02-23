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
#include "mongo/db/exec/sbe/sbe_block_test_helpers.h"
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

    void assertBlockEq(value::TypeTags blockTag,
                       value::Value blockVal,
                       const std::vector<std::pair<value::TypeTags, value::Value>>& expected) {
        ASSERT_EQ(blockTag, value::TypeTags::valueBlock);
        auto* block = value::bitcastTo<value::ValueBlock*>(blockVal);
        auto extracted = block->extract();
        ASSERT_EQ(expected.size(), extracted.count());

        for (size_t i = 0; i < extracted.count(); ++i) {
            auto [t, v] = value::compareValue(
                extracted.tags()[i], extracted.vals()[i], expected[i].first, expected[i].second);
            ASSERT_EQ(t, value::TypeTags::NumberInt32) << extracted;
            ASSERT_EQ(value::bitcastTo<int32_t>(v), 0)
                << "Got " << extracted[i] << " expected " << expected[i]
                << " full extracted output " << extracted;
        }
    }

    void testFoldF(std::vector<std::pair<value::TypeTags, value::Value>> vals,
                   std::vector<int32_t> filterPosInfo,
                   std::vector<bool> expectedResult);

    void testCmpScalar(EPrimBinary::Op, StringData cmpFunctionName, value::ValueBlock* valBlock);
    void testBlockBlockArithmeticOp(EPrimBinary::Op scalarOp,
                                    StringData blockFunctionName,
                                    value::ValueBlock* bitsetBlock,
                                    value::ValueBlock* leftBlock,
                                    value::ValueBlock* rightBlock,
                                    bool monoBlockExpected = false);
    void testBlockScalarArithmeticOp(EPrimBinary::Op scalarOp,
                                     StringData blockFunctionName,
                                     value::ValueBlock* bitsetBlock,
                                     value::ValueBlock* block,
                                     std::pair<value::TypeTags, value::Value> scalar);

    enum class BlockType { HETEROGENEOUS = 0, MONOBLOCK, BOOLBLOCK };

    void testBlockLogicalOp(EPrimBinary::Op scalarOp,
                            StringData blockFunctionName,
                            value::ValueBlock* leftBlock,
                            value::ValueBlock* rightBlock,
                            BlockType bt = BlockType::HETEROGENEOUS);

    std::pair<std::vector<bool>, std::vector<bool>> naiveLogicalAndOr(
        std::unique_ptr<value::ValueBlock> leftBlock,
        std::unique_ptr<value::ValueBlock> rightBlock) {
        auto left = leftBlock->extract();
        auto right = rightBlock->extract();
        ASSERT_EQ(left.count(), right.count());

        std::vector<bool> andRes;
        std::vector<bool> orRes;

        for (size_t i = 0; i < left.count(); ++i) {
            ASSERT_EQ(left.tags()[i], value::TypeTags::Boolean);
            ASSERT_EQ(right.tags()[i], value::TypeTags::Boolean);
            auto leftBool = value::bitcastTo<bool>(left.vals()[i]);
            auto rightBool = value::bitcastTo<bool>(right.vals()[i]);
            andRes.push_back(leftBool && rightBool);
            orRes.push_back(leftBool || rightBool);
        }

        return std::pair{andRes, orRes};
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

TEST_F(SBEBlockExpressionTest, BlockExistsMonoHomogeneousTest) {
    value::ViewOfValueAccessor blockAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);
    auto existsExpr =
        sbe::makeE<sbe::EFunction>("valueBlockExists", sbe::makeEs(makeE<EVariable>(blockSlot)));
    auto compiledExpr = compileExpression(*existsExpr);

    {
        value::Int32Block block;
        block.push_back(42);
        block.push_back(43);
        block.push_back(44);
        block.pushNothing();
        block.push_back(46);

        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        assertBlockOfBool(runTag, runVal, std::vector{true, true, true, false, true});
    }

    {
        value::Int32Block denseBlock;
        denseBlock.push_back(1);
        denseBlock.push_back(2);

        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&denseBlock));
        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        assertBlockOfBool(runTag, runVal, std::vector{true, true});
    }

    {
        value::Int32Block sparseBlock;
        sparseBlock.pushNothing();
        sparseBlock.pushNothing();

        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&sparseBlock));
        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        assertBlockOfBool(runTag, runVal, std::vector{false, false});
    }

    {
        auto [blockTag, blockVal] = value::makeNewString("MonoBlock string"_sd);
        value::ValueGuard blockInputGuard(blockTag, blockVal);
        value::MonoBlock monoBlock(2, blockTag, blockVal);

        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&monoBlock));
        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        assertBlockOfBool(runTag, runVal, std::vector{true, true});
    }

    {
        value::MonoBlock monoBlock(2, value::TypeTags::Nothing, value::Value{0u});

        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&monoBlock));
        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        assertBlockOfBool(runTag, runVal, std::vector{false, false});
    }
}

TEST_F(SBEBlockExpressionTest, BlockFillEmptyShallowTest) {
    value::OwnedValueAccessor fillAccessor;
    auto fillSlot = bindAccessor(&fillAccessor);
    value::ViewOfValueAccessor blockAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);
    auto fillEmptyExpr = sbe::makeE<sbe::EFunction>(
        "valueBlockFillEmpty",
        sbe::makeEs(makeE<EVariable>(blockSlot), makeE<EVariable>(fillSlot)));
    auto compiledExpr = compileExpression(*fillEmptyExpr);

    auto [fillTag, fillVal] = makeInt32(45);
    fillAccessor.reset(fillTag, fillVal);

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

    assertBlockEq(
        runTag,
        runVal,
        std::vector{makeInt32(42), makeInt32(43), makeInt32(44), makeInt32(45), makeInt32(46)});
}

TEST_F(SBEBlockExpressionTest, BlockFillEmptyDeepTest) {
    value::ViewOfValueAccessor blockAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);
    value::OwnedValueAccessor fillAccessor;
    auto fillSlot = bindAccessor(&fillAccessor);
    auto fillEmptyExpr = sbe::makeE<sbe::EFunction>(
        "valueBlockFillEmpty",
        sbe::makeEs(makeE<EVariable>(blockSlot), makeE<EVariable>(fillSlot)));
    auto compiledExpr = compileExpression(*fillEmptyExpr);

    auto [fillTag, fillVal] = value::makeNewString("Replacement for missing value"_sd);
    fillAccessor.reset(true, fillTag, fillVal);

    value::HeterogeneousBlock block;
    block.push_back(value::makeNewString("First string"_sd));
    block.push_back(makeNothing());
    block.push_back(value::makeNewString("Second string"_sd));
    block.push_back(value::makeNewString("Third string"_sd));
    block.push_back(value::makeNewString("tinystr"_sd));  // Stored as shallow StringSmall type

    blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(&block));
    auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(runTag, runVal);

    auto extracted = block.extract();
    assertBlockEq(
        runTag,
        runVal,
        std::vector{extracted[0], {fillTag, fillVal}, extracted[2], extracted[3], extracted[4]});
}

TEST_F(SBEBlockExpressionTest, BlockFillEmptyNothingTest) {
    value::OwnedValueAccessor fillAccessor;
    auto fillSlot = bindAccessor(&fillAccessor);
    value::ViewOfValueAccessor blockAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);
    auto fillEmptyExpr = sbe::makeE<sbe::EFunction>(
        "valueBlockFillEmpty",
        sbe::makeEs(makeE<EVariable>(blockSlot), makeE<EVariable>(fillSlot)));
    auto compiledExpr = compileExpression(*fillEmptyExpr);

    auto [fillTag, fillVal] = makeNothing();
    fillAccessor.reset(fillTag, fillVal);

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

    assertBlockEq(
        runTag,
        runVal,
        std::vector{makeInt32(42), makeInt32(43), makeInt32(44), makeNothing(), makeInt32(46)});
}

TEST_F(SBEBlockExpressionTest, BlockFillEmptyMonoHomogeneousTest) {
    value::OwnedValueAccessor fillAccessor;
    auto fillSlot = bindAccessor(&fillAccessor);
    value::ViewOfValueAccessor blockAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);
    auto fillEmptyExpr = sbe::makeE<sbe::EFunction>(
        "valueBlockFillEmpty",
        sbe::makeEs(makeE<EVariable>(blockSlot), makeE<EVariable>(fillSlot)));
    auto compiledExpr = compileExpression(*fillEmptyExpr);

    value::Int32Block block;
    block.push_back(42);
    block.push_back(43);
    block.push_back(44);
    block.pushNothing();
    block.push_back(46);

    {
        // Matching type
        auto [fillTag, fillVal] = makeInt32(45);
        fillAccessor.reset(fillTag, fillVal);

        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        assertBlockEq(
            runTag,
            runVal,
            std::vector{makeInt32(42), makeInt32(43), makeInt32(44), makeInt32(45), makeInt32(46)});
    }

    {
        // Deep replacement value of a different type.
        auto [fillTag, fillVal] = value::makeNewString("Replacement for missing value"_sd);
        fillAccessor.reset(true, fillTag, fillVal);

        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        assertBlockEq(
            runTag,
            runVal,
            std::vector{
                makeInt32(42), makeInt32(43), makeInt32(44), {fillTag, fillVal}, makeInt32(46)});
    }

    {
        auto [blockTag, blockVal] = value::makeNewString("MonoBlock string"_sd);
        value::ValueGuard blockInputGuard(blockTag, blockVal);
        value::MonoBlock monoBlock(2, blockTag, blockVal);

        auto [fillTag, fillVal] = makeInt32(0);
        fillAccessor.reset(true, fillTag, fillVal);

        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&monoBlock));
        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        auto extracted = monoBlock.extract();
        assertBlockEq(runTag, runVal, std::vector{extracted[0], extracted[1]});
    }

    {
        value::MonoBlock monoBlock(2, value::TypeTags::Nothing, value::Value{0u});

        auto [fillTag, fillVal] = value::makeNewString("MonoBlock string"_sd);
        fillAccessor.reset(true, fillTag, fillVal);

        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&monoBlock));
        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        assertBlockEq(
            runTag, runVal, std::vector{std::pair(fillTag, fillVal), std::pair(fillTag, fillVal)});
    }
}

TEST_F(SBEBlockExpressionTest, BlockFillEmptyBlockTest) {
    value::ViewOfValueAccessor fillAccessor;
    auto fillSlot = bindAccessor(&fillAccessor);
    value::ViewOfValueAccessor blockAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);
    auto fillEmptyExpr = sbe::makeE<sbe::EFunction>(
        "valueBlockFillEmptyBlock",
        sbe::makeEs(makeE<EVariable>(blockSlot), makeE<EVariable>(fillSlot)));
    auto compiledExpr = compileExpression(*fillEmptyExpr);

    value::HeterogeneousBlock fillBlock;
    fillBlock.push_back(makeInt32(742));
    fillBlock.push_back(makeInt32(743));
    fillBlock.push_back(makeInt32(744));
    fillBlock.push_back(makeInt32(745));
    fillBlock.push_back(makeInt32(746));

    fillAccessor.reset(sbe::value::TypeTags::valueBlock,
                       value::bitcastFrom<value::ValueBlock*>(&fillBlock));

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

    assertBlockEq(
        runTag,
        runVal,
        std::vector{makeInt32(42), makeInt32(43), makeInt32(44), makeInt32(745), makeInt32(46)});
}

TEST_F(SBEBlockExpressionTest, BlockCountTest) {
    auto testCount = [&](std::vector<bool> bitsetData, size_t count) {
        value::ViewOfValueAccessor bitsetAccessor;
        auto bitsetSlot = bindAccessor(&bitsetAccessor);

        auto bitset = makeHeterogeneousBoolBlock(bitsetData);
        bitsetAccessor.reset(sbe::value::TypeTags::valueBlock,
                             value::bitcastFrom<value::ValueBlock*>(bitset.get()));

        auto compiledExpr = sbe::makeE<sbe::EFunction>("valueBlockAggCount",
                                                       sbe::makeEs(makeE<EVariable>(bitsetSlot)));
        auto compiledCountExpr = compileExpression(*compiledExpr);

        auto [runTag, runVal] = runCompiledExpression(compiledCountExpr.get());

        ASSERT_EQ(runTag, value::TypeTags::NumberInt64);
        auto expectedCount = makeInt64(count);
        auto [compTag, compVal] =
            value::compareValue(runTag, runVal, expectedCount.first, expectedCount.second);

        ASSERT_EQ(compTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(value::bitcastTo<int32_t>(compVal), 0);
    };

    testCount({false, false, false, false, false, false}, 0);
    testCount({true, false, true, true, false, true}, 4);
    testCount({true, true, true, true, true, true}, 6);
}

TEST_F(SBEBlockExpressionTest, BlockSumTest) {
    auto testSum = [&](std::vector<std::pair<value::TypeTags, value::Value>> blockData,
                       std::vector<bool> bitsetData,
                       std::pair<value::TypeTags, value::Value> expectedResult) {
        ASSERT_EQ(blockData.size(), bitsetData.size());
        value::ValueGuard expectedResultGuard(expectedResult);

        value::ViewOfValueAccessor blockAccessor;
        value::ViewOfValueAccessor bitsetAccessor;
        auto blockSlot = bindAccessor(&blockAccessor);
        auto bitsetSlot = bindAccessor(&bitsetAccessor);

        value::HeterogeneousBlock block;
        for (auto&& p : blockData) {
            block.push_back(p);
        }
        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));

        auto bitset = makeHeterogeneousBoolBlock(bitsetData);
        bitsetAccessor.reset(sbe::value::TypeTags::valueBlock,
                             value::bitcastFrom<value::ValueBlock*>(bitset.get()));

        auto compiledExpr = sbe::makeE<sbe::EFunction>(
            "valueBlockAggSum",
            sbe::makeEs(makeE<EVariable>(bitsetSlot), makeE<EVariable>(blockSlot)));
        auto compiledCountExpr = compileExpression(*compiledExpr);

        auto [runTag, runVal] = runCompiledExpression(compiledCountExpr.get());
        value::ValueGuard guard(runTag, runVal);

        ASSERT_EQ(runTag, expectedResult.first);
        if (runTag != value::TypeTags::Nothing) {
            auto [compTag, compVal] =
                value::compareValue(runTag, runVal, expectedResult.first, expectedResult.second);

            ASSERT_EQ(compTag, value::TypeTags::NumberInt32);
            ASSERT_EQ(value::bitcastTo<int32_t>(compVal), 0);
        }
    };

    // Bitset is 0.
    testSum({makeNothing(), makeNothing(), makeNothing(), makeNothing()},
            {false, false, false, false},
            {value::TypeTags::Nothing, 0});
    // All values are nothing
    testSum({makeNothing(), makeNothing(), makeNothing()},
            {true, true, false},
            {value::TypeTags::Nothing, 0});
    // Only int32.
    testSum({makeInt32(1), makeNothing(), makeInt32(2), makeInt32(3), makeNothing(), makeInt32(4)},
            {false, false, true, true, false, true},
            {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(9)});
    // Put the int64 last for type promotion at the end.
    testSum({makeInt32(1), makeNothing(), makeInt32(2), makeInt32(3), makeNothing(), makeInt64(4)},
            {false, false, true, true, false, true},
            {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(9)});
    // Put the int64 first for early type promotion.
    testSum({makeInt64(1), makeNothing(), makeInt32(2), makeInt32(3), makeNothing(), makeInt32(4)},
            {true, false, true, true, false, true},
            {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10)});
    // Mix types with double.
    testSum({makeInt32(1), makeNothing(), makeDouble(2), makeInt32(3), makeNothing(), makeInt64(4)},
            {false, false, true, true, false, true},
            {value::TypeTags::NumberDouble, value::bitcastFrom<double>(9)});
    // Mix types with Decimal128.
    testSum(
        {makeInt32(1), makeNothing(), makeDouble(2), makeInt32(3), makeDecimal("50"), makeInt64(4)},
        {false, false, true, true, true, true},
        makeDecimal("59"));
    // Mix types with Nothing.
    testSum(
        {makeInt32(1), makeNothing(), makeDouble(2), makeInt32(3), makeDecimal("50"), makeInt64(4)},
        {false, true, true, true, true, true},
        makeDecimal("59"));
    // One Decimal128, to test for memory leaks.
    testSum({makeDecimal("50")}, {true}, makeDecimal("50"));
    // A few Decimal128 values.
    testSum({makeDecimal("50"),
             makeDecimal("50"),
             makeDecimal("50"),
             makeDecimal("50"),
             makeDecimal("50"),
             makeDecimal("50")},
            {false, true, true, true, true, true},
            makeDecimal("250"));
}

TEST_F(SBEBlockExpressionTest, BlockMinMaxTest) {
    value::ViewOfValueAccessor blockAccessor;
    value::ViewOfValueAccessor bitsetAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);
    auto bitsetSlot = bindAccessor(&bitsetAccessor);

    value::HeterogeneousBlock block;
    block.push_back(makeInt32(42));
    block.push_back(makeNothing());
    block.push_back(makeInt32(43));
    block.push_back(makeInt32(40));
    block.push_back(makeNothing());
    block.push_back(makeInt32(41));
    blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(&block));

    auto bitset = makeHeterogeneousBoolBlock({true, true, false, false, true, true});
    bitsetAccessor.reset(sbe::value::TypeTags::valueBlock,
                         value::bitcastFrom<value::ValueBlock*>(bitset.get()));

    {
        auto compiledExpr = sbe::makeE<sbe::EFunction>(
            "valueBlockAggMin",
            sbe::makeEs(makeE<EVariable>(bitsetSlot), makeE<EVariable>(blockSlot)));
        auto compiledMinExpr = compileExpression(*compiledExpr);

        auto [runTag, runVal] = runCompiledExpression(compiledMinExpr.get());
        value::ValueGuard guard(runTag, runVal);

        ASSERT_EQ(runTag, value::TypeTags::NumberInt32);
        auto expectedMin = makeInt32(41);
        auto [t, v] = value::compareValue(runTag, runVal, expectedMin.first, expectedMin.second);

        ASSERT_EQ(t, value::TypeTags::NumberInt32);
        ASSERT_EQ(value::bitcastTo<int32_t>(v), 0);
    }

    {
        auto compiledExpr = sbe::makeE<sbe::EFunction>(
            "valueBlockAggMax",
            sbe::makeEs(makeE<EVariable>(bitsetSlot), makeE<EVariable>(blockSlot)));
        auto compiledMinExpr = compileExpression(*compiledExpr);

        auto [runTag, runVal] = runCompiledExpression(compiledMinExpr.get());
        value::ValueGuard guard(runTag, runVal);

        ASSERT_EQ(runTag, value::TypeTags::NumberInt32);
        auto expectedMax = makeInt32(42);
        auto [t, v] = value::compareValue(runTag, runVal, expectedMax.first, expectedMax.second);

        ASSERT_EQ(t, value::TypeTags::NumberInt32);
        ASSERT_EQ(value::bitcastTo<int32_t>(v), 0);
    }
}

TEST_F(SBEBlockExpressionTest, BlockMinMaxDeepTest) {
    value::ViewOfValueAccessor blockAccessor;
    value::ViewOfValueAccessor bitsetAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);
    auto bitsetSlot = bindAccessor(&bitsetAccessor);

    value::HeterogeneousBlock block;
    block.push_back(value::makeNewString("zoom"_sd));  // TypeTags::StringSmall
    block.push_back(makeInt32(42));
    block.push_back(makeInt32(41));
    block.push_back(makeInt32(40));
    block.push_back(value::makeNewString("abcdefg"_sd));    // TypeTags::StringSmall
    block.push_back(value::makeNewString("abcdefgh"_sd));   // TypeTags::StringBig
    block.push_back(value::makeNewString("abcdefghi"_sd));  // TypeTags::StringBig
    block.push_back(makeNothing());
    blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(&block));

    auto bitset = makeHeterogeneousBoolBlock({false, true, true, false, true, true, false, true});
    bitsetAccessor.reset(sbe::value::TypeTags::valueBlock,
                         value::bitcastFrom<value::ValueBlock*>(bitset.get()));

    {
        auto compiledExpr = sbe::makeE<sbe::EFunction>(
            "valueBlockAggMin",
            sbe::makeEs(makeE<EVariable>(bitsetSlot), makeE<EVariable>(blockSlot)));
        auto compiledMinExpr = compileExpression(*compiledExpr);

        auto [runTag, runVal] = runCompiledExpression(compiledMinExpr.get());
        value::ValueGuard guard(runTag, runVal);

        ASSERT_EQ(runTag, value::TypeTags::NumberInt32);
        auto expectedMin = makeInt32(41);
        auto [t, v] = value::compareValue(runTag, runVal, expectedMin.first, expectedMin.second);

        ASSERT_EQ(t, value::TypeTags::NumberInt32);
        ASSERT_EQ(value::bitcastTo<int32_t>(v), 0);
    }

    {
        auto compiledExpr = sbe::makeE<sbe::EFunction>(
            "valueBlockAggMax",
            sbe::makeEs(makeE<EVariable>(bitsetSlot), makeE<EVariable>(blockSlot)));
        auto compiledMinExpr = compileExpression(*compiledExpr);

        auto [runTag, runVal] = runCompiledExpression(compiledMinExpr.get());
        value::ValueGuard guard(runTag, runVal);

        ASSERT_EQ(runTag, value::TypeTags::StringBig);
        auto [maxTag, maxVal] = value::makeNewString("abcdefgh"_sd);
        value::ValueGuard maxGuard(maxTag, maxVal);
        auto [t, v] = value::compareValue(runTag, runVal, maxTag, maxVal);

        ASSERT_EQ(t, value::TypeTags::NumberInt32);
        ASSERT_EQ(value::bitcastTo<int32_t>(v), 0);
    }
}

TEST_F(SBEBlockExpressionTest, BlockApplyLambdaTest) {
    value::ViewOfValueAccessor blockAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);

    FrameId frame = 10;
    // Multiply each value by two.
    auto expr = makeE<sbe::EFunction>(
        "valueBlockApplyLambda",
        sbe::makeEs(makeC(makeNothing()),
                    makeE<EVariable>(blockSlot),
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

TEST_F(SBEBlockExpressionTest, BlockApplyMaskedLambdaTest) {
    value::ViewOfValueAccessor blockAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);
    value::ViewOfValueAccessor maskAccessor;
    auto maskSlot = bindAccessor(&maskAccessor);

    FrameId frame = 10;
    // Multiply each value by two.
    auto expr = makeE<sbe::EFunction>(
        "valueBlockApplyLambda",
        sbe::makeEs(makeE<EVariable>(maskSlot),
                    makeE<EVariable>(blockSlot),
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

    auto mask = makeHeterogeneousBoolBlock({true, false, true, true, false});
    maskAccessor.reset(sbe::value::TypeTags::valueBlock,
                       value::bitcastFrom<value::ValueBlock*>(mask.get()));

    auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(runTag, runVal);

    assertBlockEq(runTag,
                  runVal,
                  std::vector<std::pair<value::TypeTags, value::Value>>{
                      makeInt32(84), makeNothing(), makeInt32(88), makeNothing(), makeNothing()});
}

void SBEBlockExpressionTest::testBlockLogicalOp(EPrimBinary::Op scalarOp,
                                                StringData blockFunctionName,
                                                value::ValueBlock* leftBlock,
                                                value::ValueBlock* rightBlock,
                                                BlockType bt) {
    auto leftValuesNum = leftBlock->count();
    auto rightValuesNum = rightBlock->count();
    ASSERT_EQ(leftValuesNum, rightValuesNum) << " left block has " << leftValuesNum
                                             << " values while right block has " << rightValuesNum;

    value::ViewOfValueAccessor leftAccessor;
    value::ViewOfValueAccessor rightAccessor;
    value::ViewOfValueAccessor leftScalarAccessor;
    value::ViewOfValueAccessor rightScalarAccessor;

    auto leftBlockSlot = bindAccessor(&leftAccessor);
    auto rightBlockSlot = bindAccessor(&rightAccessor);
    auto leftScalarSlot = bindAccessor(&leftScalarAccessor);
    auto rightScalarSlot = bindAccessor(&rightScalarAccessor);

    leftAccessor.reset(sbe::value::TypeTags::valueBlock,
                       value::bitcastFrom<value::ValueBlock*>(leftBlock));
    rightAccessor.reset(sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(rightBlock));

    {
        // Run the block expresseion.
        auto blockExpr = makeE<sbe::EFunction>(
            blockFunctionName,
            sbe::makeEs(makeE<EVariable>(leftBlockSlot), makeE<EVariable>(rightBlockSlot)));
        auto compiledBlockExpr = compileExpression(*blockExpr);
        auto [resTag, resVal] = runCompiledExpression(compiledBlockExpr.get());
        value::ValueGuard resGuard(resTag, resVal);

        // Compare the results against the result of the scalar operation.
        auto scalarExpr = sbe::makeE<sbe::EPrimBinary>(
            scalarOp, makeE<EVariable>(leftScalarSlot), makeE<EVariable>(rightScalarSlot));


        std::vector<std::pair<value::TypeTags, value::Value>> scalarResults;
        scalarResults.reserve(leftValuesNum);

        auto leftExtractedValues = leftBlock->extract();
        auto rightExtractedValues = rightBlock->extract();

        for (size_t i = 0; i < leftValuesNum; ++i) {
            leftScalarAccessor.reset(leftExtractedValues.tags()[i], leftExtractedValues.vals()[i]);
            rightScalarAccessor.reset(rightExtractedValues.tags()[i],
                                      rightExtractedValues.vals()[i]);

            auto compiledScalarExpr = compileExpression(*scalarExpr);
            scalarResults.push_back(runCompiledExpression(compiledScalarExpr.get()));
        }

        assertBlockEq(resTag, resVal, scalarResults);

        if (BlockType::MONOBLOCK == bt) {  // It should be a MonoBlock
            auto* block = value::bitcastTo<value::ValueBlock*>(resVal);
            ASSERT(block->as<value::MonoBlock>());
        }

        if (BlockType::BOOLBLOCK == bt) {  // It should be a BoolBlock
            auto* block = value::bitcastTo<value::ValueBlock*>(resVal);
            ASSERT(block->as<value::BoolBlock>());
        }
    }
}

TEST_F(SBEBlockExpressionTest, BlockHeterogeneousLogicAndOrTest) {
    size_t monoBlockSize = 5;

    auto [fTag, fVal] = makeBool(false);
    std::unique_ptr<value::ValueBlock> falseMonoblock =
        std::make_unique<value::MonoBlock>(monoBlockSize, fTag, fVal);

    auto [tTag, tVal] = makeBool(true);
    std::unique_ptr<value::ValueBlock> trueMonoblock =
        std::make_unique<value::MonoBlock>(monoBlockSize, tTag, tVal);

    auto [lTag, lVal] = makeDouble(2.5);
    std::unique_ptr<value::ValueBlock> leftMonoblock =
        std::make_unique<value::MonoBlock>(monoBlockSize, lTag, lVal);

    auto [rTag, rVal] = value::makeSmallString("small");
    value::ValueGuard sguard(rTag, rVal);
    std::unique_ptr<value::ValueBlock> rightMonoblock =
        std::make_unique<value::MonoBlock>(monoBlockSize, rTag, rVal);

    value::HeterogeneousBlock leftBlockValues;
    leftBlockValues.push_back(makeBool(true));
    leftBlockValues.push_back(makeInt32(43));
    leftBlockValues.push_back(makeBool(false));
    leftBlockValues.push_back(makeNothing());
    leftBlockValues.push_back(makeBool(true));

    value::HeterogeneousBlock rightBlockValues;
    rightBlockValues.push_back(makeBool(true));
    rightBlockValues.push_back(makeBool(true));
    rightBlockValues.push_back(makeBool(false));
    rightBlockValues.push_back(makeBool(true));
    rightBlockValues.push_back(makeBool(false));

    {  // AND
        testBlockLogicalOp(sbe::EPrimBinary::logicAnd,
                           "valueBlockLogicalAnd",
                           &leftBlockValues,
                           &rightBlockValues);
    }

    {
        // AND reverse inputs.
        testBlockLogicalOp(sbe::EPrimBinary::logicAnd,
                           "valueBlockLogicalAnd",
                           &rightBlockValues,
                           &leftBlockValues);
    }

    {
        // OR
        testBlockLogicalOp(
            sbe::EPrimBinary::logicOr, "valueBlockLogicalOr", &leftBlockValues, &rightBlockValues);
    }

    {
        // OR reverse inputs.
        testBlockLogicalOp(sbe::EPrimBinary::logicOr,
                           "valueBlockLogicalOr",
                           &rightBlockValues,
                           &leftBlockValues,
                           BlockType::BOOLBLOCK);
    }

    rightBlockValues.clear();
    rightBlockValues.push_back(makeBool(false));
    rightBlockValues.push_back(makeDouble(12.5));
    rightBlockValues.push_back(makeNothing());
    rightBlockValues.push_back(makeBool(true));
    rightBlockValues.push_back(makeBool(true));

    {
        // AND
        testBlockLogicalOp(sbe::EPrimBinary::logicAnd,
                           "valueBlockLogicalAnd",
                           &rightBlockValues,
                           &leftBlockValues);
    }

    {
        // AND reverse inputs.
        testBlockLogicalOp(sbe::EPrimBinary::logicAnd,
                           "valueBlockLogicalAnd",
                           &rightBlockValues,
                           &leftBlockValues);
    }

    {
        // OR
        testBlockLogicalOp(
            sbe::EPrimBinary::logicOr, "valueBlockLogicalOr", &leftBlockValues, &rightBlockValues);
    }

    {
        // OR reverse inputs.
        testBlockLogicalOp(
            sbe::EPrimBinary::logicOr, "valueBlockLogicalOr", &rightBlockValues, &leftBlockValues);
    }

    // Monoblocks
    {
        // AND with false monoblock.
        testBlockLogicalOp(sbe::EPrimBinary::logicAnd,
                           "valueBlockLogicalAnd",
                           &leftBlockValues,
                           falseMonoblock.get());
    }

    {
        // AND with false monoblock reverse inputs.
        testBlockLogicalOp(sbe::EPrimBinary::logicAnd,
                           "valueBlockLogicalAnd",
                           falseMonoblock.get(),
                           &leftBlockValues,
                           BlockType::MONOBLOCK);
    }

    {
        // OR with false monoblock.
        testBlockLogicalOp(sbe::EPrimBinary::logicOr,
                           "valueBlockLogicalOr",
                           &leftBlockValues,
                           falseMonoblock.get());
    }

    {
        // OR with false monoblock reverse inputs.
        testBlockLogicalOp(sbe::EPrimBinary::logicOr,
                           "valueBlockLogicalOr",
                           falseMonoblock.get(),
                           &leftBlockValues);
    }

    {
        // AND with true monoblock.
        testBlockLogicalOp(sbe::EPrimBinary::logicAnd,
                           "valueBlockLogicalAnd",
                           &leftBlockValues,
                           trueMonoblock.get());
    }

    {
        // AND with true monoblock reverse inputs.
        testBlockLogicalOp(sbe::EPrimBinary::logicAnd,
                           "valueBlockLogicalAnd",
                           trueMonoblock.get(),
                           &leftBlockValues);
    }

    {
        // OR with true monoblock.
        testBlockLogicalOp(sbe::EPrimBinary::logicOr,
                           "valueBlockLogicalOr",
                           &leftBlockValues,
                           trueMonoblock.get());
    }

    {
        // OR with true monoblock reverse inputs.
        testBlockLogicalOp(sbe::EPrimBinary::logicOr,
                           "valueBlockLogicalOr",
                           trueMonoblock.get(),
                           &leftBlockValues,
                           BlockType::MONOBLOCK);
    }

    {
        // AND true monoblock and non boolean monoblock.
        testBlockLogicalOp(sbe::EPrimBinary::logicAnd,
                           "valueBlockLogicalAnd",
                           leftMonoblock.get(),
                           trueMonoblock.get(),
                           BlockType::MONOBLOCK);
    }

    {
        // AND true monoblock and non boolean monoblock reversed.
        testBlockLogicalOp(sbe::EPrimBinary::logicAnd,
                           "valueBlockLogicalAnd",
                           trueMonoblock.get(),
                           rightMonoblock.get(),
                           BlockType::MONOBLOCK);
    }

    {
        // AND false monoblock and non boolean monoblock.
        testBlockLogicalOp(sbe::EPrimBinary::logicAnd,
                           "valueBlockLogicalAnd",
                           leftMonoblock.get(),
                           falseMonoblock.get(),
                           BlockType::MONOBLOCK);
    }

    {
        // AND false monoblock and non boolean monoblock reversed.
        testBlockLogicalOp(sbe::EPrimBinary::logicAnd,
                           "valueBlockLogicalAnd",
                           falseMonoblock.get(),
                           rightMonoblock.get(),
                           BlockType::MONOBLOCK);
    }

    {
        // AND non boolean monoblocks.
        testBlockLogicalOp(sbe::EPrimBinary::logicAnd,
                           "valueBlockLogicalAnd",
                           leftMonoblock.get(),
                           rightMonoblock.get(),
                           BlockType::MONOBLOCK);
    }

    {
        // OR true monoblock and non boolean monoblock.
        testBlockLogicalOp(sbe::EPrimBinary::logicOr,
                           "valueBlockLogicalOr",
                           leftMonoblock.get(),
                           trueMonoblock.get(),
                           BlockType::MONOBLOCK);
    }

    {
        // OR true monoblock and non boolean monoblock reversed.
        testBlockLogicalOp(sbe::EPrimBinary::logicOr,
                           "valueBlockLogicalOr",
                           trueMonoblock.get(),
                           rightMonoblock.get(),
                           BlockType::MONOBLOCK);
    }

    {
        // OR false monoblock and non boolean monoblock.
        testBlockLogicalOp(sbe::EPrimBinary::logicOr,
                           "valueBlockLogicalOr",
                           leftMonoblock.get(),
                           falseMonoblock.get(),
                           BlockType::MONOBLOCK);
    }

    {
        // OR false monoblock and non boolean monoblock reversed.
        testBlockLogicalOp(sbe::EPrimBinary::logicOr,
                           "valueBlockLogicalOr",
                           falseMonoblock.get(),
                           rightMonoblock.get(),
                           BlockType::MONOBLOCK);
    }

    {
        // OR non boolean monoblocks.
        testBlockLogicalOp(sbe::EPrimBinary::logicOr,
                           "valueBlockLogicalOr",
                           leftMonoblock.get(),
                           rightMonoblock.get(),
                           BlockType::MONOBLOCK);
    }

    leftBlockValues.clear();
    leftBlockValues.push_back(value::makeSmallString("small"));
    leftBlockValues.push_back(makeNothing());
    leftBlockValues.push_back(makeNothing());
    leftBlockValues.push_back(makeInt32(125));
    leftBlockValues.push_back(makeDecimal("5.5"));

    rightBlockValues.clear();
    rightBlockValues.push_back(makeNothing());
    rightBlockValues.push_back(makeInt32(124));
    rightBlockValues.push_back(value::makeSmallString("another"));
    rightBlockValues.push_back(makeNothing());
    rightBlockValues.push_back(makeNothing());

    {
        // AND non boolean produces Nothing monoblock.
        testBlockLogicalOp(sbe::EPrimBinary::logicAnd,
                           "valueBlockLogicalAnd",
                           &leftBlockValues,
                           &rightBlockValues,
                           BlockType::MONOBLOCK);
    }

    {
        // OR non boolean produces Nothing monoblock.
        testBlockLogicalOp(sbe::EPrimBinary::logicOr,
                           "valueBlockLogicalOr",
                           &leftBlockValues,
                           &rightBlockValues,
                           BlockType::MONOBLOCK);
    }

    // Bool block
    leftBlockValues.clear();
    leftBlockValues.push_back(makeBool(false));
    leftBlockValues.push_back(makeBool(true));
    leftBlockValues.push_back(makeBool(false));
    leftBlockValues.push_back(makeBool(true));
    leftBlockValues.push_back(makeBool(true));

    rightBlockValues.clear();
    rightBlockValues.push_back(makeBool(false));
    rightBlockValues.push_back(makeBool(false));
    rightBlockValues.push_back(makeBool(true));
    rightBlockValues.push_back(makeBool(true));
    rightBlockValues.push_back(makeBool(false));

    {
        // AND BoolBlock
        testBlockLogicalOp(sbe::EPrimBinary::logicAnd,
                           "valueBlockLogicalAnd",
                           &leftBlockValues,
                           &rightBlockValues,
                           BlockType::BOOLBLOCK);
    }

    {
        // OR BoolBlock.
        testBlockLogicalOp(sbe::EPrimBinary::logicOr,
                           "valueBlockLogicalOr",
                           &leftBlockValues,
                           &rightBlockValues,
                           BlockType::BOOLBLOCK);
    }
}

TEST_F(SBEBlockExpressionTest, BlockLogicAndOrTest) {
    value::ViewOfValueAccessor blockAccessorLeft;
    value::ViewOfValueAccessor blockAccessorRight;
    value::ViewOfValueAccessor falseMonoBlockAccessor;
    value::ViewOfValueAccessor trueMonoBlockAccessor;
    auto blockLeftSlot = bindAccessor(&blockAccessorLeft);
    auto blockRightSlot = bindAccessor(&blockAccessorRight);
    auto falseMonoBlockSlot = bindAccessor(&falseMonoBlockAccessor);
    auto trueMonoBlockSlot = bindAccessor(&trueMonoBlockAccessor);

    auto leftBlock = makeHeterogeneousBoolBlock({true, false, true, false});
    blockAccessorLeft.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(leftBlock.get()));

    auto rightBlock = makeHeterogeneousBoolBlock({true, true, false, false});
    blockAccessorRight.reset(sbe::value::TypeTags::valueBlock,
                             value::bitcastFrom<value::ValueBlock*>(rightBlock.get()));

    auto [fTag, fVal] = makeBool(false);
    std::unique_ptr<value::ValueBlock> falseMonoBlock =
        std::make_unique<value::MonoBlock>(leftBlock->count(), fTag, fVal);
    falseMonoBlockAccessor.reset(sbe::value::TypeTags::valueBlock,
                                 value::bitcastFrom<value::ValueBlock*>(falseMonoBlock.get()));

    auto [tTag, tVal] = makeBool(true);
    std::unique_ptr<value::ValueBlock> trueMonoBlock =
        std::make_unique<value::MonoBlock>(leftBlock->count(), tTag, tVal);
    trueMonoBlockAccessor.reset(sbe::value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(trueMonoBlock.get()));

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

    {
        // MonoBlock test
        std::vector<value::SlotId> blockSlots{blockLeftSlot, falseMonoBlockSlot, trueMonoBlockSlot};
        std::vector<std::unique_ptr<value::ValueBlock>> kBlocks;
        kBlocks.push_back(leftBlock->clone());
        kBlocks.push_back(falseMonoBlock->clone());
        kBlocks.push_back(trueMonoBlock->clone());

        for (size_t i = 0; i < blockSlots.size(); ++i) {
            for (size_t j = 0; j < blockSlots.size(); ++j) {
                auto andExpr = makeE<sbe::EFunction>(
                    "valueBlockLogicalAnd",
                    sbe::makeEs(makeE<EVariable>(blockSlots[i]), makeE<EVariable>(blockSlots[j])));
                auto compiledAndExpr = compileExpression(*andExpr);

                auto [andTag, andVal] = runCompiledExpression(compiledAndExpr.get());
                value::ValueGuard andGuard(andTag, andVal);

                auto orExpr = makeE<sbe::EFunction>(
                    "valueBlockLogicalOr",
                    sbe::makeEs(makeE<EVariable>(blockSlots[i]), makeE<EVariable>(blockSlots[j])));
                auto compiledOrExpr = compileExpression(*orExpr);

                auto [orTag, orVal] = runCompiledExpression(compiledOrExpr.get());
                value::ValueGuard orGuard(orTag, orVal);

                auto [andNaive, orNaive] =
                    naiveLogicalAndOr(kBlocks[i]->clone(), kBlocks[j]->clone());

                assertBlockOfBool(andTag, andVal, andNaive);
                assertBlockOfBool(orTag, orVal, orNaive);
            }
        }
    }

    {
        // BoolBlock test
        value::ViewOfValueAccessor boolBlockAccessorLeft;
        value::ViewOfValueAccessor boolBlockAccessorRight;
        auto boolBlockLeftSlot = bindAccessor(&boolBlockAccessorLeft);
        auto boolBlockRightSlot = bindAccessor(&boolBlockAccessorRight);

        auto leftBoolBlock = makeBoolBlock({true, false, true, false});
        boolBlockAccessorLeft.reset(sbe::value::TypeTags::valueBlock,
                                    value::bitcastFrom<value::ValueBlock*>(leftBoolBlock.get()));

        auto rightBoolBlock = makeBoolBlock({true, true, false, false});
        boolBlockAccessorRight.reset(sbe::value::TypeTags::valueBlock,
                                     value::bitcastFrom<value::ValueBlock*>(rightBoolBlock.get()));

        auto andExpr = makeE<sbe::EFunction>(
            "valueBlockLogicalAnd",
            sbe::makeEs(makeE<EVariable>(boolBlockLeftSlot), makeE<EVariable>(boolBlockRightSlot)));
        auto compiledAndExpr = compileExpression(*andExpr);

        auto [andTag, andVal] = runCompiledExpression(compiledAndExpr.get());
        value::ValueGuard andGuard(andTag, andVal);

        auto orExpr = makeE<sbe::EFunction>(
            "valueBlockLogicalOr",
            sbe::makeEs(makeE<EVariable>(boolBlockLeftSlot), makeE<EVariable>(boolBlockRightSlot)));
        auto compiledOrExpr = compileExpression(*orExpr);

        auto [orTag, orVal] = runCompiledExpression(compiledOrExpr.get());
        value::ValueGuard orGuard(orTag, orVal);

        assertBlockOfBool(andTag, andVal, {true, false, false, false});
        assertBlockOfBool(orTag, orVal, {true, true, true, false});

        // Test HeterogeneousBlock fallback when applying the op to a bool block on one side and
        // heterogeneous on the other.
        auto heterogeneousAndExpr = makeE<sbe::EFunction>(
            "valueBlockLogicalAnd",
            sbe::makeEs(makeE<EVariable>(blockLeftSlot), makeE<EVariable>(boolBlockRightSlot)));
        auto compiledHeterogeneousAndExpr = compileExpression(*andExpr);

        auto [andHeterogeneousTag, andHeterogeneousVal] =
            runCompiledExpression(compiledHeterogeneousAndExpr.get());
        value::ValueGuard andHeterogeneousGuard(andHeterogeneousTag, andHeterogeneousVal);

        auto heretergeneousOrExpr = makeE<sbe::EFunction>(
            "valueBlockLogicalOr",
            sbe::makeEs(makeE<EVariable>(blockLeftSlot), makeE<EVariable>(boolBlockRightSlot)));
        auto compiledHeterogeneousOrExpr = compileExpression(*orExpr);

        auto [orHeterogeneousTag, orHeterogeneousVal] =
            runCompiledExpression(compiledHeterogeneousOrExpr.get());
        value::ValueGuard orHeterogeneousGuard(orHeterogeneousTag, orHeterogeneousVal);

        assertBlockOfBool(andHeterogeneousTag, andHeterogeneousVal, {true, false, false, false});
        assertBlockOfBool(orHeterogeneousTag, orHeterogeneousVal, {true, true, true, false});
    }
}

void SBEBlockExpressionTest::testFoldF(std::vector<std::pair<value::TypeTags, value::Value>> vals,
                                       std::vector<int32_t> filterPosInfo,
                                       std::vector<bool> expectedResult) {
    value::ViewOfValueAccessor valBlockAccessor;
    value::ViewOfValueAccessor cellBlockAccessor;
    auto valBlockSlot = bindAccessor(&valBlockAccessor);
    auto cellBlockSlot = bindAccessor(&cellBlockAccessor);

    auto materializedCellBlock = std::make_unique<value::MaterializedCellBlock>();
    materializedCellBlock->_deblocked = nullptr;  // This is never read by the test.
    materializedCellBlock->_filterPosInfo = filterPosInfo;

    auto valBlock = std::make_unique<value::HeterogeneousBlock>();
    for (auto v : vals) {
        valBlock->push_back(v);
    }
    valBlockAccessor.reset(sbe::value::TypeTags::valueBlock,
                           value::bitcastFrom<value::ValueBlock*>(valBlock.get()));
    cellBlockAccessor.reset(sbe::value::TypeTags::cellBlock,
                            value::bitcastFrom<value::CellBlock*>(materializedCellBlock.get()));

    {
        auto expr = makeE<sbe::EFunction>(
            "cellFoldValues_F",
            sbe::makeEs(makeE<EVariable>(valBlockSlot), makeE<EVariable>(cellBlockSlot)));
        auto compiledExpr = compileExpression(*expr);

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        assertBlockOfBool(runTag, runVal, expectedResult);
    }
}

TEST_F(SBEBlockExpressionTest, CellFoldFTest) {
    // For empty position info, FoldF() should act as an identity function.
    testFoldF({makeBool(true),
               makeBool(true),
               makeBool(false),
               makeBool(false),
               makeBool(true)},                 // Values.
              {},                               // Position info.
              {true, true, false, false, true}  // Expected result.
    );

    testFoldF({makeBool(true),
               makeBool(true),
               makeBool(false),
               makeBool(false),
               makeBool(true)},          // Values.
              {1, 1, 2, 1},              // Position info.
              {true, true, false, true}  // Expected result.
    );


    testFoldF({makeBool(true),
               makeNothing(),
               makeInt32(123),
               makeBool(false),
               makeBool(true)},                  // Values.
              {},                                // Position info.
              {true, false, false, false, true}  // Expected result.
    );


    //
    // Non-empty position info edge case tests.
    //

    testFoldF({},  // Values.
              {},  // Position info.
              {}   // Expected result.
    );

    testFoldF({makeBool(false)},  // Values.
              {1},                // Position info.
              {false}             // Expected result.
    );

    testFoldF({makeBool(true)},  // Values.
              {1},               // Position info.
              {true}             // Expected result.
    );

    testFoldF({makeNothing()},  // Values.
              {1},              // Position info.
              {false}           // Expected result.
    );

    testFoldF({},      // Values.
              {0},     // Position info.
              {false}  // Expected result.
    );

    testFoldF({makeBool(true),
               makeBool(true),
               makeBool(false),
               makeBool(false),
               makeBool(true)},  // Values.
              {5},               // Position info.
              {true}             // Expected result.
    );
    testFoldF({makeBool(true),
               makeBool(true),
               makeBool(false),
               makeBool(false),
               makeBool(true)},          // Values.
              {1, 1, 1, 2},              // Position info.
              {true, true, false, true}  // Expected result.
    );
    testFoldF({makeBool(false),
               makeBool(false),
               makeBool(false),
               makeBool(false),
               makeBool(false)},  // Values.
              {5},                // Position info.
              {false}             // Expected result.
    );
    testFoldF({makeBool(false),
               makeBool(false),
               makeBool(false),
               makeBool(false),
               makeBool(false)},  // Values.
              {2, 3},             // Position info.
              {false, false}      // Expected result.
    );
    testFoldF({makeBool(false), makeBool(false), makeBool(false), makeBool(true)},  // Values.
              {3, 1},        // Position info.
              {false, true}  // Expected result.
    );

    testFoldF({makeBool(false), makeBool(false), makeBool(false), makeBool(true)},  // Values.
              {0, 0, 0, 0, 3, 1},                        // Position info.
              {false, false, false, false, false, true}  // Expected result.
    );

    testFoldF({makeNothing(), makeBool(false), makeBool(false)},  // Values.
              {0, 1, 2, 0},                                       // Position info.
              {false, false, false, false}                        // Expected result.
    );

    testFoldF({},                    // Values.
              {0, 0, 0},             // Position info.
              {false, false, false}  // Expected result.
    );
}

template <typename BlockType, typename T>
std::unique_ptr<BlockType> makeTestHomogeneousBlock() {
    std::unique_ptr<BlockType> testHomogeneousBlock = std::make_unique<BlockType>();
    testHomogeneousBlock->push_back(value::bitcastFrom<T>(-1));
    testHomogeneousBlock->push_back(value::bitcastFrom<T>(0));
    testHomogeneousBlock->push_back(value::bitcastFrom<T>(1));
    testHomogeneousBlock->push_back(value::bitcastFrom<T>(std::numeric_limits<T>::min()));
    testHomogeneousBlock->push_back(value::bitcastFrom<T>(std::numeric_limits<T>::max()));
    testHomogeneousBlock->pushNothing();
    return testHomogeneousBlock;
}

std::unique_ptr<value::ValueBlock> makeTestInt32Block() {
    return makeTestHomogeneousBlock<value::Int32Block, int32_t>();
}

std::unique_ptr<value::ValueBlock> makeTestInt64Block() {
    return makeTestHomogeneousBlock<value::Int64Block, int64_t>();
}

std::unique_ptr<value::ValueBlock> makeTestDateBlock() {
    return makeTestHomogeneousBlock<value::DateBlock, int64_t>();
}

std::unique_ptr<value::ValueBlock> makeTestDoubleBlock() {
    auto testDoubleBlock = makeTestHomogeneousBlock<value::DoubleBlock, double>();
    testDoubleBlock->push_back(std::numeric_limits<double>::quiet_NaN());
    testDoubleBlock->push_back(std::numeric_limits<double>::signaling_NaN());
    return testDoubleBlock;
}

void SBEBlockExpressionTest::testCmpScalar(EPrimBinary::Op scalarOp,
                                           StringData cmpFunctionName,
                                           value::ValueBlock* valBlock) {
    invariant(valBlock != nullptr);

    value::ViewOfValueAccessor valBlockAccessor;
    value::ViewOfValueAccessor scalarAccessorLhs;
    value::ViewOfValueAccessor scalarAccessorRhs;
    auto valBlockSlot = bindAccessor(&valBlockAccessor);
    auto scalarSlotLhs = bindAccessor(&scalarAccessorLhs);
    auto scalarSlotRhs = bindAccessor(&scalarAccessorRhs);

    auto deblocked = valBlock->extract();

    valBlockAccessor.reset(sbe::value::TypeTags::valueBlock,
                           value::bitcastFrom<value::ValueBlock*>(valBlock));

    auto expr = makeE<sbe::EFunction>(
        cmpFunctionName,
        sbe::makeEs(makeE<EVariable>(valBlockSlot), makeE<EVariable>(scalarSlotRhs)));
    auto compiledExpr = compileExpression(*expr);

    auto scalarExpr = makeE<sbe::EPrimBinary>(
        scalarOp, makeE<EVariable>(scalarSlotLhs), makeE<EVariable>(scalarSlotRhs));
    auto compiledScalarExpr = compileExpression(*scalarExpr);

    for (size_t i = 0; i < deblocked.count(); ++i) {
        scalarAccessorRhs.reset(deblocked.tags()[i], deblocked.vals()[i]);

        // Run the block expression and get the result.
        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        ASSERT_EQ(runTag, value::TypeTags::valueBlock);
        auto* resultValBlock = value::getValueBlock(runVal);
        auto resultExtracted = resultValBlock->extract();

        ASSERT_EQ(resultExtracted.count(), deblocked.count());

        for (size_t j = 0; j < resultExtracted.count(); ++j) {
            // Determine the expected result.
            scalarAccessorLhs.reset(deblocked.tags()[j], deblocked.vals()[j]);
            auto [expectedTag, expectedVal] = runCompiledExpression(compiledScalarExpr.get());
            value::ValueGuard guard(expectedTag, expectedVal);


            auto [gotTag, gotVal] = resultExtracted[j];

            auto [cmpTag, cmpVal] = value::compareValue(gotTag, gotVal, expectedTag, expectedVal);
            ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32) << gotTag << " " << expectedTag;
            ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), 0)
                << "Comparing " << deblocked[i] << " " << deblocked[j] << " and got "
                << std::pair(gotTag, gotVal) << " expected " << std::pair(expectedTag, expectedVal);
        }
    }
}

TEST_F(SBEBlockExpressionTest, ValueBlockCmpScalarTest) {
    auto testValues = std::vector<std::pair<value::TypeTags, value::Value>>{
        makeNothing(),
        makeInt32(123),
        makeInt32(456),
        makeInt64(std::numeric_limits<int32_t>::min()),
        makeInt64(std::numeric_limits<int32_t>::max()),
        makeInt64(std::numeric_limits<int64_t>::min()),
        makeInt64(std::numeric_limits<int64_t>::max()),
        value::makeBigString("foobar"),
        value::makeBigString("baz"),
        makeDouble(999.0),
        makeDouble(111.0),
    };

    std::unique_ptr<value::HeterogeneousBlock> testBlock =
        std::make_unique<value::HeterogeneousBlock>();
    for (auto tv : testValues) {
        testBlock->push_back(tv);
    }

    testCmpScalar(EPrimBinary::greater, "valueBlockGtScalar", testBlock.get());
    testCmpScalar(EPrimBinary::greaterEq, "valueBlockGteScalar", testBlock.get());
    testCmpScalar(EPrimBinary::less, "valueBlockLtScalar", testBlock.get());
    testCmpScalar(EPrimBinary::lessEq, "valueBlockLteScalar", testBlock.get());
    testCmpScalar(EPrimBinary::eq, "valueBlockEqScalar", testBlock.get());
    testCmpScalar(EPrimBinary::neq, "valueBlockNeqScalar", testBlock.get());
}

TEST_F(SBEBlockExpressionTest, ValueBlockCmpScalarHomogeneousTest) {
    std::vector<std::unique_ptr<value::ValueBlock>> testBlocks;
    testBlocks.push_back(makeTestInt32Block());
    testBlocks.push_back(makeTestInt64Block());
    testBlocks.push_back(makeTestDateBlock());
    testBlocks.push_back(makeTestDoubleBlock());

    for (auto& block : testBlocks) {
        testCmpScalar(EPrimBinary::greater, "valueBlockGtScalar", block.get());
        testCmpScalar(EPrimBinary::greaterEq, "valueBlockGteScalar", block.get());
        testCmpScalar(EPrimBinary::less, "valueBlockLtScalar", block.get());
        testCmpScalar(EPrimBinary::lessEq, "valueBlockLteScalar", block.get());
        testCmpScalar(EPrimBinary::eq, "valueBlockEqScalar", block.get());
        testCmpScalar(EPrimBinary::neq, "valueBlockNeqScalar", block.get());
    }
}

void SBEBlockExpressionTest::testBlockBlockArithmeticOp(EPrimBinary::Op scalarOp,
                                                        StringData blockFunctionName,
                                                        value::ValueBlock* bitsetBlock,
                                                        value::ValueBlock* leftBlock,
                                                        value::ValueBlock* rightBlock,
                                                        bool monoBlockExpected) {
    value::ViewOfValueAccessor bitsetBlockAccessor;
    value::ViewOfValueAccessor leftBlockAccessor;
    value::ViewOfValueAccessor rightBlockAccessor;

    auto bitsetSlot = bindAccessor(&bitsetBlockAccessor);
    auto leftBlockSlot = bindAccessor(&leftBlockAccessor);
    auto rightBlockSlot = bindAccessor(&rightBlockAccessor);

    auto blockMathExpr = sbe::makeE<sbe::EFunction>(blockFunctionName,
                                                    sbe::makeEs(makeE<EVariable>(bitsetSlot),
                                                                makeE<EVariable>(leftBlockSlot),
                                                                makeE<EVariable>(rightBlockSlot)));

    auto blockCompiledExpr = compileExpression(*blockMathExpr);

    if (bitsetBlock) {
        bitsetBlockAccessor.reset(sbe::value::TypeTags::valueBlock,
                                  value::bitcastFrom<value::ValueBlock*>(bitsetBlock));
    } else {
        bitsetBlockAccessor.reset(sbe::value::TypeTags::Nothing, 0);
    }
    leftBlockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(leftBlock));
    rightBlockAccessor.reset(sbe::value::TypeTags::valueBlock,
                             value::bitcastFrom<value::ValueBlock*>(rightBlock));

    // run the block operation
    auto [resBlockTag, resBlockVal] = runCompiledExpression(blockCompiledExpr.get());
    value::ValueGuard guard(resBlockTag, resBlockVal);
    auto* resBlock = value::bitcastTo<value::ValueBlock*>(resBlockVal);
    auto resBlockExtractedValues = resBlock->extract();

    ASSERT_EQ(resBlockTag, value::TypeTags::valueBlock);
    if (monoBlockExpected) {
        ASSERT_TRUE(resBlock->as<value::MonoBlock>());
    }

    // run the same operations using the scalar version of the operation
    auto leftExtractedValues = leftBlock->extract();
    auto rightExtractedValues = rightBlock->extract();
    auto resNum = leftExtractedValues.count();

    ASSERT_EQ(resBlockExtractedValues.count(), resNum);

    value::ViewOfValueAccessor leftScalarAccessor;
    value::ViewOfValueAccessor rightScalarAccessor;

    auto leftScalarSlot = bindAccessor(&leftScalarAccessor);
    auto rightScalarSlot = bindAccessor(&rightScalarAccessor);

    auto scalarMathExpr = sbe::makeE<sbe::EPrimBinary>(
        scalarOp, makeE<EVariable>(leftScalarSlot), makeE<EVariable>(rightScalarSlot));

    auto scalarCompiledExpr = compileExpression(*scalarMathExpr);

    if (bitsetBlock) {
        auto bitsetExtractedValues = bitsetBlock->extract();
        for (size_t i = 0; i < resNum; ++i) {
            if (bitsetExtractedValues.tags()[i] != value::TypeTags::Boolean ||
                !value::bitcastTo<bool>(bitsetExtractedValues.vals()[i])) {
                // skip
                continue;
            }

            leftScalarAccessor.reset(leftExtractedValues.tags()[i], leftExtractedValues.vals()[i]);
            rightScalarAccessor.reset(rightExtractedValues.tags()[i],
                                      rightExtractedValues.vals()[i]);
            auto [scalarTag, scalarVal] = runCompiledExpression(scalarCompiledExpr.get());

            ASSERT_EQ(scalarTag, resBlockExtractedValues.tags()[i]);
            ASSERT_EQ(scalarVal, resBlockExtractedValues.vals()[i]);
        }
    } else {
        for (size_t i = 0; i < resNum; ++i) {
            leftScalarAccessor.reset(leftExtractedValues.tags()[i], leftExtractedValues.vals()[i]);
            rightScalarAccessor.reset(rightExtractedValues.tags()[i],
                                      rightExtractedValues.vals()[i]);
            auto [scalarTag, scalarVal] = runCompiledExpression(scalarCompiledExpr.get());

            ASSERT_EQ(scalarTag, resBlockExtractedValues.tags()[i]);
            ASSERT_EQ(scalarVal, resBlockExtractedValues.vals()[i]);
        }
    }
}

void SBEBlockExpressionTest::testBlockScalarArithmeticOp(
    EPrimBinary::Op scalarOp,
    StringData blockFunctionName,
    value::ValueBlock* bitsetBlock,
    value::ValueBlock* block,
    std::pair<value::TypeTags, value::Value> scalar) {
    value::ViewOfValueAccessor bitsetBlockAccessor;
    value::ViewOfValueAccessor blockAccessor;
    value::ViewOfValueAccessor scalarAccessor;

    auto bitsetSlot = bindAccessor(&bitsetBlockAccessor);
    auto blockSlot = bindAccessor(&blockAccessor);
    auto scalarSlot = bindAccessor(&scalarAccessor);

    auto scalarBlockMathExpr = sbe::makeE<sbe::EFunction>(blockFunctionName,
                                                          sbe::makeEs(makeE<EVariable>(bitsetSlot),
                                                                      makeE<EVariable>(scalarSlot),
                                                                      makeE<EVariable>(blockSlot)));

    auto blockScalarMathExpr =
        sbe::makeE<sbe::EFunction>(blockFunctionName,
                                   sbe::makeEs(makeE<EVariable>(bitsetSlot),
                                               makeE<EVariable>(blockSlot),
                                               makeE<EVariable>(scalarSlot)));

    auto scalarBlockCompiledExpr = compileExpression(*scalarBlockMathExpr);
    auto blockScalarCompiledExpr = compileExpression(*blockScalarMathExpr);

    if (bitsetBlock) {
        bitsetBlockAccessor.reset(sbe::value::TypeTags::valueBlock,
                                  value::bitcastFrom<value::ValueBlock*>(bitsetBlock));
    } else {
        bitsetBlockAccessor.reset(sbe::value::TypeTags::Nothing, 0);
    }
    blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(block));
    scalarAccessor.reset(scalar.first, scalar.second);


    // run the block operations
    auto [resScalarBlockTag, resScalarBlockVal] =
        runCompiledExpression(scalarBlockCompiledExpr.get());
    value::ValueGuard scalarBlockGuard(resScalarBlockTag, resScalarBlockVal);
    auto* resScalarBlock = value::bitcastTo<value::ValueBlock*>(resScalarBlockVal);
    auto resScalarBlockExtractedValues = resScalarBlock->extract();

    auto [resBlockScalarTag, resBlockScalarVal] =
        runCompiledExpression(blockScalarCompiledExpr.get());
    value::ValueGuard blockScalarGuard(resBlockScalarTag, resBlockScalarVal);
    auto* resBlockScalar = value::bitcastTo<value::ValueBlock*>(resBlockScalarVal);
    auto resBlockScalarExtractedValues = resBlockScalar->extract();

    ASSERT_EQ(resScalarBlockTag, value::TypeTags::valueBlock);
    ASSERT_EQ(resBlockScalarTag, value::TypeTags::valueBlock);

    if (block->as<value::MonoBlock>()) {
        ASSERT_TRUE(resScalarBlock->as<value::MonoBlock>());
        ASSERT_TRUE(resBlockScalar->as<value::MonoBlock>());
    }

    // verify the results against the scalar operation
    auto extractedValues = block->extract();
    auto resNum = extractedValues.count();

    ASSERT_EQ(resScalarBlockExtractedValues.count(), resNum);
    ASSERT_EQ(resBlockScalarExtractedValues.count(), resNum);

    value::ViewOfValueAccessor leftScalarAccessor;
    value::ViewOfValueAccessor rightScalarAccessor;

    auto leftScalarSlot = bindAccessor(&leftScalarAccessor);
    auto rightScalarSlot = bindAccessor(&rightScalarAccessor);

    auto scalarMathExpr = sbe::makeE<sbe::EPrimBinary>(
        scalarOp, makeE<EVariable>(leftScalarSlot), makeE<EVariable>(rightScalarSlot));

    auto scalarCompiledExpr = compileExpression(*scalarMathExpr);

    if (bitsetBlock) {
        auto bitsetExtractedValues = bitsetBlock->extract();
        for (size_t i = 0; i < resNum; ++i) {
            if (bitsetExtractedValues.tags()[i] != value::TypeTags::Boolean ||
                !value::bitcastTo<bool>(bitsetExtractedValues.vals()[i])) {
                // skip
                continue;
            }

            // scalar - block
            leftScalarAccessor.reset(scalar.first, scalar.second);
            rightScalarAccessor.reset(extractedValues.tags()[i], extractedValues.vals()[i]);
            auto [scalarSBTag, scalarSBVal] = runCompiledExpression(scalarCompiledExpr.get());

            ASSERT_EQ(scalarSBTag, resScalarBlockExtractedValues.tags()[i]);
            ASSERT_EQ(scalarSBVal, resScalarBlockExtractedValues.vals()[i]);

            // block - scalar
            leftScalarAccessor.reset(extractedValues.tags()[i], extractedValues.vals()[i]);
            rightScalarAccessor.reset(scalar.first, scalar.second);
            auto [scalarBSTag, scalarBSVal] = runCompiledExpression(scalarCompiledExpr.get());

            ASSERT_EQ(scalarBSTag, resBlockScalarExtractedValues.tags()[i]);
            ASSERT_EQ(scalarBSVal, resBlockScalarExtractedValues.vals()[i]);
        }
    } else {
        for (size_t i = 0; i < resNum; ++i) {
            // scalar - block
            leftScalarAccessor.reset(scalar.first, scalar.second);
            rightScalarAccessor.reset(extractedValues.tags()[i], extractedValues.vals()[i]);
            auto [scalarSBTag, scalarSBVal] = runCompiledExpression(scalarCompiledExpr.get());

            ASSERT_EQ(scalarSBTag, resScalarBlockExtractedValues.tags()[i]);
            ASSERT_EQ(scalarSBVal, resScalarBlockExtractedValues.vals()[i]);

            // block - scalar
            leftScalarAccessor.reset(extractedValues.tags()[i], extractedValues.vals()[i]);
            rightScalarAccessor.reset(scalar.first, scalar.second);
            auto [scalarBSTag, scalarBSVal] = runCompiledExpression(scalarCompiledExpr.get());

            ASSERT_EQ(scalarBSTag, resBlockScalarExtractedValues.tags()[i]);
            ASSERT_EQ(scalarBSVal, resBlockScalarExtractedValues.vals()[i]);
        }
    }
}

TEST_F(SBEBlockExpressionTest, ValueBlockAddHeterogeneousTest) {
    StringData fnName{"valueBlockAdd"};
    value::HeterogeneousBlock leftBlock;
    value::HeterogeneousBlock rightBlock;

    // 1 : Integer + Integer -> Integer
    leftBlock.push_back(makeInt32(42));
    rightBlock.push_back(makeInt32(8));
    // 2 : Double + integer -> Double
    leftBlock.push_back(makeDouble(42.5));
    rightBlock.push_back(makeInt32(123));
    // 3 : Overflow -> Promote to int64_t
    leftBlock.push_back(makeInt32(43));
    rightBlock.push_back(makeInt32(std::numeric_limits<int32_t>::max()));
    // 4 : Nothing + Number -> Nothing
    leftBlock.push_back(makeNothing());
    rightBlock.push_back(makeInt64(std::numeric_limits<int64_t>::max()));
    // 5 : String + Number -> Nothing
    leftBlock.push_back(value::makeNewString("45"_sd));
    rightBlock.push_back(makeDouble(12.5));
    // 6 : Overflow -> Double
    leftBlock.push_back(makeInt64(std::numeric_limits<int64_t>::max()));
    rightBlock.push_back(makeInt64(10));
    // 7 : Date + Number -> Date
    leftBlock.push_back(
        {value::TypeTags::Date,
         value::bitcastFrom<int64_t>(TimeZoneDatabase::utcZone()
                                         .createFromDateParts(2023, 10, 20, 12, 30, 0, 0)
                                         .toMillisSinceEpoch())});
    rightBlock.push_back(makeInt32(std::numeric_limits<int32_t>::max()));

    {
        auto bitsetBlock = makeBoolBlock({true, true, true, true, true, true, true});

        testBlockBlockArithmeticOp(
            EPrimBinary::add, fnName, bitsetBlock.get(), &leftBlock, &rightBlock);
    }

    {
        auto bitsetBlock = makeBoolBlock({true, true, false, true, false, true, true});

        testBlockBlockArithmeticOp(
            EPrimBinary::add, fnName, bitsetBlock.get(), &leftBlock, &rightBlock);
    }

    { testBlockBlockArithmeticOp(EPrimBinary::add, fnName, nullptr, &leftBlock, &rightBlock); }
}

TEST_F(SBEBlockExpressionTest, ValueBlockAddMonoBlockTest) {
    StringData fnName{"valueBlockAdd"};

    value::Int32Block block;
    block.push_back(1);
    block.push_back(2);
    block.push_back(3);
    block.push_back(4);
    block.push_back(5);
    block.push_back(6);
    block.push_back(7);

    value::MonoBlock monoBlock1(7, value::TypeTags::NumberInt32, value::bitcastFrom<int>(100));
    value::MonoBlock monoBlock2(
        7, value::TypeTags::NumberDouble, value::bitcastFrom<double>(98.67));

    {
        auto bitsetBlock = makeBoolBlock({true, true, true, true, true, true, true});

        testBlockBlockArithmeticOp(
            EPrimBinary::add, fnName, bitsetBlock.get(), &block, &monoBlock2);
        testBlockBlockArithmeticOp(
            EPrimBinary::add, fnName, bitsetBlock.get(), &monoBlock1, &block);
        testBlockBlockArithmeticOp(
            EPrimBinary::add, fnName, bitsetBlock.get(), &monoBlock1, &monoBlock2);
    }

    {
        auto bitsetBlock = makeBoolBlock({true, true, false, true, false, true, true});

        testBlockBlockArithmeticOp(
            EPrimBinary::add, fnName, bitsetBlock.get(), &block, &monoBlock2);
        testBlockBlockArithmeticOp(
            EPrimBinary::add, fnName, bitsetBlock.get(), &monoBlock1, &block);
        testBlockBlockArithmeticOp(
            EPrimBinary::add, fnName, bitsetBlock.get(), &monoBlock1, &monoBlock2);
    }

    {
        testBlockBlockArithmeticOp(EPrimBinary::add, fnName, nullptr, &block, &monoBlock2);
        testBlockBlockArithmeticOp(EPrimBinary::add, fnName, nullptr, &monoBlock1, &block);
        testBlockBlockArithmeticOp(EPrimBinary::add, fnName, nullptr, &monoBlock1, &monoBlock2);
    }
}

TEST_F(SBEBlockExpressionTest, ValueBlockAddScalarTest) {
    StringData fnName{"valueBlockAdd"};

    value::Int32Block block;
    block.push_back(1);
    block.push_back(2);
    block.push_back(3);
    block.push_back(4);
    block.push_back(5);
    block.push_back(6);
    block.push_back(7);

    {
        auto bitsetBlock = makeBoolBlock({true, true, true, true, true, true, true});

        testBlockScalarArithmeticOp(
            EPrimBinary::add, fnName, bitsetBlock.get(), &block, makeInt32(100));
    }

    {
        auto bitsetBlock = makeBoolBlock({true, true, false, true, false, true, true});

        testBlockScalarArithmeticOp(
            EPrimBinary::add, fnName, bitsetBlock.get(), &block, makeInt32(100));
    }

    { testBlockScalarArithmeticOp(EPrimBinary::add, fnName, nullptr, &block, makeInt32(100)); }
}

TEST_F(SBEBlockExpressionTest, ValueBlockSubHeterogeneousTest) {
    StringData fnName{"valueBlockSub"};

    value::HeterogeneousBlock leftBlock;
    value::HeterogeneousBlock rightBlock;

    // 1 : Integer - Integer -> Integer (>0)
    leftBlock.push_back(makeInt32(42));
    rightBlock.push_back(makeInt32(8));
    // 2 : Double - integer -> Double (<0)
    leftBlock.push_back(makeDouble(42.5));
    rightBlock.push_back(makeInt32(123));
    // 3 : Underflow -> promote to int64
    leftBlock.push_back(makeInt32(std::numeric_limits<int32_t>::min()));
    rightBlock.push_back(makeInt32(std::numeric_limits<int32_t>::max()));
    // 4 : Nothing - Number -> Nothing
    leftBlock.push_back(makeNothing());
    rightBlock.push_back(makeInt64(std::numeric_limits<int64_t>::max()));
    // 5 : Number - Nothing -> Nothing
    leftBlock.push_back(makeInt64(std::numeric_limits<int64_t>::max()));
    rightBlock.push_back(makeNothing());
    // 6 : String - Number -> Nothing
    leftBlock.push_back(value::makeNewString("45"_sd));
    rightBlock.push_back(makeDouble(12.5));
    // 7 : Number - String -> Nothing
    leftBlock.push_back(makeDouble(12.5));
    rightBlock.push_back(value::makeNewString("45"_sd));
    // 8 : Underflow -> promote to Double
    leftBlock.push_back(makeInt64(std::numeric_limits<int64_t>::min()));
    rightBlock.push_back(makeInt64(std::numeric_limits<int64_t>::max()));
    // 9 : Date - Number -> Date
    leftBlock.push_back(
        {value::TypeTags::Date,
         value::bitcastFrom<int64_t>(TimeZoneDatabase::utcZone()
                                         .createFromDateParts(2023, 10, 20, 12, 30, 0, 0)
                                         .toMillisSinceEpoch())});
    rightBlock.push_back(makeInt32(std::numeric_limits<int32_t>::min()));

    {
        auto bitsetBlock = makeBoolBlock({true, true, true, true, true, true, true, true, true});

        testBlockBlockArithmeticOp(
            EPrimBinary::sub, fnName, bitsetBlock.get(), &leftBlock, &rightBlock);
    }

    {
        auto bitsetBlock = makeBoolBlock({true, true, false, true, false, true, true, true, false});

        testBlockBlockArithmeticOp(
            EPrimBinary::sub, fnName, bitsetBlock.get(), &leftBlock, &rightBlock);
    }
    { testBlockBlockArithmeticOp(EPrimBinary::sub, fnName, nullptr, &leftBlock, &rightBlock); }
}

TEST_F(SBEBlockExpressionTest, ValueBlockSubMonoBlockTest) {
    StringData fnName{"valueBlockSub"};

    value::Int32Block block;
    block.push_back(1);
    block.push_back(2);
    block.push_back(3);
    block.push_back(4);
    block.push_back(5);
    block.push_back(6);
    block.push_back(7);

    value::MonoBlock monoBlock1(7, value::TypeTags::NumberInt32, value::bitcastFrom<int>(100));
    value::MonoBlock monoBlock2(
        7, value::TypeTags::NumberDouble, value::bitcastFrom<double>(98.67));

    {
        auto bitsetBlock = makeBoolBlock({true, true, true, true, true, true, true});

        testBlockBlockArithmeticOp(
            EPrimBinary::sub, fnName, bitsetBlock.get(), &block, &monoBlock2);
        testBlockBlockArithmeticOp(
            EPrimBinary::sub, fnName, bitsetBlock.get(), &monoBlock1, &block);
        testBlockBlockArithmeticOp(
            EPrimBinary::sub, fnName, bitsetBlock.get(), &monoBlock1, &monoBlock2);
    }

    {
        auto bitsetBlock = makeBoolBlock({true, true, false, true, false, true, true});

        testBlockBlockArithmeticOp(
            EPrimBinary::sub, fnName, bitsetBlock.get(), &block, &monoBlock2);
        testBlockBlockArithmeticOp(
            EPrimBinary::sub, fnName, bitsetBlock.get(), &monoBlock1, &block);
        testBlockBlockArithmeticOp(
            EPrimBinary::sub, fnName, bitsetBlock.get(), &monoBlock1, &monoBlock2);
    }

    {
        testBlockBlockArithmeticOp(EPrimBinary::sub, fnName, nullptr, &block, &monoBlock2);
        testBlockBlockArithmeticOp(EPrimBinary::sub, fnName, nullptr, &monoBlock1, &block);
        testBlockBlockArithmeticOp(EPrimBinary::sub, fnName, nullptr, &monoBlock1, &monoBlock2);
    }
}

TEST_F(SBEBlockExpressionTest, ValueBlockSubScalarTest) {
    StringData fnName{"valueBlockSub"};

    value::Int32Block block;
    block.push_back(1);
    block.push_back(2);
    block.push_back(3);
    block.push_back(4);
    block.push_back(5);
    block.push_back(6);
    block.push_back(7);

    {
        auto bitsetBlock = makeBoolBlock({true, true, true, true, true, true, true});

        testBlockScalarArithmeticOp(
            EPrimBinary::sub, fnName, bitsetBlock.get(), &block, makeInt32(100));
    }

    {
        auto bitsetBlock = makeBoolBlock({true, true, false, true, false, true, true});

        testBlockScalarArithmeticOp(
            EPrimBinary::sub, fnName, bitsetBlock.get(), &block, makeInt32(100));
    }

    { testBlockScalarArithmeticOp(EPrimBinary::sub, fnName, nullptr, &block, makeInt32(100)); }
}

TEST_F(SBEBlockExpressionTest, ValueBlockMultHeterogeneousTest) {
    StringData fnName{"valueBlockMult"};

    value::HeterogeneousBlock leftBlock;
    value::HeterogeneousBlock rightBlock;

    // 1 : Integer * Integer -> Integer
    leftBlock.push_back(makeInt32(42));
    rightBlock.push_back(makeInt32(8));
    // 2 : Double * integer -> Double
    leftBlock.push_back(makeDouble(42.5));
    rightBlock.push_back(makeInt32(123));
    // 3 : Overflow -> Promote to int64_t
    leftBlock.push_back(makeInt32(43));
    rightBlock.push_back(makeInt32(std::numeric_limits<int32_t>::max()));
    // 4 : Nothing * Number -> Nothing
    leftBlock.push_back(makeNothing());
    rightBlock.push_back(makeInt64(std::numeric_limits<int64_t>::max()));
    // 5 : String * Number -> Nothing
    leftBlock.push_back(value::makeNewString("45"_sd));
    rightBlock.push_back(makeDouble(12.5));
    // 6 : Overflow -> Double
    leftBlock.push_back(makeInt64(std::numeric_limits<int64_t>::max()));
    rightBlock.push_back(makeInt64(10));
    // 7 : Date * Number -> Date
    leftBlock.push_back(
        {value::TypeTags::Date,
         value::bitcastFrom<int64_t>(TimeZoneDatabase::utcZone()
                                         .createFromDateParts(2023, 10, 20, 12, 30, 0, 0)
                                         .toMillisSinceEpoch())});
    rightBlock.push_back(makeInt32(5));

    {
        auto bitsetBlock = makeBoolBlock({true, true, true, true, true, true, true});

        testBlockBlockArithmeticOp(
            EPrimBinary::mul, fnName, bitsetBlock.get(), &leftBlock, &rightBlock);
    }

    {
        auto bitsetBlock = makeBoolBlock({true, true, false, true, false, true, true});

        testBlockBlockArithmeticOp(
            EPrimBinary::mul, fnName, bitsetBlock.get(), &leftBlock, &rightBlock);
    }

    { testBlockBlockArithmeticOp(EPrimBinary::mul, fnName, nullptr, &leftBlock, &rightBlock); }
}

TEST_F(SBEBlockExpressionTest, ValueBlockMultMonoBlockTest) {
    StringData fnName{"valueBlockMult"};

    value::Int32Block block;
    block.push_back(1);
    block.push_back(2);
    block.push_back(3);
    block.push_back(4);
    block.push_back(5);
    block.push_back(6);
    block.push_back(7);

    value::MonoBlock monoBlock1(7, value::TypeTags::NumberInt32, value::bitcastFrom<int>(100));
    value::MonoBlock monoBlock2(
        7, value::TypeTags::NumberDouble, value::bitcastFrom<double>(98.67));

    {
        auto bitsetBlock = makeBoolBlock({true, true, true, true, true, true, true});

        testBlockBlockArithmeticOp(
            EPrimBinary::mul, fnName, bitsetBlock.get(), &block, &monoBlock2);
        testBlockBlockArithmeticOp(
            EPrimBinary::mul, fnName, bitsetBlock.get(), &monoBlock1, &block);
        testBlockBlockArithmeticOp(
            EPrimBinary::mul, fnName, bitsetBlock.get(), &monoBlock1, &monoBlock2);
    }

    {
        auto bitsetBlock = makeBoolBlock({true, true, false, true, false, true, true});

        testBlockBlockArithmeticOp(
            EPrimBinary::mul, fnName, bitsetBlock.get(), &block, &monoBlock2);
        testBlockBlockArithmeticOp(
            EPrimBinary::mul, fnName, bitsetBlock.get(), &monoBlock1, &block);
        testBlockBlockArithmeticOp(
            EPrimBinary::mul, fnName, bitsetBlock.get(), &monoBlock1, &monoBlock2);
    }

    {
        testBlockBlockArithmeticOp(EPrimBinary::mul, fnName, nullptr, &block, &monoBlock2);
        testBlockBlockArithmeticOp(EPrimBinary::mul, fnName, nullptr, &monoBlock1, &block);
        testBlockBlockArithmeticOp(EPrimBinary::mul, fnName, nullptr, &monoBlock1, &monoBlock2);
    }
}

TEST_F(SBEBlockExpressionTest, ValueBlockMultScalarTest) {
    StringData fnName{"valueBlockMult"};

    value::Int32Block block;
    block.push_back(1);
    block.push_back(2);
    block.push_back(3);
    block.push_back(4);
    block.push_back(5);
    block.push_back(6);
    block.push_back(7);

    {
        auto bitsetBlock = makeBoolBlock({true, true, true, true, true, true, true});

        testBlockScalarArithmeticOp(
            EPrimBinary::mul, fnName, bitsetBlock.get(), &block, makeInt32(100));
    }

    {
        auto bitsetBlock = makeBoolBlock({true, true, false, true, false, true, true});

        testBlockScalarArithmeticOp(
            EPrimBinary::mul, fnName, bitsetBlock.get(), &block, makeInt32(100));
    }

    { testBlockScalarArithmeticOp(EPrimBinary::mul, fnName, nullptr, &block, makeInt32(100)); }
}

TEST_F(SBEBlockExpressionTest, ValueBlockDivHeterogeneousTest) {
    StringData fnName{"valueBlockDiv"};

    value::HeterogeneousBlock leftBlock;
    value::HeterogeneousBlock rightBlock;

    // 1 : Integer / Integer -> Double
    leftBlock.push_back(makeInt32(32));
    rightBlock.push_back(makeInt32(8));
    // 2 : Double / Integer -> Double
    leftBlock.push_back(makeDouble(42.5));
    rightBlock.push_back(makeInt32(123));
    // 3 : Underflow -> promote to Double -1
    leftBlock.push_back(makeInt32(std::numeric_limits<int32_t>::min()));
    rightBlock.push_back(makeInt32(std::numeric_limits<int32_t>::max()));
    // 4 : Nothing / Number -> Nothing
    leftBlock.push_back(makeNothing());
    rightBlock.push_back(makeInt64(std::numeric_limits<int64_t>::max()));
    // 5 : Number / Nothing -> Nothing
    leftBlock.push_back(makeInt64(std::numeric_limits<int64_t>::max()));
    rightBlock.push_back(makeNothing());
    // 6 : String / Number -> Nothing
    leftBlock.push_back(value::makeNewString("45"_sd));
    rightBlock.push_back(makeDouble(12.5));
    // 7 : Number / String -> Nothing
    leftBlock.push_back(makeDouble(12.5));
    rightBlock.push_back(value::makeNewString("45"_sd));
    // 8 : Underflow -> promote to Double -1
    leftBlock.push_back(makeInt64(std::numeric_limits<int64_t>::min()));
    rightBlock.push_back(makeInt64(std::numeric_limits<int64_t>::max()));
    // 9 : Date / Number -> Nothing
    leftBlock.push_back(
        {value::TypeTags::Date,
         value::bitcastFrom<int64_t>(TimeZoneDatabase::utcZone()
                                         .createFromDateParts(2023, 10, 20, 12, 30, 0, 0)
                                         .toMillisSinceEpoch())});
    rightBlock.push_back(makeInt32(2));

    {
        auto bitsetBlock = makeBoolBlock({true, true, true, true, true, true, true, true, true});

        testBlockBlockArithmeticOp(
            EPrimBinary::div, fnName, bitsetBlock.get(), &leftBlock, &rightBlock);
    }

    {
        auto bitsetBlock = makeBoolBlock({true, true, false, true, false, true, true, true, false});

        testBlockBlockArithmeticOp(
            EPrimBinary::div, fnName, bitsetBlock.get(), &leftBlock, &rightBlock);
    }

    { testBlockBlockArithmeticOp(EPrimBinary::div, fnName, nullptr, &leftBlock, &rightBlock); }
}

TEST_F(SBEBlockExpressionTest, ValueBlockDivMonoBlockTest) {
    StringData fnName{"valueBlockDiv"};

    value::Int32Block block;
    block.push_back(100);
    block.push_back(200);
    block.push_back(300);
    block.push_back(400);
    block.push_back(500);
    block.push_back(600);
    block.push_back(700);
    block.push_back(0);

    value::MonoBlock monoBlock1(8, value::TypeTags::NumberInt32, value::bitcastFrom<int>(10));
    value::MonoBlock monoBlock2(8, value::TypeTags::NumberDouble, value::bitcastFrom<double>(9.67));

    {
        auto bitsetBlock = makeBoolBlock({true, true, true, true, true, true, true, true});

        testBlockBlockArithmeticOp(
            EPrimBinary::div, fnName, bitsetBlock.get(), &block, &monoBlock2);
        ASSERT_THROWS_CODE(testBlockBlockArithmeticOp(
                               EPrimBinary::div, fnName, bitsetBlock.get(), &monoBlock1, &block),
                           DBException,
                           4848401);  // division by zero
        testBlockBlockArithmeticOp(
            EPrimBinary::div, fnName, bitsetBlock.get(), &monoBlock1, &monoBlock2);
    }

    {
        auto bitsetBlock = makeBoolBlock({true, true, false, true, false, true, true, false});

        testBlockBlockArithmeticOp(
            EPrimBinary::div, fnName, bitsetBlock.get(), &block, &monoBlock2);
        testBlockBlockArithmeticOp(
            EPrimBinary::div, fnName, bitsetBlock.get(), &monoBlock1, &block);
        testBlockBlockArithmeticOp(
            EPrimBinary::div, fnName, bitsetBlock.get(), &monoBlock1, &monoBlock2);
    }

    {
        testBlockBlockArithmeticOp(EPrimBinary::div, fnName, nullptr, &block, &monoBlock2);
        ASSERT_THROWS_CODE(
            testBlockBlockArithmeticOp(EPrimBinary::div, fnName, nullptr, &monoBlock1, &block),
            DBException,
            4848401);  // division by zero
        testBlockBlockArithmeticOp(EPrimBinary::div, fnName, nullptr, &monoBlock1, &monoBlock2);
    }

    {
        value::HeterogeneousBlock bitsetBlock;
        bitsetBlock.push_back(makeBool(true));
        bitsetBlock.push_back(makeBool(true));
        bitsetBlock.push_back(makeInt32(100));
        bitsetBlock.push_back(makeNothing());
        bitsetBlock.push_back(makeBool(true));
        bitsetBlock.push_back(makeBool(false));
        bitsetBlock.push_back(makeBool(true));
        bitsetBlock.push_back(makeDouble(2.5));

        testBlockBlockArithmeticOp(EPrimBinary::div, fnName, &bitsetBlock, &block, &monoBlock2);
        testBlockBlockArithmeticOp(EPrimBinary::div, fnName, &bitsetBlock, &monoBlock1, &block);
        testBlockBlockArithmeticOp(
            EPrimBinary::div, fnName, &bitsetBlock, &monoBlock1, &monoBlock2);
    }
}

TEST_F(SBEBlockExpressionTest, ValueBlockDivScalarTest) {
    StringData fnName{"valueBlockDiv"};

    value::Int32Block block;
    block.push_back(100);
    block.push_back(200);
    block.push_back(300);
    block.push_back(400);
    block.push_back(500);
    block.push_back(600);
    block.push_back(700);
    block.push_back(0);

    {
        auto bitsetBlock = makeBoolBlock({true, true, true, true, true, true, true, true});

        ASSERT_THROWS_CODE(testBlockScalarArithmeticOp(
                               EPrimBinary::div, fnName, bitsetBlock.get(), &block, makeInt32(100)),
                           DBException,
                           4848401);  // division by zero
    }

    {
        auto bitsetBlock = makeBoolBlock({true, true, false, true, false, true, true, false});

        testBlockScalarArithmeticOp(
            EPrimBinary::div, fnName, bitsetBlock.get(), &block, makeInt32(100));
    }

    {
        ASSERT_THROWS_CODE(
            testBlockScalarArithmeticOp(EPrimBinary::div, fnName, nullptr, &block, makeInt32(100)),
            DBException,
            4848401);  // division by zero
    }

    {
        value::HeterogeneousBlock bitsetBlock;
        bitsetBlock.push_back(makeBool(true));
        bitsetBlock.push_back(makeBool(true));
        bitsetBlock.push_back(makeInt32(100));
        bitsetBlock.push_back(makeNothing());
        bitsetBlock.push_back(makeBool(true));
        bitsetBlock.push_back(makeBool(false));
        bitsetBlock.push_back(makeBool(true));
        bitsetBlock.push_back(makeDouble(2.5));

        testBlockScalarArithmeticOp(EPrimBinary::div, fnName, &bitsetBlock, &block, makeInt32(100));
    }
}


TEST_F(SBEBlockExpressionTest, BlockNewTest) {
    auto expr = makeE<sbe::EFunction>("valueBlockNewFill",
                                      sbe::makeEs(makeC(makeBool(false)), makeC(makeInt32(7))));
    auto compiledExpr = compileExpression(*expr);

    auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(runTag, runVal);

    assertBlockOfBool(runTag, runVal, {false, false, false, false, false, false, false});
}

TEST_F(SBEBlockExpressionTest, BlockSizeTest) {
    value::ViewOfValueAccessor blockAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);

    auto block = makeHeterogeneousBoolBlock({true, false, true, false});
    blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(block.get()));

    auto expr = makeE<sbe::EFunction>("valueBlockSize", sbe::makeEs(makeE<EVariable>(blockSlot)));
    auto compiledExpr = compileExpression(*expr);

    auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(runTag, runVal);

    ASSERT_EQ(runTag, value::TypeTags::NumberInt32);
    ASSERT_EQ(value::bitcastTo<int32_t>(runVal), 4);
}

TEST_F(SBEBlockExpressionTest, BitmapNoneTest) {
    value::ViewOfValueAccessor blockAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);

    auto block1 = makeHeterogeneousBoolBlock({true, false, true, false});
    blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(block1.get()));

    auto expr = makeE<sbe::EFunction>(
        "valueBlockNone", sbe::makeEs(makeE<EVariable>(blockSlot), makeC(makeBool(true))));
    auto compiledExpr = compileExpression(*expr);

    auto [runTag1, runVal1] = runCompiledExpression(compiledExpr.get());

    ASSERT_EQ(runTag1, value::TypeTags::Boolean);
    ASSERT_EQ(value::bitcastTo<bool>(runVal1), false);

    auto block2 = makeHeterogeneousBoolBlock({false, false, false, false});
    blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(block2.get()));

    auto [runTag2, runVal2] = runCompiledExpression(compiledExpr.get());

    ASSERT_EQ(runTag2, value::TypeTags::Boolean);
    ASSERT_EQ(value::bitcastTo<bool>(runVal2), true);
}

TEST_F(SBEBlockExpressionTest, BlockLogicNotTest) {
    value::ViewOfValueAccessor blockAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);

    auto block = makeHeterogeneousBoolBlock({true, false, true, false});
    blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(block.get()));

    auto expr =
        makeE<sbe::EFunction>("valueBlockLogicalNot", sbe::makeEs(makeE<EVariable>(blockSlot)));
    auto compiledExpr = compileExpression(*expr);

    auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(runTag, runVal);

    assertBlockOfBool(runTag, runVal, {false, true, false, true});
}

TEST_F(SBEBlockExpressionTest, BlockCombineTest) {
    value::ViewOfValueAccessor blockAccessorLeft;
    value::ViewOfValueAccessor blockAccessorRight;
    value::ViewOfValueAccessor blockAccessorMask;
    auto blockLeftSlot = bindAccessor(&blockAccessorLeft);
    auto blockRightSlot = bindAccessor(&blockAccessorRight);
    auto blockMaskSlot = bindAccessor(&blockAccessorMask);

    value::HeterogeneousBlock leftBlock;
    leftBlock.push_back(makeInt32(1));
    leftBlock.push_back(makeInt32(2));
    leftBlock.push_back(makeInt32(3));
    leftBlock.push_back(makeNothing());
    leftBlock.push_back(makeInt32(5));
    blockAccessorLeft.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&leftBlock));

    value::HeterogeneousBlock rightBlock;
    rightBlock.push_back(value::makeNewString("This is item #1"_sd));
    rightBlock.push_back(makeNothing());
    rightBlock.push_back(value::makeNewString("This is item #3"_sd));
    rightBlock.push_back(value::makeNewString("This is item #4"_sd));
    rightBlock.push_back(value::makeNewString("This is item #5"_sd));
    blockAccessorRight.reset(sbe::value::TypeTags::valueBlock,
                             value::bitcastFrom<value::ValueBlock*>(&rightBlock));

    {
        auto block = makeHeterogeneousBoolBlock({true, false, true, false, true});
        blockAccessorMask.reset(sbe::value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(block.get()));

        auto expr = makeE<sbe::EFunction>("valueBlockCombine",
                                          sbe::makeEs(makeE<EVariable>(blockLeftSlot),
                                                      makeE<EVariable>(blockRightSlot),
                                                      makeE<EVariable>(blockMaskSlot)));
        auto compiledExpr = compileExpression(*expr);

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guardRun(runTag, runVal);
        auto [strTag, strVal] = value::makeNewString("This is item #4"_sd);
        value::ValueGuard guardStr(strTag, strVal);

        assertBlockEq(
            runTag,
            runVal,
            std::vector<std::pair<value::TypeTags, value::Value>>{makeInt32(1),
                                                                  makeNothing(),
                                                                  makeInt32(3),
                                                                  std::make_pair(strTag, strVal),
                                                                  makeInt32(5)});
    }

    {
        value::HeterogeneousBlock block;
        block.push_back(makeBool(true));
        block.push_back(makeBool(false));
        block.push_back(makeBool(true));
        block.push_back(makeBool(false));
        block.push_back(makeNothing());
        blockAccessorMask.reset(sbe::value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(&block));

        auto expr = makeE<sbe::EFunction>("valueBlockCombine",
                                          sbe::makeEs(makeE<EVariable>(blockLeftSlot),
                                                      makeE<EVariable>(blockRightSlot),
                                                      makeE<EVariable>(blockMaskSlot)));
        auto compiledExpr = compileExpression(*expr);

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guardRun(runTag, runVal);
        auto [strTag, strVal] = value::makeNewString("This is item #4"_sd);
        value::ValueGuard guardStr(strTag, strVal);

        assertBlockEq(
            runTag,
            runVal,
            std::vector<std::pair<value::TypeTags, value::Value>>{makeInt32(1),
                                                                  makeNothing(),
                                                                  makeInt32(3),
                                                                  std::make_pair(strTag, strVal),
                                                                  makeNothing()});
    }
}

TEST_F(SBEBlockExpressionTest, BlockIsMemberArrayTestNumeric) {
    value::ViewOfValueAccessor blockAccessor;

    auto blockSlot = bindAccessor(&blockAccessor);

    value::HeterogeneousBlock block;
    block.push_back(makeInt32(1));
    block.push_back(makeInt32(2));
    block.push_back(makeInt32(3));
    block.push_back(makeNothing());
    block.push_back(makeInt32(5));
    blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(&block));

    auto [arrayTag, arrayVal] = value::makeNewArray();
    auto array = value::getArrayView(arrayVal);
    array->push_back(makeInt32(1));
    array->push_back(makeInt32(5));
    array->push_back(makeInt32(10));

    auto expr = makeE<sbe::EFunction>(
        "valueBlockIsMember",
        sbe::makeEs(makeE<EVariable>(blockSlot), makeE<EConstant>(arrayTag, arrayVal)));

    auto compiledExpr = compileExpression(*expr);

    auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(runTag, runVal);

    assertBlockOfBool(runTag, runVal, {true, false, false, false, true});
}

TEST_F(SBEBlockExpressionTest, BlockIsMemberArrayTestString) {
    value::ViewOfValueAccessor blockAccessor;

    auto blockSlot = bindAccessor(&blockAccessor);

    value::HeterogeneousBlock block;
    block.push_back(value::makeBigString("teststring1"));
    block.push_back(value::makeBigString("teststring2"));
    block.push_back(value::makeBigString("teststring3"));
    block.push_back(makeNothing());
    block.push_back(value::makeBigString("teststring5"));
    blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(&block));

    auto [arrayTag, arrayVal] = value::makeNewArray();
    auto array = value::getArrayView(arrayVal);
    array->push_back(value::makeBigString("teststring1"));
    array->push_back(value::makeBigString("teststring5"));
    array->push_back(value::makeBigString("teststring10"));

    auto expr = makeE<sbe::EFunction>(
        "valueBlockIsMember",
        sbe::makeEs(makeE<EVariable>(blockSlot), makeE<EConstant>(arrayTag, arrayVal)));

    auto compiledExpr = compileExpression(*expr);

    auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(runTag, runVal);

    assertBlockOfBool(runTag, runVal, {true, false, false, false, true});
}

TEST_F(SBEBlockExpressionTest, BlockIsMemberOnNothingTest) {
    value::ViewOfValueAccessor blockAccessor;

    auto blockSlot = bindAccessor(&blockAccessor);

    value::HeterogeneousBlock block;
    block.push_back(makeInt32(1));
    block.push_back(makeInt32(2));
    block.push_back(makeInt32(3));
    block.push_back(makeNothing());
    block.push_back(makeInt32(5));
    blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(&block));

    auto expr = makeE<sbe::EFunction>(
        "valueBlockIsMember",
        sbe::makeEs(makeE<EVariable>(blockSlot), makeE<EConstant>(value::TypeTags::Nothing, 0)));

    auto compiledExpr = compileExpression(*expr);

    auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(runTag, runVal);

    assertBlockEq(runTag,
                  runVal,
                  std::vector<std::pair<value::TypeTags, value::Value>>{
                      makeNothing(), makeNothing(), makeNothing(), makeNothing(), makeNothing()});
}

TEST_F(SBEBlockExpressionTest, BlockCoerceToBool) {
    value::ViewOfValueAccessor blockAccessor;

    auto blockSlot = bindAccessor(&blockAccessor);

    value::HeterogeneousBlock block;
    block.push_back(value::makeNewString("teststring1"));
    block.push_back(value::makeNewString(""));
    block.push_back(makeInt32(-2));
    block.push_back(makeInt32(0));
    block.push_back(makeBool(false));
    block.push_back(makeBool(true));
    block.push_back(makeDouble(0.0));
    block.push_back(makeDouble(-0.0));
    block.push_back(makeDouble(10.0));
    block.push_back(makeNothing());
    block.push_back(makeNull());

    blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(&block));

    auto expr =
        makeE<sbe::EFunction>("valueBlockCoerceToBool", sbe::makeEs(makeE<EVariable>(blockSlot)));

    auto compiledExpr = compileExpression(*expr);

    auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(runTag, runVal);

    assertBlockEq(runTag,
                  runVal,
                  std::vector<std::pair<value::TypeTags, value::Value>>{
                      makeBool(true),   // "teststring1"
                      makeBool(true),   // ""
                      makeBool(true),   // -2
                      makeBool(false),  // 0
                      makeBool(false),  // false
                      makeBool(true),   // true
                      makeBool(false),  // 0.0
                      makeBool(false),  // -0.0
                      makeBool(true),   // 10.0
                      makeNothing(),    // Nothing
                      makeBool(false),  // Null
                  });
}

TEST_F(SBEBlockExpressionTest, BlockRound) {
    value::ViewOfValueAccessor blockAccessor;
    value::ViewOfValueAccessor scalarAccessor;

    auto blockSlot = bindAccessor(&blockAccessor);
    auto scalarSlot = bindAccessor(&scalarAccessor);

    value::HeterogeneousBlock block;
    block.push_back(value::makeNewString("abcd"));
    block.push_back(makeInt32(-2));
    block.push_back(makeInt32(2));
    block.push_back(makeBool(false));
    block.push_back(makeDouble(0.0));
    block.push_back(makeDouble(-1234.987));
    block.push_back(makeDouble(1987.956));
    block.push_back(makeDouble(-1234.1234));
    block.push_back(makeDouble(1987.1234));
    block.push_back(makeDecimal("-1234.987"));
    block.push_back(makeDecimal("1987.956"));
    block.push_back(makeDecimal("1234.1234"));
    block.push_back(makeDecimal("-1987.1234"));
    block.push_back(makeNothing());
    block.push_back(makeNull());

    {
        auto expr = sbe::makeE<sbe::EFunction>(
            "valueBlockRound",
            sbe::makeEs(makeE<EVariable>(blockSlot), makeE<EVariable>(scalarSlot)));

        auto compiledExpr = compileExpression(*expr);

        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        scalarAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int>(2));

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        auto expectedResults = std::vector<std::pair<value::TypeTags, value::Value>>{
            makeNothing(),  // string
            makeInt32(-2),
            makeInt32(2),
            makeNothing(),  // bool
            makeDouble(0.0),
            makeDouble(-1234.99),
            makeDouble(1987.96),
            makeDouble(-1234.12),
            makeDouble(1987.12),
            makeDecimal("-1234.99"),
            makeDecimal("1987.96"),
            makeDecimal("1234.12"),
            makeDecimal("-1987.12"),
            makeNothing(),  // Nothing
            makeNothing(),  // Null
        };

        assertBlockEq(runTag, runVal, expectedResults);

        for (size_t i = 0; i < expectedResults.size(); ++i) {
            releaseValue(expectedResults[i].first, expectedResults[i].second);
        }
    }

    {
        auto expr = sbe::makeE<sbe::EFunction>(
            "valueBlockRound",
            sbe::makeEs(makeE<EVariable>(blockSlot), makeE<EVariable>(scalarSlot)));

        auto compiledExpr = compileExpression(*expr);

        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        scalarAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int>(-2));

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        auto expectedResults = std::vector<std::pair<value::TypeTags, value::Value>>{
            makeNothing(),  // string
            makeInt32(0),
            makeInt32(0),
            makeNothing(),  // bool
            makeDouble(0.0),
            makeDouble(-1200),
            makeDouble(2000),
            makeDouble(-1200),
            makeDouble(2000),
            makeDecimal("-1200"),
            makeDecimal("2000"),
            makeDecimal("1200"),
            makeDecimal("-2000"),
            makeNothing(),  // Nothing
            makeNothing(),  // Null
        };

        assertBlockEq(runTag, runVal, expectedResults);

        for (size_t i = 0; i < expectedResults.size(); ++i) {
            releaseValue(expectedResults[i].first, expectedResults[i].second);
        }
    }

    {
        auto expr =
            sbe::makeE<sbe::EFunction>("valueBlockRound", sbe::makeEs(makeE<EVariable>(blockSlot)));

        auto compiledExpr = compileExpression(*expr);

        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        auto expectedResults = std::vector<std::pair<value::TypeTags, value::Value>>{
            makeNothing(),  // string
            makeInt32(-2),
            makeInt32(2),
            makeNothing(),  // bool
            makeDouble(0.0),
            makeDouble(-1235),
            makeDouble(1988),
            makeDouble(-1234),
            makeDouble(1987),
            makeDecimal("-1235"),
            makeDecimal("1988"),
            makeDecimal("1234"),
            makeDecimal("-1987"),
            makeNothing(),  // Nothing
            makeNothing(),  // Null
        };

        assertBlockEq(runTag, runVal, expectedResults);

        for (size_t i = 0; i < expectedResults.size(); ++i) {
            releaseValue(expectedResults[i].first, expectedResults[i].second);
        }
    }
}

TEST_F(SBEBlockExpressionTest, BlockTrunc) {
    value::ViewOfValueAccessor blockAccessor;
    value::ViewOfValueAccessor scalarAccessor;

    auto blockSlot = bindAccessor(&blockAccessor);
    auto scalarSlot = bindAccessor(&scalarAccessor);

    value::HeterogeneousBlock block;
    block.push_back(value::makeNewString("abcd"));
    block.push_back(makeInt32(-2));
    block.push_back(makeInt32(2));
    block.push_back(makeBool(false));
    block.push_back(makeDouble(0.0));
    block.push_back(makeDouble(-1234.987));
    block.push_back(makeDouble(1987.956));
    block.push_back(makeDouble(-1234.1234));
    block.push_back(makeDouble(1987.1234));
    block.push_back(makeDecimal("-1234.987"));
    block.push_back(makeDecimal("1987.956"));
    block.push_back(makeDecimal("1234.1234"));
    block.push_back(makeDecimal("-1987.1234"));
    block.push_back(makeNothing());
    block.push_back(makeNull());

    {
        auto expr = sbe::makeE<sbe::EFunction>(
            "valueBlockTrunc",
            sbe::makeEs(makeE<EVariable>(blockSlot), makeE<EVariable>(scalarSlot)));

        auto compiledExpr = compileExpression(*expr);

        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        scalarAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int>(2));

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        auto expectedResults = std::vector<std::pair<value::TypeTags, value::Value>>{
            makeNothing(),  // string
            makeInt32(-2),
            makeInt32(2),
            makeNothing(),  // bool
            makeDouble(0.0),
            makeDouble(-1234.98),
            makeDouble(1987.95),
            makeDouble(-1234.12),
            makeDouble(1987.12),
            makeDecimal("-1234.98"),
            makeDecimal("1987.95"),
            makeDecimal("1234.12"),
            makeDecimal("-1987.12"),
            makeNothing(),  // Nothing
            makeNothing(),  // Null
        };

        assertBlockEq(runTag, runVal, expectedResults);

        for (size_t i = 0; i < expectedResults.size(); ++i) {
            releaseValue(expectedResults[i].first, expectedResults[i].second);
        }
    }

    {
        auto expr = sbe::makeE<sbe::EFunction>(
            "valueBlockTrunc",
            sbe::makeEs(makeE<EVariable>(blockSlot), makeE<EVariable>(scalarSlot)));

        auto compiledExpr = compileExpression(*expr);

        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        scalarAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int>(-2));

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        auto expectedResults = std::vector<std::pair<value::TypeTags, value::Value>>{
            makeNothing(),  // string
            makeInt32(0),
            makeInt32(0),
            makeNothing(),  // bool
            makeDouble(0.0),
            makeDouble(-1200),
            makeDouble(1900),
            makeDouble(-1200),
            makeDouble(1900),
            makeDecimal("-1200"),
            makeDecimal("1900"),
            makeDecimal("1200"),
            makeDecimal("-1900"),
            makeNothing(),  // Nothing
            makeNothing(),  // Null
        };

        assertBlockEq(runTag, runVal, expectedResults);

        for (size_t i = 0; i < expectedResults.size(); ++i) {
            releaseValue(expectedResults[i].first, expectedResults[i].second);
        }
    }

    {
        auto expr =
            sbe::makeE<sbe::EFunction>("valueBlockTrunc", sbe::makeEs(makeE<EVariable>(blockSlot)));

        auto compiledExpr = compileExpression(*expr);

        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        auto expectedResults = std::vector<std::pair<value::TypeTags, value::Value>>{
            makeNothing(),  // string
            makeInt32(-2),
            makeInt32(2),
            makeNothing(),  // bool
            makeDouble(0.0),
            makeDouble(-1234),
            makeDouble(1987),
            makeDouble(-1234),
            makeDouble(1987),
            makeDecimal("-1234"),
            makeDecimal("1987"),
            makeDecimal("1234"),
            makeDecimal("-1987"),
            makeNothing(),  // Nothing
            makeNothing(),  // Null
        };

        assertBlockEq(runTag, runVal, expectedResults);

        for (size_t i = 0; i < expectedResults.size(); ++i) {
            releaseValue(expectedResults[i].first, expectedResults[i].second);
        }
    }
}

TEST_F(SBEBlockExpressionTest, BlockMod) {
    value::ViewOfValueAccessor blockAccessor;
    value::ViewOfValueAccessor scalarAccessor;

    auto blockSlot = bindAccessor(&blockAccessor);
    auto scalarSlot = bindAccessor(&scalarAccessor);

    value::HeterogeneousBlock block;
    block.push_back(value::makeNewString("abcd"));
    block.push_back(makeInt32(-2));
    block.push_back(makeInt32(2));
    block.push_back(makeBool(false));
    block.push_back(makeDouble(0.0));
    block.push_back(makeDouble(-1234.987));
    block.push_back(makeDouble(1987.956));
    block.push_back(makeDecimal("-1234.987"));
    block.push_back(makeDecimal("1987.956"));
    block.push_back(makeInt64(-54687));
    block.push_back(makeInt64(25166));
    block.push_back(makeNothing());
    block.push_back(makeNull());

    auto expr = sbe::makeE<sbe::EFunction>(
        "valueBlockMod", sbe::makeEs(makeE<EVariable>(blockSlot), makeE<EVariable>(scalarSlot)));

    auto compiledExpr = compileExpression(*expr);

    // mod is positive int32
    {
        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        scalarAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int>(5));

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        auto expectedResults = std::vector<std::pair<value::TypeTags, value::Value>>{
            makeNothing(),  // string
            makeInt32(-2),
            makeInt32(2),
            makeNothing(),  // bool
            makeDouble(0.0),
            makeDouble(fmod(
                numericCast<double>(value::TypeTags::NumberDouble,
                                    value::bitcastFrom<double>(-1234.987)),
                numericCast<double>(value::TypeTags::NumberInt32, value::bitcastFrom<int>(5)))),
            makeDouble(fmod(
                numericCast<double>(value::TypeTags::NumberDouble,
                                    value::bitcastFrom<double>(1987.956)),
                numericCast<double>(value::TypeTags::NumberInt32, value::bitcastFrom<int>(5)))),
            makeDecimal("-4.987"),
            makeDecimal("2.956"),
            makeInt64(-2),
            makeInt64(1),
            makeNothing(),  // Nothing
            makeNothing(),  // Null
        };

        assertBlockEq(runTag, runVal, expectedResults);

        for (size_t i = 0; i < expectedResults.size(); ++i) {
            releaseValue(expectedResults[i].first, expectedResults[i].second);
        }
    }

    // mod is negative int32
    {
        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        scalarAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int>(-5));

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        auto expectedResults = std::vector<std::pair<value::TypeTags, value::Value>>{
            makeNothing(),  // string
            makeInt32(-2),
            makeInt32(2),
            makeNothing(),  // bool
            makeDouble(0.0),
            makeDouble(fmod(
                numericCast<double>(value::TypeTags::NumberDouble,
                                    value::bitcastFrom<double>(-1234.987)),
                numericCast<double>(value::TypeTags::NumberInt32, value::bitcastFrom<int>(-5)))),
            makeDouble(fmod(
                numericCast<double>(value::TypeTags::NumberDouble,
                                    value::bitcastFrom<double>(1987.956)),
                numericCast<double>(value::TypeTags::NumberInt32, value::bitcastFrom<int>(-5)))),
            makeDecimal("-4.987"),
            makeDecimal("2.956"),
            makeInt64(-2),
            makeInt64(1),
            makeNothing(),  // Nothing
            makeNothing(),  // Null
        };

        assertBlockEq(runTag, runVal, expectedResults);

        for (size_t i = 0; i < expectedResults.size(); ++i) {
            releaseValue(expectedResults[i].first, expectedResults[i].second);
        }
    }

    // mod is positive double
    {
        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        scalarAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(1.2));

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        auto expectedResults = std::vector<std::pair<value::TypeTags, value::Value>>{
            makeNothing(),  // string
            makeDouble(
                fmod(numericCast<double>(value::TypeTags::NumberInt32, value::bitcastFrom<int>(-2)),
                     numericCast<double>(value::TypeTags::NumberDouble,
                                         value::bitcastFrom<double>(1.2)))),
            makeDouble(
                fmod(numericCast<double>(value::TypeTags::NumberInt32, value::bitcastFrom<int>(2)),
                     numericCast<double>(value::TypeTags::NumberDouble,
                                         value::bitcastFrom<double>(1.2)))),
            makeNothing(),  // bool
            makeDouble(0.0),
            makeDouble(fmod(numericCast<double>(value::TypeTags::NumberDouble,
                                                value::bitcastFrom<double>(-1234.987)),
                            numericCast<double>(value::TypeTags::NumberDouble,
                                                value::bitcastFrom<double>(1.2)))),
            makeDouble(fmod(numericCast<double>(value::TypeTags::NumberDouble,
                                                value::bitcastFrom<double>(1987.956)),
                            numericCast<double>(value::TypeTags::NumberDouble,
                                                value::bitcastFrom<double>(1.2)))),
            makeDecimal("-0.187"),
            makeDecimal("0.756"),
            makeDouble(fmod(
                numericCast<double>(value::TypeTags::NumberInt64, value::bitcastFrom<int>(-54687)),
                numericCast<double>(value::TypeTags::NumberDouble,
                                    value::bitcastFrom<double>(1.2)))),
            makeDouble(fmod(
                numericCast<double>(value::TypeTags::NumberInt64, value::bitcastFrom<int>(25166)),
                numericCast<double>(value::TypeTags::NumberDouble,
                                    value::bitcastFrom<double>(1.2)))),
            makeNothing(),  // Nothing
            makeNothing(),  // Null
        };

        for (size_t i = 0; i < expectedResults.size(); ++i) {
            releaseValue(expectedResults[i].first, expectedResults[i].second);
        }
    }

    // mod is negative double
    {
        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        scalarAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(-1.2));

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        auto expectedResults = std::vector<std::pair<value::TypeTags, value::Value>>{
            makeNothing(),  // string
            makeDouble(
                fmod(numericCast<double>(value::TypeTags::NumberInt32, value::bitcastFrom<int>(-2)),
                     numericCast<double>(value::TypeTags::NumberDouble,
                                         value::bitcastFrom<double>(-1.2)))),
            makeDouble(
                fmod(numericCast<double>(value::TypeTags::NumberInt32, value::bitcastFrom<int>(2)),
                     numericCast<double>(value::TypeTags::NumberDouble,
                                         value::bitcastFrom<double>(-1.2)))),
            makeNothing(),  // bool
            makeDouble(0.0),
            makeDouble(fmod(numericCast<double>(value::TypeTags::NumberDouble,
                                                value::bitcastFrom<double>(-1234.987)),
                            numericCast<double>(value::TypeTags::NumberDouble,
                                                value::bitcastFrom<double>(-1.2)))),
            makeDouble(fmod(numericCast<double>(value::TypeTags::NumberDouble,
                                                value::bitcastFrom<double>(1987.956)),
                            numericCast<double>(value::TypeTags::NumberDouble,
                                                value::bitcastFrom<double>(-1.2)))),
            makeDecimal("-0.187"),
            makeDecimal("0.756"),
            makeDouble(fmod(
                numericCast<double>(value::TypeTags::NumberInt64, value::bitcastFrom<int>(-54687)),
                numericCast<double>(value::TypeTags::NumberDouble,
                                    value::bitcastFrom<double>(-1.2)))),
            makeDouble(fmod(
                numericCast<double>(value::TypeTags::NumberInt64, value::bitcastFrom<int>(25166)),
                numericCast<double>(value::TypeTags::NumberDouble,
                                    value::bitcastFrom<double>(-1.2)))),
            makeNothing(),  // Nothing
            makeNothing(),  // Null
        };

        assertBlockEq(runTag, runVal, expectedResults);

        for (size_t i = 0; i < expectedResults.size(); ++i) {
            releaseValue(expectedResults[i].first, expectedResults[i].second);
        }
    }

    // mod is positive decimal
    {
        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        auto dec = makeDecimal("8.56");
        value::ValueGuard decGuard(dec.first, dec.second);
        scalarAccessor.reset(dec.first, dec.second);

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        auto expectedResults = std::vector<std::pair<value::TypeTags, value::Value>>{
            makeNothing(),  // string
            makeDecimal("-2.00"),
            makeDecimal("2.00"),
            makeNothing(),  // bool
            makeDecimal("0.00"),
            makeDecimal("-2.347"),
            makeDecimal("2.036"),
            makeDecimal("-2.347"),
            makeDecimal("2.036"),
            makeDecimal("-5.72"),
            makeDecimal("8.16"),
            makeNothing(),  // Nothing
            makeNothing(),  // Null
        };

        assertBlockEq(runTag, runVal, expectedResults);

        for (size_t i = 0; i < expectedResults.size(); ++i) {
            releaseValue(expectedResults[i].first, expectedResults[i].second);
        }
    }

    // mod is negative decimal
    {
        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        auto dec = makeDecimal("-8.56");
        value::ValueGuard decGuard(dec.first, dec.second);
        scalarAccessor.reset(dec.first, dec.second);

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        auto expectedResults = std::vector<std::pair<value::TypeTags, value::Value>>{
            makeNothing(),  // string
            makeDecimal("-2.00"),
            makeDecimal("2.00"),
            makeNothing(),  // bool
            makeDecimal("0.00"),
            makeDecimal("-2.347"),
            makeDecimal("2.036"),
            makeDecimal("-2.347"),
            makeDecimal("2.036"),
            makeDecimal("-5.72"),
            makeDecimal("8.16"),
            makeNothing(),  // Nothing
            makeNothing(),  // Null
        };

        assertBlockEq(runTag, runVal, expectedResults);

        for (size_t i = 0; i < expectedResults.size(); ++i) {
            releaseValue(expectedResults[i].first, expectedResults[i].second);
        }
    }

    // mod is string
    {
        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        auto md = value::makeSmallString("abc"_sd);
        value::ValueGuard mdGuard(md.first, md.second);
        scalarAccessor.reset(md.first, md.second);

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        auto expectedResults =
            std::vector<std::pair<value::TypeTags, value::Value>>(13, makeNothing());

        assertBlockEq(runTag, runVal, expectedResults);

        for (size_t i = 0; i < expectedResults.size(); ++i) {
            releaseValue(expectedResults[i].first, expectedResults[i].second);
        }
    }

    {
        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        scalarAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int>(0));

        ASSERT_THROWS_CODE(runCompiledExpression(compiledExpr.get()),
                           DBException,
                           4848403);  // mod with zero
    }

    {
        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        scalarAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int>(0));

        ASSERT_THROWS_CODE(runCompiledExpression(compiledExpr.get()),
                           DBException,
                           4848403);  // mod with zero
    }

    {
        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        scalarAccessor.reset(value::TypeTags::NumberDouble, value::bitcastFrom<double>(0));

        ASSERT_THROWS_CODE(runCompiledExpression(compiledExpr.get()),
                           DBException,
                           4848403);  // mod with zero
    }

    {
        blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(&block));
        auto md = makeDecimal("0");
        value::ValueGuard mdGuard(md.first, md.second);
        scalarAccessor.reset(md.first, md.second);

        ASSERT_THROWS_CODE(runCompiledExpression(compiledExpr.get()),
                           DBException,
                           4848403);  // mod with zero
    }
}

TEST_F(SBEBlockExpressionTest, BlockDateAdd) {
    value::ViewOfValueAccessor blockAccessor;
    value::ViewOfValueAccessor bitsetAccessor;

    auto blockSlot = bindAccessor(&blockAccessor);
    auto bitsetSlot = bindAccessor(&bitsetAccessor);

    auto block = makeTestDateBlock();

    auto tzdb = std::make_unique<TimeZoneDatabase>();

    auto expr = sbe::makeE<sbe::EFunction>(
        "valueBlockDateAdd",
        sbe::makeEs(makeE<EVariable>(bitsetSlot),
                    makeE<EVariable>(blockSlot),
                    makeE<EConstant>(value::TypeTags::timeZoneDB,
                                     value::bitcastFrom<TimeZoneDatabase*>(tzdb.get())),
                    makeE<EConstant>("millisecond"_sd),
                    makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int>(1)),
                    makeE<EConstant>("UTC"_sd)));

    auto compiledExpr = compileExpression(*expr);

    {
        // empty bitset
        bitsetAccessor.reset(value::TypeTags::Nothing, 0);
        blockAccessor.reset(value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(block.get()));

        ASSERT_THROWS_CODE(
            runCompiledExpression(compiledExpr.get()), DBException, ErrorCodes::DurationOverflow);
    }
    {
        // bitset masking the value that would overflow
        auto bitset = makeHeterogeneousBoolBlock({true, true, true, true, false, true});
        bitsetAccessor.reset(value::TypeTags::valueBlock,
                             value::bitcastFrom<value::ValueBlock*>(bitset.get()));
        blockAccessor.reset(value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(block.get()));

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        auto expectedResults = std::vector<std::pair<value::TypeTags, value::Value>>{
            {value::TypeTags::Date, value::bitcastFrom<int64_t>(0)},
            {value::TypeTags::Date, value::bitcastFrom<int64_t>(1)},
            {value::TypeTags::Date, value::bitcastFrom<int64_t>(2)},
            {value::TypeTags::Date,
             value::bitcastFrom<int64_t>(std::numeric_limits<int64_t>::min() + 1)},
            makeNothing(),
            makeNothing(),
        };

        assertBlockEq(runTag, runVal, expectedResults);
    }
}
}  // namespace mongo::sbe
