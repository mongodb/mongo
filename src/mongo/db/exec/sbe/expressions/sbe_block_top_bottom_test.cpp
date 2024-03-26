/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_block_test_helpers.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/assert.h"

namespace mongo::sbe {

class SBEBlockTopBottomTest : public EExpressionTestFixture {
public:
    std::pair<std::vector<std::unique_ptr<value::ValueBlock>>,
              std::vector<std::unique_ptr<value::ValueBlock>>>
    makeBlockTopBottomNInputs(const std::vector<TypedValues>& inputKeys,
                              const std::vector<TypedValues>& outVals,
                              size_t startIdx = 0) {
        invariant(inputKeys.size() == outVals.size());
        std::vector<std::unique_ptr<value::ValueBlock>> inputKeyBlocks;
        std::vector<std::unique_ptr<value::ValueBlock>> outValBlocks;
        if (inputKeys.size() == 0) {
            return {std::move(inputKeyBlocks), std::move(outValBlocks)};
        }
        for (size_t j = 0; j < inputKeys[0].size(); ++j) {
            inputKeyBlocks.push_back(std::make_unique<value::HeterogeneousBlock>());
            outValBlocks.push_back(std::make_unique<value::HeterogeneousBlock>());
            for (size_t i = 0; i < inputKeys.size(); ++i) {
                invariant(inputKeys[i].size() == inputKeys[0].size() &&
                          inputKeys[i].size() == outVals[i].size());
                inputKeyBlocks[j]->as<value::HeterogeneousBlock>()->push_back(
                    value::copyValue(inputKeys[i][j].first, inputKeys[i][j].second));
                auto [outValArrTag, outValArrVal] = value::makeNewArray();
                auto* outValArr = value::getArrayView(outValArrVal);
                outValArr->push_back(value::copyValue(outVals[i][j].first, outVals[i][j].second));
                outValArr->push_back(makeInt64(i + startIdx));
                outValBlocks[j]->as<value::HeterogeneousBlock>()->push_back(outValArrTag,
                                                                            outValArrVal);
            }
        }

        return {std::move(inputKeyBlocks), std::move(outValBlocks)};
    }

