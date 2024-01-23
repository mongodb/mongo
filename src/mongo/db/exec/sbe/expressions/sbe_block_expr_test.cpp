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
        ASSERT_EQ(expected.size(), extracted.count);

        for (size_t i = 0; i < extracted.count; ++i) {
            auto [t, v] = value::compareValue(
                extracted.tags[i], extracted.vals[i], expected[i].first, expected[i].second);
            ASSERT_EQ(t, value::TypeTags::NumberInt32) << extracted;
            ASSERT_EQ(value::bitcastTo<int32_t>(v), 0)
                << "Got " << extracted[i] << " expected " << expected[i]
                << " full extracted output " << extracted;
        }
    }

    void testFoldF(std::vector<bool> vals,
                   std::vector<char> filterPosInfo,
                   std::vector<bool> expectedResult);

    void testCmpScalar(EPrimBinary::Op, StringData cmpFunctionName, value::ValueBlock* valBlock);

    std::pair<std::vector<bool>, std::vector<bool>> naiveLogicalAndOr(
        std::unique_ptr<value::ValueBlock> leftBlock,
        std::unique_ptr<value::ValueBlock> rightBlock) {
        auto left = leftBlock->extract();
        auto right = rightBlock->extract();
        ASSERT_EQ(left.count, right.count);

        std::vector<bool> andRes;
        std::vector<bool> orRes;

        for (size_t i = 0; i < left.count; ++i) {
            ASSERT_EQ(left.tags[i], value::TypeTags::Boolean);
            ASSERT_EQ(right.tags[i], value::TypeTags::Boolean);
            auto leftBool = value::bitcastTo<bool>(left.vals[i]);
            auto rightBool = value::bitcastTo<bool>(right.vals[i]);
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

        auto compiledExpr = sbe::makeE<sbe::EFunction>("valueBlockCount",
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
            "valueBlockSum",
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
            "valueBlockMin",
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
            "valueBlockMax",
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
            "valueBlockMin",
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
            "valueBlockMax",
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
        std::make_unique<value::MonoBlock>(*leftBlock->tryCount(), fTag, fVal);
    falseMonoBlockAccessor.reset(sbe::value::TypeTags::valueBlock,
                                 value::bitcastFrom<value::ValueBlock*>(falseMonoBlock.get()));

    auto [tTag, tVal] = makeBool(true);
    std::unique_ptr<value::ValueBlock> trueMonoBlock =
        std::make_unique<value::MonoBlock>(*leftBlock->tryCount(), tTag, tVal);
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

void SBEBlockExpressionTest::testFoldF(std::vector<bool> vals,
                                       std::vector<char> filterPosInfo,
                                       std::vector<bool> expectedResult) {

    value::ViewOfValueAccessor valBlockAccessor;
    value::ViewOfValueAccessor cellBlockAccessor;
    auto valBlockSlot = bindAccessor(&valBlockAccessor);
    auto cellBlockSlot = bindAccessor(&cellBlockAccessor);

    auto materializedCellBlock = std::make_unique<value::MaterializedCellBlock>();
    materializedCellBlock->_deblocked = nullptr;  // This is never read by the test.
    materializedCellBlock->_filterPosInfo = filterPosInfo;

    auto valBlock = makeHeterogeneousBoolBlock(vals);
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
    testFoldF({true, true, false, false, true},  // Values.
              {},                                // Position info.
              {true, true, false, false, true}   // Expected result.
    );

    testFoldF({true, true, false, false, true},  // Values.
              {1, 1, 1, 0, 1},                   // Position info.
              {true, true, false, true}          // Expected result.
    );

    //
    // Non-empty position info edge case tests.
    //

    testFoldF({false},  // Values.
              {1},      // Position info.
              {false}   // Expected result.
    );

    testFoldF({true},  // Values.
              {1},     // Position info.
              {true}   // Expected result.
    );

    testFoldF({true, true, false, false, true},  // Values.
              {1, 0, 0, 0, 0},                   // Position info.
              {true}                             // Expected result.
    );
    testFoldF({true, true, false, false, true},  // Values.
              {1, 1, 1, 1, 0},                   // Position info.
              {true, true, false, true}          // Expected result.
    );
    testFoldF({false, false, false, false, false},  // Values.
              {1, 0, 0, 0, 0},                      // Position info.
              {false}                               // Expected result.
    );
    testFoldF({false, false, false, false, false},  // Values.
              {1, 0, 1, 0, 0},                      // Position info.
              {false, false}                        // Expected result.
    );
    testFoldF({false, false, false, true},  // Values.
              {1, 0, 0, 1},                 // Position info.
              {false, true}                 // Expected result.
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

    for (size_t i = 0; i < deblocked.count; ++i) {
        scalarAccessorRhs.reset(deblocked.tags[i], deblocked.vals[i]);

        // Run the block expression and get the result.
        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        ASSERT_EQ(runTag, value::TypeTags::valueBlock);
        auto* resultValBlock = value::getValueBlock(runVal);
        auto resultExtracted = resultValBlock->extract();

        ASSERT_EQ(resultExtracted.count, deblocked.count);

        for (size_t j = 0; j < resultExtracted.count; ++j) {
            // Determine the expected result.
            scalarAccessorLhs.reset(deblocked.tags[j], deblocked.vals[j]);
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
}  // namespace mongo::sbe
