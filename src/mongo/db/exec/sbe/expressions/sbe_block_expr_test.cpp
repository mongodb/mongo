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

    void testFoldF(std::vector<bool> vals,
                   std::vector<char> filterPosInfo,
                   std::vector<bool> expectedResult);

    void testCmpScalar(const std::vector<std::pair<value::TypeTags, value::Value>>& testValues,
                       EPrimBinary::Op,
                       StringData cmpFunctionName);
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

    auto valBlock = makeBoolBlock(vals);
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

void SBEBlockExpressionTest::testCmpScalar(
    const std::vector<std::pair<value::TypeTags, value::Value>>& testValues,
    EPrimBinary::Op scalarOp,
    StringData cmpFunctionName) {

    value::ViewOfValueAccessor valBlockAccessor;
    value::ViewOfValueAccessor scalarAccessorLhs;
    value::ViewOfValueAccessor scalarAccessorRhs;
    auto valBlockSlot = bindAccessor(&valBlockAccessor);
    auto scalarSlotLhs = bindAccessor(&scalarAccessorLhs);
    auto scalarSlotRhs = bindAccessor(&scalarAccessorRhs);

    auto valBlock = std::make_unique<value::HeterogeneousBlock>();
    for (auto [t, v] : testValues) {
        auto [cpyT, cpyV] = value::copyValue(t, v);
        valBlock->push_back(cpyT, cpyV);
    }

    valBlockAccessor.reset(sbe::value::TypeTags::valueBlock,
                           value::bitcastFrom<value::ValueBlock*>(valBlock.get()));

    auto expr = makeE<sbe::EFunction>(
        cmpFunctionName,
        sbe::makeEs(makeE<EVariable>(valBlockSlot), makeE<EVariable>(scalarSlotRhs)));
    auto compiledExpr = compileExpression(*expr);

    auto scalarExpr = makeE<sbe::EPrimBinary>(
        scalarOp, makeE<EVariable>(scalarSlotLhs), makeE<EVariable>(scalarSlotRhs));
    auto compiledScalarExpr = compileExpression(*scalarExpr);

    for (auto [t, v] : testValues) {
        scalarAccessorRhs.reset(t, v);

        // Run the block expression and get the result.
        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(runTag, runVal);

        ASSERT_EQ(runTag, value::TypeTags::valueBlock);
        auto* resultValBlock = value::getValueBlock(runVal);
        auto resultExtracted = resultValBlock->extract();

        ASSERT_EQ(resultExtracted.count, testValues.size());

        for (size_t i = 0; i < resultExtracted.count; ++i) {
            // Determine the expected result.
            scalarAccessorLhs.reset(testValues[i].first, testValues[i].second);
            auto [expectedTag, expectedVal] = runCompiledExpression(compiledScalarExpr.get());
            value::ValueGuard guard(expectedTag, expectedVal);


            auto [gotTag, gotVal] = resultExtracted[i];

            auto [cmpTag, cmpVal] = value::compareValue(gotTag, gotVal, expectedTag, expectedVal);
            ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32) << gotTag << " " << expectedTag;
            ASSERT_EQ(value::bitcastTo<int32_t>(cmpVal), 0)
                << "Comparing " << std::pair(t, v) << " " << testValues[i] << " and got "
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

    ON_BLOCK_EXIT([&]() {
        for (auto [t, v] : testValues) {
            value::releaseValue(t, v);
        }
    });

    testCmpScalar(testValues, EPrimBinary::greater, "valueBlockGtScalar");
    testCmpScalar(testValues, EPrimBinary::greaterEq, "valueBlockGteScalar");
    testCmpScalar(testValues, EPrimBinary::less, "valueBlockLtScalar");
    testCmpScalar(testValues, EPrimBinary::lessEq, "valueBlockLteScalar");
    testCmpScalar(testValues, EPrimBinary::eq, "valueBlockEqScalar");
    testCmpScalar(testValues, EPrimBinary::neq, "valueBlockNeqScalar");
}

}  // namespace mongo::sbe