    std::pair<TypedValue, TypedValue> executeBlockTopBottomN(
        const std::vector<std::unique_ptr<value::ValueBlock>>& inputKeyBlocks,
        const std::vector<std::unique_ptr<value::ValueBlock>>& outValBlocks,
        TypedValue topNState,
        TypedValue bottomNState,
        const std::vector<bool>& bitset,
        SortSpec sortSpec) {
        invariant(inputKeyBlocks.size() == outValBlocks.size() && inputKeyBlocks.size() == 1);

        value::ViewOfValueAccessor keyBlockAccessor;
        value::ViewOfValueAccessor valBlockAccessor;
        value::ViewOfValueAccessor bitsetAccessor;
        value::ViewOfValueAccessor sortSpecAccessor;
        value::OwnedValueAccessor topNAggAccessor;
        value::OwnedValueAccessor bottomNAggAccessor;
        auto keyBlockSlot = bindAccessor(&keyBlockAccessor);
        auto valBlockSlot = bindAccessor(&valBlockAccessor);
        auto bitsetSlot = bindAccessor(&bitsetAccessor);
        auto sortSpecSlot = bindAccessor(&sortSpecAccessor);

        value::TypeTags numKeyBlocksTag{value::TypeTags::Null};
        value::Value numKeyBlocksVal{0u};

        auto topNExpr = sbe::makeE<sbe::EFunction>(
            "valueBlockAggTopN",
            sbe::makeEs(makeE<EVariable>(bitsetSlot),
                        makeE<EVariable>(sortSpecSlot),
                        makeE<EConstant>(numKeyBlocksTag, numKeyBlocksVal),
                        makeE<EVariable>(keyBlockSlot),
                        makeE<EVariable>(valBlockSlot)));
        auto compiledTopNExpr = compileAggExpression(*topNExpr, &topNAggAccessor);

        auto bottomNExpr = sbe::makeE<sbe::EFunction>(
            "valueBlockAggBottomN",
            sbe::makeEs(makeE<EVariable>(bitsetSlot),
                        makeE<EVariable>(sortSpecSlot),
                        makeE<EConstant>(numKeyBlocksTag, numKeyBlocksVal),
                        makeE<EVariable>(keyBlockSlot),
                        makeE<EVariable>(valBlockSlot)));
        auto compiledBottomNExpr = compileAggExpression(*bottomNExpr, &bottomNAggAccessor);

        value::ValueBlock* keyBlock = inputKeyBlocks[0].get();
        value::ValueBlock* valBlock = outValBlocks[0].get();
        keyBlockAccessor.reset(sbe::value::TypeTags::valueBlock,
                               value::bitcastFrom<value::ValueBlock*>(keyBlock));
        valBlockAccessor.reset(sbe::value::TypeTags::valueBlock,
                               value::bitcastFrom<value::ValueBlock*>(valBlock));

        auto bitsetBlock = makeHeterogeneousBoolBlock(bitset);
        bitsetAccessor.reset(sbe::value::TypeTags::valueBlock,
                             value::bitcastFrom<value::ValueBlock*>(bitsetBlock.get()));

        // sortSpec is owned by the caller
        auto [sortSpecTag, sortSpecVal] =
            std::pair{sbe::value::TypeTags::sortSpec, value::bitcastFrom<SortSpec*>(&sortSpec)};
        sortSpecAccessor.reset(sortSpecTag, sortSpecVal);

        // Add both states to their OwnedValueAccessors so they can be released if an exception is
        // thrown.
        topNAggAccessor.reset(topNState.first, topNState.second);
        bottomNAggAccessor.reset(bottomNState.first, bottomNState.second);

        auto topNRes = runCompiledExpression(compiledTopNExpr.get());
        auto bottomNRes = runCompiledExpression(compiledBottomNExpr.get());

        return {topNRes, bottomNRes};
    }

    std::pair<TypedValue, TypedValue> finalizeTopBottomN(TypedValue topNRes,
                                                         TypedValue bottomNRes,
                                                         SortSpec sortSpec) {
        value::ViewOfValueAccessor stateAccessor;
        value::ViewOfValueAccessor sortSpecAccessor;
        auto stateSlot = bindAccessor(&stateAccessor);
        auto sortSpecSlot = bindAccessor(&sortSpecAccessor);

        // topNFinalize and bottomNFinalize both call into builtinAggTopBottomNFinalize with no
        // unique parameters.
        auto finalizeExpr = sbe::makeE<sbe::EFunction>(
            "aggTopNFinalize",
            sbe::makeEs(makeE<EVariable>(stateSlot), makeE<EVariable>(sortSpecSlot)));

        auto compiledExpr = compileExpression(*finalizeExpr);

        // sortSpec is owned by the caller
        sortSpecAccessor.reset(sbe::value::TypeTags::sortSpec,
                               value::bitcastFrom<SortSpec*>(&sortSpec));

        stateAccessor.reset(topNRes.first, topNRes.second);
        auto topNFinal = runCompiledExpression(compiledExpr.get());
        ASSERT_EQ(topNFinal.first, value::TypeTags::Array);

        stateAccessor.reset(bottomNRes.first, bottomNRes.second);
        auto bottomNFinal = runCompiledExpression(compiledExpr.get());
        ASSERT_EQ(bottomNFinal.first, value::TypeTags::Array);

        return {topNFinal, bottomNFinal};
    }

    std::pair<TypedValue, TypedValue> executeAndFinalizeTopBottomN(
        const std::vector<std::unique_ptr<value::ValueBlock>>& keyBlocks,
        const std::vector<std::unique_ptr<value::ValueBlock>>& valBlocks,
        TypedValue topNState,
        TypedValue bottomNState,
        const std::vector<bool>& bitset,
        SortSpec sortSpec) {
        auto [newTopNState, newBottomNState] =
            executeBlockTopBottomN(keyBlocks, valBlocks, topNState, bottomNState, bitset, sortSpec);

        value::ValueGuard topNResGuard{newTopNState};
        value::ValueGuard bottomNResGuard{newBottomNState};

        auto [topNFinal, bottomNFinal] =
            finalizeTopBottomN(newTopNState, newBottomNState, sortSpec);

        return {topNFinal, bottomNFinal};
    }

    std::vector<TypedValues> blocksTo2dVector(
        const std::vector<std::unique_ptr<value::ValueBlock>>& blocks) {
        if (blocks.size() == 0) {
            return {{}};
        }
        auto blockSize = blocks[0]->count();
        std::vector<TypedValues> typedValVecs(blockSize);
        for (size_t i = 0; i < blocks.size(); ++i) {
            invariant(*blocks[i]->tryCount() == *blocks[0]->tryCount());
            auto deblocked = blocks[i]->extract();
            for (size_t j = 0; j < deblocked.count(); ++j) {
                // Capture fillEmpty(null) semantics.
                if (deblocked.tags()[j] == value::TypeTags::Nothing) {
                    typedValVecs[j].push_back({value::TypeTags::Null, value::Value{0u}});
                } else {
                    typedValVecs[j].push_back(deblocked[j]);
                }
            }
        }

        return typedValVecs;
    }

    void verifyTopBottomNOutput(const std::vector<std::unique_ptr<value::ValueBlock>>& keyBlocks,
                                TypedValue finalRes,
                                const SortSpec& sortSpec,
                                StringData builtinName) {
        auto inputKeys = blocksTo2dVector(keyBlocks);
        ASSERT_EQ(finalRes.first, value::TypeTags::Array) << builtinName;
        auto* finalArr = value::getArrayView(finalRes.second);

        TypedValues sortKeys;
        stdx::unordered_map<size_t, bool> seenIdxs;
        for (auto [outTag, outVal] : finalArr->values()) {
            ASSERT_EQ(outTag, value::TypeTags::Array) << builtinName;
            auto* outArr = value::getArrayView(outVal);
            ASSERT_EQ(outArr->size(), 2) << builtinName;

            auto [outIdxTag, outIdxVal] = outArr->getAt(1);
            ASSERT_EQ(outIdxTag, value::TypeTags::NumberInt64) << builtinName;
            size_t outIdx = value::bitcastTo<size_t>(outIdxVal);

            auto [_, newIdx] = seenIdxs.insert({outIdx, true});
            invariant(newIdx);
            invariant(inputKeys[outIdx].size() == 1);

            auto [sortKeyTag, sortKeyVal] = inputKeys[outIdx][0];
            if (!sortKeys.empty()) {
                auto [t, v] = sortSpec.compare(
                    sortKeys.back().first, sortKeys.back().second, sortKeyTag, sortKeyVal);
                ASSERT_EQ(t, value::TypeTags::NumberInt32) << builtinName;
                ASSERT_LT(value::bitcastTo<int32_t>(v), 0) << builtinName;
                sortKeys.push_back(TypedValue{sortKeyTag, sortKeyVal});
            }
        }

        if (sortKeys.empty()) {
            return;
        }

        for (size_t i = 0; i < keyBlocks.size(); ++i) {
            // Don't compare against sort keys that are in the final output that we already verified
            // compare >= to sortKeys.back().
            if (seenIdxs.contains(i)) {
                continue;
            }

            invariant(inputKeys[i].size() == 1);
            auto [sortKeyTag, sortKeyVal] = inputKeys[i][0];
            auto [t, v] = sortSpec.compare(
                sortKeys.back().first, sortKeys.back().second, sortKeyTag, sortKeyVal);
            ASSERT_EQ(t, value::TypeTags::NumberInt32) << builtinName;
            ASSERT_LT(value::bitcastTo<int32_t>(v), 0) << builtinName;
            sortKeys.push_back(TypedValue{sortKeyTag, sortKeyVal});
        }
    }

    void topBottomNOracleTest(const std::vector<std::unique_ptr<value::ValueBlock>>& keyBlocks,
                              const std::vector<std::unique_ptr<value::ValueBlock>>& valBlocks,
                              const std::vector<std::vector<bool>>& bitsets,
                              size_t maxSizeMax) {
        for (const auto& bitset : bitsets) {
            std::vector<int> sortDirections{-1 /* ascending */, 1 /* descending */};
            for (auto sd : sortDirections) {
                SortSpec sortSpec{BSON("sort_field" << sd)};

                // maxSize = 0 is not valid.
                for (size_t maxSize = 1; maxSize <= maxSizeMax; ++maxSize) {
                    auto [topNFinal, bottomNFinal] =
                        executeAndFinalizeTopBottomN(keyBlocks,
                                                     valBlocks,
                                                     makeEmptyState(maxSize),
                                                     makeEmptyState(maxSize),
                                                     bitset,
                                                     sortSpec);

                    value::ValueGuard topNFinalGuard{topNFinal};
                    value::ValueGuard bottomNFinalGuard{bottomNFinal};

                    TypedValues finalRes{topNFinal, bottomNFinal};
                    std::vector<StringData> accType{"valueBlockAggTopN"_sd,
                                                    "valueBlockAggBottomN"_sd};
                    for (size_t i = 0; i < finalRes.size(); ++i) {
                        verifyTopBottomNOutput(keyBlocks, finalRes[i], sortSpec, accType[i]);
                    }
                }
            }
        }
    }
};

TEST_F(SBEBlockTopBottomTest, TopBottomNSingleKeySingleOutputTest) {
    // Tests with Decimal128s to test memory management while still being easy to reason about.

    // Field path "a"
    std::vector<TypedValues> inputKeys{{makeDecimal("6")},
                                       {makeDecimal("2")},
                                       {makeNull()},
                                       {makeDecimal("1")},
                                       {makeDecimal("5")}};

    std::vector<TypedValues> outVals{{makeDecimal("25")},
                                     {makeDecimal("50")},
                                     {makeDecimal("75")},
                                     {makeDecimal("100")},
                                     {makeDecimal("125")}};

    SortSpec sortSpec{BSON("a" << -1)};

    auto addToCombinedBlocks =
        [](std::vector<std::unique_ptr<value::ValueBlock>>& combinedKeyBlocks,
           std::vector<TypedValues> inputKeys) {
            for (size_t j = 0; j < inputKeys[0].size(); ++j) {
                for (size_t i = 0; i < inputKeys.size(); ++i) {
                    invariant(inputKeys[i].size() == inputKeys[0].size());
                    combinedKeyBlocks[j]->as<value::HeterogeneousBlock>()->push_back(
                        value::copyValue(inputKeys[i][j].first, inputKeys[i][j].second));
                }
            }
        };

    auto runHandwrittenTest = [&, this](std::vector<std::vector<TypedValues>> inputKeysVec,
                                        std::vector<std::vector<TypedValues>> outValsVec,
                                        std::vector<bool> bitset,
                                        size_t maxSize,
                                        size_t numIters = 1,
                                        int32_t memLimit = std::numeric_limits<int32_t>::max()) {
        invariant(inputKeysVec.size() == numIters && outValsVec.size() == numIters);

        auto topNState = makeEmptyState(maxSize, memLimit);
        auto bottomNState = makeEmptyState(maxSize, memLimit);

        size_t startIdx = 0;

        // Initialize the combined blocks.
        std::vector<std::unique_ptr<value::ValueBlock>> combinedKeyBlocks;
        for (size_t j = 0;
             !inputKeysVec.empty() && !inputKeysVec[0].empty() && j < inputKeysVec[0][0].size();
             ++j) {
            combinedKeyBlocks.push_back(std::make_unique<value::HeterogeneousBlock>());
        }

        for (size_t iter = 0; iter < numIters - 1; ++iter) {
            auto [keyBlocks, valBlocks] =
                makeBlockTopBottomNInputs(inputKeysVec[iter], outValsVec[iter], startIdx);
            startIdx += inputKeysVec[iter].size();

            std::tie(topNState, bottomNState) = executeBlockTopBottomN(
                keyBlocks, valBlocks, topNState, bottomNState, bitset, sortSpec);

            // Add to the keys we have encountered so far.
            addToCombinedBlocks(combinedKeyBlocks, inputKeysVec[iter]);

            // Verify that intermediate results are still correct.
            auto [topNInter, bottomNInter] = finalizeTopBottomN(topNState, bottomNState, sortSpec);
            value::ValueGuard topNInterGuard{topNInter};
            value::ValueGuard bottomNInterGuard{bottomNInter};

            verifyTopBottomNOutput(combinedKeyBlocks, topNInter, sortSpec, "valueBlockAggTopN");
            verifyTopBottomNOutput(
                combinedKeyBlocks, bottomNInter, sortSpec, "valueBlockAggBottomN");
        }

        auto [keyBlocks, valBlocks] =
            makeBlockTopBottomNInputs(inputKeysVec.back(), outValsVec.back(), startIdx);

        auto [topNFinal, bottomNFinal] = executeAndFinalizeTopBottomN(
            keyBlocks, valBlocks, topNState, bottomNState, bitset, sortSpec);

        value::ValueGuard topNFinalGuard{topNFinal};
        value::ValueGuard bottomNFinalGuard{bottomNFinal};

        // Add to the keys we have encountered so far.
        addToCombinedBlocks(combinedKeyBlocks, inputKeysVec.back());

        TypedValues finalRes{topNFinal, bottomNFinal};
        std::vector<StringData> accType{"valueBlockAggTopN"_sd, "valueBlockAggBottomN"_sd};
        for (size_t i = 0; i < finalRes.size(); ++i) {
            verifyTopBottomNOutput(combinedKeyBlocks, finalRes[i], sortSpec, accType[i]);
        }
    };

    {
        // Input bitset is all false
        std::vector<bool> falseBitset(inputKeys.size(), false);

        runHandwrittenTest({inputKeys}, {outVals}, falseBitset, 3 /* maxSize */);
    }

    {
        // Input bitset is all true
        std::vector<bool> trueBitset(inputKeys.size(), true);

        runHandwrittenTest({inputKeys}, {outVals}, trueBitset, 3 /* maxSize */);
    }

    std::vector<bool> bitset{false, true, true, true, true};

    {
        // Input bitset has trues and falses
        runHandwrittenTest({inputKeys}, {outVals}, bitset, 3 /* maxSize */);
    }

    {
        // An exception should be throw when we exceed the state's memory limit.
        ASSERT_THROWS_CODE(runHandwrittenTest({inputKeys},
                                              {outVals},
                                              bitset,
                                              3 /* maxSize */,
                                              1 /* numIters */,
                                              64 /* memLimit */),
                           DBException,
                           ErrorCodes::ExceededMemoryLimit);
    }

    {
        // maxSize >= # of trues always returns the same results.
        size_t numTrues = 4;

        auto [keyBlocks, valBlocks] = makeBlockTopBottomNInputs(inputKeys, outVals);
        auto [topNFinal1, bottomNFinal1] = executeAndFinalizeTopBottomN(keyBlocks,
                                                                        valBlocks,
                                                                        makeEmptyState(numTrues),
                                                                        makeEmptyState(numTrues),
                                                                        bitset,
                                                                        sortSpec);

        value::ValueGuard topNFinalGuard1{topNFinal1};
        value::ValueGuard bottomNFinalGuard1{bottomNFinal1};

        std::tie(keyBlocks, valBlocks) = makeBlockTopBottomNInputs(inputKeys, outVals);
        auto [topNFinal2, bottomNFinal2] =
            executeAndFinalizeTopBottomN(keyBlocks,
                                         valBlocks,
                                         makeEmptyState(numTrues + 1),
                                         makeEmptyState(numTrues + 1),
                                         bitset,
                                         sortSpec);

        value::ValueGuard topNFinalGuard2{topNFinal2};
        value::ValueGuard bottomNFinalGuard2{bottomNFinal2};

        // Compare topN results.
        auto [t, v] = value::compareValue(
            topNFinal1.first, topNFinal1.second, topNFinal2.first, topNFinal2.second);
        ASSERT_EQ(t, value::TypeTags::NumberInt32) << "valueBlockAggTopN";
        ASSERT_EQ(value::bitcastTo<int32_t>(v), 0) << "valueBlockAggTopN";

        // Compre bottomN results.
        std::tie(t, v) = value::compareValue(
            bottomNFinal1.first, bottomNFinal1.second, bottomNFinal2.first, bottomNFinal2.second);
        ASSERT_EQ(t, value::TypeTags::NumberInt32) << "valueBlockAggBottomN";
        ASSERT_EQ(value::bitcastTo<int32_t>(v), 0) << "valueBlockAggBottomN";
    }

    {
        // While there is no guarantee of stable sorting, verify that duplicate [k, v] pairs are
        // preserved in the output.
        std::vector<TypedValues> addlInputKeys = {TypedValues{makeDecimal("10")},
                                                  TypedValues{makeDecimal("10")}};
        std::vector<TypedValues> addlOutVals = {TypedValues{makeDecimal("1000")},
                                                TypedValues{makeDecimal("1000")}};

        std::vector<TypedValues> newInputKeys = inputKeys;
        std::vector<TypedValues> newOutVals = outVals;
        std::vector<bool> newBitset = bitset;

        newInputKeys.push_back(addlInputKeys[0]);
        newOutVals.push_back(addlOutVals[0]);
        newBitset.push_back(true);

        newInputKeys.push_back(addlInputKeys[1]);
        newOutVals.push_back(addlOutVals[1]);
        newBitset.push_back(true);

        size_t maxSize = 2;
        auto [keyBlocks, valBlocks] = makeBlockTopBottomNInputs(newInputKeys, newOutVals);
        auto [topNFinal, bottomNFinal] = executeAndFinalizeTopBottomN(keyBlocks,
                                                                      valBlocks,
                                                                      makeEmptyState(maxSize),
                                                                      makeEmptyState(maxSize),
                                                                      newBitset,
                                                                      sortSpec);

        value::ValueGuard topNFinalGuard{topNFinal};
        value::ValueGuard bottomNFinalGuard{bottomNFinal};

        ASSERT_EQ(topNFinal.first, value::TypeTags::Array);
        auto* topNArr = value::getArrayView(topNFinal.second);
        for (auto [outTag, outVal] : topNArr->values()) {
            ASSERT_EQ(outTag, value::TypeTags::Array) << "valueBlockAggTopN";
            auto* outArr = value::getArrayView(outVal);
            ASSERT_EQ(outArr->size(), 2) << "valueBlockAggTopN";

            auto [outValTag, outValVal] = outArr->getAt(0);
            auto [t, v] = value::compareValue(
                addlOutVals[0][0].first, addlOutVals[0][0].second, outValTag, outValVal);
            ASSERT_EQ(t, value::TypeTags::NumberInt32) << "valueBlockAggTopN";
            ASSERT_EQ(value::bitcastTo<int32_t>(v), 0) << "valueBlockAggTopN";
        }

        TypedValues finalRes{topNFinal, bottomNFinal};
        std::vector<StringData> accType{"valueBlockAggTopN"_sd, "valueBlockAggBottomN"_sd};
        for (size_t i = 0; i < finalRes.size(); ++i) {
            verifyTopBottomNOutput(keyBlocks, finalRes[i], sortSpec, accType[i]);
        }

        release2dValueVector(std::move(addlInputKeys));
        release2dValueVector(std::move(addlOutVals));
    }

    {
        // Test with non-empty input state.

        // Field path "a"
        std::vector<TypedValues> addlInputKeys{{makeDecimal("7")},
                                               {makeDecimal("0")},
                                               {makeDecimal("4")},
                                               {makeDecimal("8")},
                                               {makeDecimal("3")}};

        std::vector<TypedValues> addlOutVals{{makeDecimal("150")},
                                             {makeDecimal("175")},
                                             {makeDecimal("200")},
                                             {makeDecimal("225")},
                                             {makeDecimal("250")}};

        std::vector<bool> newBitset = {true, true, true, false, true};


        {
            // Test non-empty state as input and maxSize = num of trues in input bitsets.
            runHandwrittenTest({inputKeys, addlInputKeys},
                               {outVals, addlOutVals},
                               bitset,
                               8 /* maxSize */,
                               2 /* numIters */);
        }

        {
            // Test non-empty state as input and maxSize = num of trues in a single block but <
            // total num trues.
            runHandwrittenTest({inputKeys, addlInputKeys},
                               {outVals, addlOutVals},
                               bitset,
                               6 /* maxSize */,
                               2 /* numIters */);
        }

        {
            // Test non-empty state as input and maxSize < num trues in a single block
            runHandwrittenTest({inputKeys, addlInputKeys},
                               {outVals, addlOutVals},
                               bitset,
                               3 /* maxSize */,
                               2 /* numIters */);
        }

        {
            // An exception should be throw when we exceed the state's memory limit. The first block
            // uses ~350 bytes of memory so we will hit the limit while processing the second block.
            ASSERT_THROWS_CODE(runHandwrittenTest({inputKeys, addlInputKeys},
                                                  {outVals, addlOutVals},
                                                  bitset,
                                                  8 /* maxSize */,
                                                  2 /* numIters */,
                                                  450 /* memLimit */),
                               DBException,
                               ErrorCodes::ExceededMemoryLimit);
        }

        release2dValueVector(std::move(addlInputKeys));
        release2dValueVector(std::move(addlOutVals));
    }

    release2dValueVector(std::move(inputKeys));
    release2dValueVector(std::move(outVals));
}

TEST_F(SBEBlockTopBottomTest, TopBottomNOracleTest) {
    auto input = makeInterestingValues();
    auto output = makeInterestingValues();
    ValueVectorGuard inputGuard{input};
    ValueVectorGuard outputGuard{output};

    std::vector<TypedValues> outVals;
    for (auto outVal : output) {
        outVals.push_back(TypedValues{value::copyValue(outVal.first, outVal.second)});
    }

    // bitset logic is tested by "handwritten" tests.
    std::vector<bool> bitset(input.size(), true);

    auto runOracleTest = [&, this](std::vector<TypedValues> inputKeys) {
        auto [keyBlocks, valBlocks] = makeBlockTopBottomNInputs(inputKeys, outVals);
        topBottomNOracleTest(keyBlocks, valBlocks, {bitset}, inputKeys.size());
    };

    {
        // All values are top level fields.

        std::vector<TypedValues> inputKeys;
        for (auto inVal : input) {
            inputKeys.push_back(TypedValues{value::copyValue(inVal.first, inVal.second)});
        }

        runOracleTest(inputKeys);

        release2dValueVector(inputKeys);
    }

    {
        // All values are in nested fields.

        std::vector<TypedValues> inputKeys;
        for (auto inVal : input) {
            UniqueBSONObjBuilder bob;
            bson::appendValueToBsonObj(bob, "b"_sd, inVal.first, inVal.second);
            bob.doneFast();
            inputKeys.push_back(
                TypedValues{std::pair{value::TypeTags::bsonObject,
                                      value::bitcastFrom<char*>(bob.bb().release().release())}});
        }

        runOracleTest(inputKeys);

        release2dValueVector(inputKeys);
    }

    {
        // All values are in top level arrays.

        std::vector<TypedValues> inputKeys;
        for (auto inVal : input) {
            UniqueBSONArrayBuilder bab;
            bson::appendValueToBsonArr(bab, inVal.first, inVal.second);
            bab.doneFast();
            inputKeys.push_back(
                TypedValues{std::pair{value::TypeTags::bsonArray,
                                      value::bitcastFrom<char*>(bab.bb().release().release())}});
        }

        runOracleTest(inputKeys);

        release2dValueVector(inputKeys);
    }

    release2dValueVector(outVals);
}
}  // namespace mongo::sbe
