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
#include "mongo/unittest/unittest.h"

namespace mongo::sbe {

class SBEBlockTopBottomTest : public EExpressionTestFixture {
public:
    // InBlockType and OutBlockType must implement push_back.
    template <typename InBlockType = value::HeterogeneousBlock,
              typename OutBlockType = value::HeterogeneousBlock>
    std::pair<std::vector<std::unique_ptr<value::ValueBlock>>,
              std::vector<std::unique_ptr<value::ValueBlock>>>
    makeBlockTopBottomNInputs(const std::vector<TypedValues>& inputKeys,
                              const std::vector<TypedValues>& outVals,
                              size_t startIdx = 0,
                              const TypedValues& inputKeyMins = {},
                              const TypedValues& inputKeyMaxs = {}) {
        invariant(inputKeys.size() == outVals.size());
        invariant((inputKeyMins.empty() && inputKeyMaxs.empty()) ||
                  (!inputKeys.empty() &&
                   ((inputKeyMins.size() == inputKeys[0].size() &&
                     inputKeyMaxs.size() == inputKeys[0].size()))));
        std::vector<std::unique_ptr<value::ValueBlock>> inputKeyBlocks;
        std::vector<std::unique_ptr<value::ValueBlock>> outValBlocks;
        if (inputKeys.size() == 0) {
            return {std::move(inputKeyBlocks), std::move(outValBlocks)};
        }
        for (size_t j = 0; j < inputKeys[0].size(); ++j) {
            inputKeyBlocks.push_back(std::make_unique<InBlockType>());
            for (size_t i = 0; i < inputKeys.size(); ++i) {
                invariant(inputKeys[i].size() == inputKeys[0].size());
                if constexpr (std::is_same_v<InBlockType, value::Int32Block>) {
                    inputKeyBlocks[j]->as<InBlockType>()->push_back(inputKeys[i][j].second);

                } else {
                    inputKeyBlocks[j]->as<InBlockType>()->push_back(
                        value::copyValue(inputKeys[i][j].first, inputKeys[i][j].second));
                }
            }
            if constexpr (std::is_same_v<InBlockType, TestBlock> ||
                          std::is_same_v<InBlockType, UnextractableTestBlock>) {
                if (!inputKeyMins.empty() && !inputKeyMaxs.empty()) {
                    inputKeyBlocks[j]->as<InBlockType>()->setMin(inputKeyMins[j].first,
                                                                 inputKeyMins[j].second);
                    inputKeyBlocks[j]->as<InBlockType>()->setMax(inputKeyMaxs[j].first,
                                                                 inputKeyMaxs[j].second);
                }
            }
        }

        for (size_t j = 0; j < outVals[0].size(); ++j) {
            outValBlocks.push_back(std::make_unique<OutBlockType>());
            for (size_t i = 0; i < outVals.size(); ++i) {
                invariant(outVals[i].size() == outVals[0].size());
                auto [outValArrTag, outValArrVal] = value::makeNewArray();
                auto* outValArr = value::getArrayView(outValArrVal);
                outValArr->push_back(value::copyValue(outVals[i][j].first, outVals[i][j].second));
                outValArr->push_back(value::TypeTags::NumberInt64,
                                     value::bitcastFrom<size_t>(i + startIdx));
                outValBlocks[j]->as<OutBlockType>()->push_back(outValArrTag, outValArrVal);
            }
        }

        return {std::move(inputKeyBlocks), std::move(outValBlocks)};
    }

    template <bool HomogeneousBitset = false>
    std::pair<TypedValue, TypedValue> executeBlockTopBottomN(
        const std::vector<std::unique_ptr<value::ValueBlock>>& inputKeyBlocks,
        const std::vector<std::unique_ptr<value::ValueBlock>>& outValBlocks,
        TypedValue topNState,
        const std::vector<bool>& bitset,
        SortSpec sortSpec,
        TypedValue bottomNState = {value::TypeTags::Nothing, value::Value{0u}}) {

        std::vector<value::ViewOfValueAccessor> keyBlockAccessors{inputKeyBlocks.size()};
        std::vector<value::ViewOfValueAccessor> valBlockAccessors{outValBlocks.size()};
        value::ViewOfValueAccessor bitsetAccessor;
        value::ViewOfValueAccessor sortSpecAccessor;
        value::OwnedValueAccessor topNAggAccessor;
        value::OwnedValueAccessor bottomNAggAccessor;

        std::vector<sbe::value::SlotId> keyBlockSlots(inputKeyBlocks.size());
        std::vector<sbe::value::SlotId> valBlockSlots(outValBlocks.size());

        for (size_t i = 0; i < inputKeyBlocks.size(); ++i) {
            keyBlockSlots[i] = bindAccessor(&keyBlockAccessors[i]);
        }
        for (size_t i = 0; i < outValBlocks.size(); ++i) {
            valBlockSlots[i] = bindAccessor(&valBlockAccessors[i]);
        }
        auto bitsetSlot = bindAccessor(&bitsetAccessor);
        auto sortSpecSlot = bindAccessor(&sortSpecAccessor);

        auto getTopBottomNExpr = [&](bool isTopN) {
            auto args = sbe::makeEs(makeE<EVariable>(bitsetSlot), makeE<EVariable>(sortSpecSlot));
            if (inputKeyBlocks.size() > 1) {
                args.emplace_back(makeE<EConstant>(
                    value::TypeTags::NumberInt32,
                    value::bitcastFrom<int32_t>(static_cast<int32_t>(inputKeyBlocks.size()))));
            } else {
                args.emplace_back(makeE<EConstant>(value::TypeTags::Null, 0));
            }

            for (size_t i = 0; i < inputKeyBlocks.size(); ++i) {
                args.emplace_back(makeE<EVariable>(keyBlockSlots[i]));
            }
            for (size_t i = 0; i < outValBlocks.size(); ++i) {
                args.emplace_back(makeE<EVariable>(valBlockSlots[i]));
            }
            auto accTopN = outValBlocks.size() > 1 ? "valueBlockAggTopNArray" : "valueBlockAggTopN";
            auto accBottomN =
                outValBlocks.size() > 1 ? "valueBlockAggBottomNArray" : "valueBlockAggBottomN";
            return sbe::makeE<sbe::EFunction>(isTopN ? accTopN : accBottomN, std::move(args));
        };

        auto topNExpr = getTopBottomNExpr(true);
        auto compiledTopNExpr = compileAggExpression(*topNExpr, &topNAggAccessor);

        for (size_t i = 0; i < inputKeyBlocks.size(); ++i) {
            keyBlockAccessors[i].reset(
                sbe::value::TypeTags::valueBlock,
                value::bitcastFrom<value::ValueBlock*>(inputKeyBlocks[i].get()));
        }
        for (size_t i = 0; i < outValBlocks.size(); ++i) {
            valBlockAccessors[i].reset(
                sbe::value::TypeTags::valueBlock,
                value::bitcastFrom<value::ValueBlock*>(outValBlocks[i].get()));
        }

        std::unique_ptr<value::ValueBlock> bitsetBlock;
        if constexpr (HomogeneousBitset) {
            bitsetBlock = makeBoolBlock(bitset);
        } else {
            bitsetBlock = makeHeterogeneousBoolBlock(bitset);
        }
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

        auto bottomNRes = bottomNState;
        if (bottomNState.first != value::TypeTags::Nothing) {
            auto bottomNExpr = getTopBottomNExpr(false);
            auto compiledBottomNExpr = compileAggExpression(*bottomNExpr, &bottomNAggAccessor);
            bottomNRes = runCompiledExpression(compiledBottomNExpr.get());
        }

        return {topNRes, bottomNRes};
    }

    std::pair<TypedValue, TypedValue> finalizeTopBottomN(TypedValue topNRes,
                                                         SortSpec sortSpec,
                                                         TypedValue bottomNRes = {
                                                             value::TypeTags::Nothing, 0u}) {
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

        auto bottomNFinal = bottomNRes;
        if (bottomNRes.first != value::TypeTags::Nothing) {
            stateAccessor.reset(bottomNRes.first, bottomNRes.second);
            bottomNFinal = runCompiledExpression(compiledExpr.get());
            ASSERT_EQ(bottomNFinal.first, value::TypeTags::Array);
        }

        return {topNFinal, bottomNFinal};
    }

    template <bool HomogeneousBitset = false>
    std::pair<TypedValue, TypedValue> executeAndFinalizeTopBottomN(
        const std::vector<std::unique_ptr<value::ValueBlock>>& keyBlocks,
        const std::vector<std::unique_ptr<value::ValueBlock>>& valBlocks,
        TypedValue topNState,
        TypedValue bottomNState,
        const std::vector<bool>& bitset,
        SortSpec sortSpec) {
        auto [newTopNState, newBottomNState] = executeBlockTopBottomN<HomogeneousBitset>(
            keyBlocks, valBlocks, topNState, bitset, sortSpec, bottomNState);

        value::ValueGuard topNResGuard{newTopNState};
        value::ValueGuard bottomNResGuard{newBottomNState};

        auto [topNFinal, bottomNFinal] =
            finalizeTopBottomN(newTopNState, sortSpec, newBottomNState);

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
            invariant(blocks[i]->count() == blocks[0]->count());
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
                                const std::vector<bool>& bitset,
                                StringData builtinName,
                                size_t outArraySize = 1) {
        auto inputKeys = blocksTo2dVector(keyBlocks);
        ASSERT_EQ(finalRes.first, value::TypeTags::Array) << builtinName;
        auto* finalArr = value::getArrayView(finalRes.second);

        auto numKeys = keyBlocks.size();

        std::vector<TypedValues> sortKeyList;
        stdx::unordered_set<size_t> seenIdxs;
        for (auto [outTag, outVal] : finalArr->values()) {
            ASSERT_EQ(outTag, value::TypeTags::Array) << builtinName;
            auto* outArr = value::getArrayView(outVal);

            if (outArraySize > 1) {
                ASSERT_EQ(outArr->size(), outArraySize) << builtinName;
                auto [t, v] = outArr->getAt(0);
                ASSERT_EQ(t, value::TypeTags::Array) << builtinName;
                outArr = value::getArrayView(v);
            }
            ASSERT_EQ(outArr->size(), 2) << builtinName;

            auto [outIdxTag, outIdxVal] = outArr->getAt(1);
            ASSERT_EQ(outIdxTag, value::TypeTags::NumberInt64) << builtinName;
            size_t outIdx = value::bitcastTo<size_t>(outIdxVal);

            auto [_, newIdx] = seenIdxs.insert(outIdx);
            invariant(newIdx);

            auto sortKeys = inputKeys[outIdx];
            if (!sortKeyList.empty()) {
                if (numKeys > 1) {
                    auto sortPattern = sortSpec.getSortPattern();
                    for (size_t i = 0; i < numKeys; i++) {
                        auto [t, v] = value::compareValue(sortKeyList.back()[i].first,
                                                          sortKeyList.back()[i].second,
                                                          sortKeys[i].first,
                                                          sortKeys[i].second);
                        ASSERT_EQ(t, value::TypeTags::NumberInt32) << builtinName;
                        auto cmp =
                            value::bitcastTo<int32_t>(v) * (sortPattern[i].isAscending ? 1 : -1);
                        ASSERT_LTE(cmp, 0) << builtinName;
                        if (cmp < 0) {
                            break;
                        }
                    }
                } else {
                    auto [t, v] = sortSpec.compare(sortKeyList.back()[0].first,
                                                   sortKeyList.back()[0].second,
                                                   sortKeys[0].first,
                                                   sortKeys[0].second);
                    ASSERT_EQ(t, value::TypeTags::NumberInt32) << builtinName;
                    ASSERT_LTE(value::bitcastTo<int32_t>(v), 0) << builtinName;
                }
            }
            sortKeyList.push_back(sortKeys);
        }

        if (sortKeyList.empty()) {
            return;
        }

        for (size_t i = 0; i < inputKeys.size(); ++i) {
            // Don't compare against sort keys that are in the final output that we already verified
            // compare >= to sortKeys.back().
            if (seenIdxs.contains(i) || !bitset[i % bitset.size()]) {
                continue;
            }

            auto sortKeys = inputKeys[i];
            auto [borderKey, sign] = [&]() {
                if (builtinName == "valueBlockAggTopN" || builtinName == "valueBlockAggTopNArray") {
                    return std::pair{sortKeyList.back(), 1};
                } else {
                    return std::pair{sortKeyList.front(), -1};
                }
            }();
            if (numKeys > 1) {
                auto sortPattern = sortSpec.getSortPattern();
                for (size_t i = 0; i < numKeys; i++) {
                    auto [t, v] = value::compareValue(borderKey[i].first,
                                                      borderKey[i].second,
                                                      sortKeys[i].first,
                                                      sortKeys[i].second);
                    ASSERT_EQ(t, value::TypeTags::NumberInt32) << builtinName;
                    auto cmp =
                        value::bitcastTo<int32_t>(v) * (sortPattern[i].isAscending ? 1 : -1) * sign;
                    ASSERT_LTE(cmp, 0) << builtinName;
                    if (cmp < 0) {
                        break;
                    }
                }
            } else {
                auto [t, v] = sortSpec.compare(
                    borderKey[0].first, borderKey[0].second, sortKeys[0].first, sortKeys[0].second);
                ASSERT_EQ(t, value::TypeTags::NumberInt32) << builtinName;
                ASSERT_LTE(value::bitcastTo<int32_t>(v) * sign, 0) << builtinName;
            }
        }
    }

    void topBottomNOracleTest(const std::vector<std::unique_ptr<value::ValueBlock>>& keyBlocks,
                              const std::vector<std::unique_ptr<value::ValueBlock>>& valBlocks,
                              const std::vector<std::vector<bool>>& bitsets,
                              size_t maxSizeMax) {
        for (const auto& bitset : bitsets) {
            std::vector<int> sortDirections{-1 /* descending */, 1 /* ascending */};
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
                        verifyTopBottomNOutput(
                            keyBlocks, finalRes[i], sortSpec, bitset, accType[i]);
                    }
                }
            }
        }
    }

    template <typename InBlockType = value::HeterogeneousBlock,
              typename OutBlockType = value::HeterogeneousBlock,
              bool HomogeneousBitset = false>
    void runHandwrittenTest(std::vector<std::vector<TypedValues>> inputKeysVec,
                            std::vector<std::vector<TypedValues>> outValsVec,
                            std::vector<bool> bitset,
                            SortSpec sortSpec,
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
            auto [keyBlocks, valBlocks] = makeBlockTopBottomNInputs<InBlockType, OutBlockType>(
                inputKeysVec[iter], outValsVec[iter], startIdx);
            startIdx += inputKeysVec[iter].size();

            std::tie(topNState, bottomNState) = executeBlockTopBottomN<HomogeneousBitset>(
                keyBlocks, valBlocks, topNState, bitset, sortSpec, bottomNState);

            // Add to the keys we have encountered so far.
            addToCombinedBlocks(combinedKeyBlocks, inputKeysVec[iter]);

            // Verify that intermediate results are still correct.
            auto [topNInter, bottomNInter] = finalizeTopBottomN(topNState, sortSpec, bottomNState);
            value::ValueGuard topNInterGuard{topNInter};
            value::ValueGuard bottomNInterGuard{bottomNInter};

            verifyTopBottomNOutput(combinedKeyBlocks,
                                   topNInter,
                                   sortSpec,
                                   bitset,
                                   "valueBlockAggTopN",
                                   outValsVec[iter][0].size());
            verifyTopBottomNOutput(combinedKeyBlocks,
                                   bottomNInter,
                                   sortSpec,
                                   bitset,
                                   "valueBlockAggBottomN",
                                   outValsVec[iter][0].size());
        }

        auto [keyBlocks, valBlocks] = makeBlockTopBottomNInputs<InBlockType, OutBlockType>(
            inputKeysVec.back(), outValsVec.back(), startIdx);

        auto [topNFinal, bottomNFinal] = executeAndFinalizeTopBottomN<HomogeneousBitset>(
            keyBlocks, valBlocks, topNState, bottomNState, bitset, sortSpec);

        value::ValueGuard topNFinalGuard{topNFinal};
        value::ValueGuard bottomNFinalGuard{bottomNFinal};

        // Add to the keys we have encountered so far.
        addToCombinedBlocks(combinedKeyBlocks, inputKeysVec.back());

        TypedValues finalRes{topNFinal, bottomNFinal};
        std::vector<StringData> accType{"valueBlockAggTopN"_sd, "valueBlockAggBottomN"_sd};
        for (size_t i = 0; i < finalRes.size(); ++i) {
            verifyTopBottomNOutput(combinedKeyBlocks,
                                   finalRes[i],
                                   sortSpec,
                                   bitset,
                                   accType[i],
                                   outValsVec.back()[0].size());
        }
    }

    void testTopBottomN(std::vector<TypedValues>& inputKeys,
                        std::vector<TypedValues>& outVals,
                        std::vector<TypedValues>& moreInputKeys,
                        std::vector<TypedValues>& moreOutVals,
                        std::vector<TypedValues>& addlInputKeys,
                        std::vector<TypedValues>& addlOutVals,
                        SortSpec sortSpec,
                        std::vector<bool>& bitset) {
        {
            // Input bitset is all false
            std::vector<bool> falseBitset(inputKeys.size(), false);

            runHandwrittenTest({inputKeys}, {outVals}, falseBitset, sortSpec, 3 /* maxSize */);
        }

        {
            // Input bitset is all true
            std::vector<bool> trueBitset(inputKeys.size(), true);

            runHandwrittenTest({inputKeys}, {outVals}, trueBitset, sortSpec, 3 /* maxSize */);
        }

        {
            // Input bitset has trues and falses
            runHandwrittenTest({inputKeys}, {outVals}, bitset, sortSpec, 3 /* maxSize */);
        }

        {
            // An exception should be throw when we exceed the state's memory limit.
            ASSERT_THROWS_CODE(runHandwrittenTest({inputKeys},
                                                  {outVals},
                                                  bitset,
                                                  sortSpec,
                                                  3 /* maxSize */,
                                                  1 /* numIters */,
                                                  64 /* memLimit */),
                               DBException,
                               ErrorCodes::ExceededMemoryLimit);
        }

        {
            // maxSize >= # of trues always returns the same results.
            size_t numTrues = 4;

            auto [keyBlocks, valBlocks] = makeBlockTopBottomNInputs<>(inputKeys, outVals);
            auto [topNFinal1, bottomNFinal1] =
                executeAndFinalizeTopBottomN(keyBlocks,
                                             valBlocks,
                                             makeEmptyState(numTrues),
                                             makeEmptyState(numTrues),
                                             bitset,
                                             sortSpec);

            value::ValueGuard topNFinalGuard1{topNFinal1};
            value::ValueGuard bottomNFinalGuard1{bottomNFinal1};

            std::tie(keyBlocks, valBlocks) = makeBlockTopBottomNInputs<>(inputKeys, outVals);
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
            std::tie(t, v) = value::compareValue(bottomNFinal1.first,
                                                 bottomNFinal1.second,
                                                 bottomNFinal2.first,
                                                 bottomNFinal2.second);
            ASSERT_EQ(t, value::TypeTags::NumberInt32) << "valueBlockAggBottomN";
            ASSERT_EQ(value::bitcastTo<int32_t>(v), 0) << "valueBlockAggBottomN";
        }

        {
            // While there is no guarantee of stable sorting, verify that duplicate [k, v] pairs are
            // preserved in the output.
            std::vector<TypedValues> newInputKeys = inputKeys;
            std::vector<TypedValues> newOutVals = outVals;
            std::vector<bool> newBitset = bitset;

            newInputKeys.push_back(moreInputKeys[0]);
            newOutVals.push_back(moreOutVals[0]);
            newBitset.push_back(true);

            newInputKeys.push_back(moreInputKeys[1]);
            newOutVals.push_back(moreOutVals[1]);
            newBitset.push_back(true);

            size_t maxSize = 2;
            auto [keyBlocks, valBlocks] = makeBlockTopBottomNInputs<>(newInputKeys, newOutVals);
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

                value::Array* innerArr = outArr;
                if (newOutVals[0].size() > 1) {
                    auto [innerArrTag, innerArrVal] = outArr->getAt(0);

                    ASSERT_EQ(innerArrTag, value::TypeTags::Array) << "valueBlockAggTopN";
                    innerArr = value::getArrayView(innerArrVal);
                    ASSERT_EQ(innerArr->size(), 2) << "valueBlockAggTopN";
                }

                auto [outValTag, outValVal] = innerArr->getAt(0);

                auto [t, v] = value::compareValue(
                    moreOutVals[0][0].first, moreOutVals[0][0].second, outValTag, outValVal);
                ASSERT_EQ(t, value::TypeTags::NumberInt32) << "valueBlockAggTopN";
                ASSERT_EQ(value::bitcastTo<int32_t>(v), 0) << "valueBlockAggTopN";
            }

            TypedValues finalRes{topNFinal, bottomNFinal};
            std::vector<StringData> accType{"valueBlockAggTopN"_sd, "valueBlockAggBottomN"_sd};
            for (size_t i = 0; i < finalRes.size(); ++i) {
                verifyTopBottomNOutput(
                    keyBlocks, finalRes[i], sortSpec, bitset, accType[i], newOutVals[0].size());
            }
        }

        {
            // Test with non-empty input state.
            {
                // Test non-empty state as input and maxSize = num of trues in input bitsets.
                runHandwrittenTest({inputKeys, addlInputKeys},
                                   {outVals, addlOutVals},
                                   bitset,
                                   sortSpec,
                                   8 /* maxSize */,
                                   2 /* numIters */);
            }

            {
                // Test non-empty state as input and maxSize = num of trues in a single block but <
                // total num trues.
                runHandwrittenTest({inputKeys, addlInputKeys},
                                   {outVals, addlOutVals},
                                   bitset,
                                   sortSpec,
                                   6 /* maxSize */,
                                   2 /* numIters */);
            }

            {
                // Test non-empty state as input and maxSize < num trues in a single block
                runHandwrittenTest({inputKeys, addlInputKeys},
                                   {outVals, addlOutVals},
                                   bitset,
                                   sortSpec,
                                   3 /* maxSize */,
                                   2 /* numIters */);
            }

            {
                // An exception should be throw when we exceed the state's memory limit. The first
                // block uses ~350 bytes of memory so we will hit the limit while processing the
                // second block.
                ASSERT_THROWS_CODE(runHandwrittenTest({inputKeys, addlInputKeys},
                                                      {outVals, addlOutVals},
                                                      bitset,
                                                      sortSpec,
                                                      8 /* maxSize */,
                                                      2 /* numIters */,
                                                      450 /* memLimit */),
                                   DBException,
                                   ErrorCodes::ExceededMemoryLimit);
            }
        }

        release2dValueVector(std::move(inputKeys));
        release2dValueVector(std::move(outVals));

        release2dValueVector(std::move(moreInputKeys));
        release2dValueVector(std::move(moreOutVals));

        release2dValueVector(std::move(addlInputKeys));
        release2dValueVector(std::move(addlOutVals));
    }

    void addToCombinedBlocks(std::vector<std::unique_ptr<value::ValueBlock>>& combinedKeyBlocks,
                             const std::vector<TypedValues>& inputKeys) {
        for (size_t j = 0; j < inputKeys[0].size(); ++j) {
            for (size_t i = 0; i < inputKeys.size(); ++i) {
                invariant(inputKeys[i].size() == inputKeys[0].size());
                combinedKeyBlocks[j]->as<value::HeterogeneousBlock>()->push_back(
                    value::copyValue(inputKeys[i][j].first, inputKeys[i][j].second));
            }
        }
    }
};

TEST_F(SBEBlockTopBottomTest, TopBottomNSingleKeySingleOutputTest) {
    // Tests with Decimal128s to test memory management while still being easy to reason about.
    SortSpec sortSpec{BSON("a" << -1)};

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


    std::vector<TypedValues> moreInputKeys{TypedValues{makeDecimal("10")},
                                           TypedValues{makeDecimal("10")}};
    std::vector<TypedValues> moreOutVals{TypedValues{makeDecimal("1000")},
                                         TypedValues{makeDecimal("1000")}};

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

    std::vector<bool> bitset{false, true, true, true, true};

    testTopBottomN(inputKeys,
                   outVals,
                   moreInputKeys,
                   moreOutVals,
                   addlInputKeys,
                   addlOutVals,
                   sortSpec,
                   bitset);
}

TEST_F(SBEBlockTopBottomTest, TopBottomNMultipleKeySingleOutputTest) {

    SortSpec sortSpec{BSON("a" << -1 << "b" << -1)};

    std::vector<TypedValues> inputKeys{{makeDecimal("2"), makeInt32(5)},
                                       {makeDecimal("2"), makeInt32(1)},
                                       {makeNull(), makeInt32(3)},
                                       {makeDecimal("1"), makeInt32(2)},
                                       {makeDecimal("1"), makeInt32(4)}};

    std::vector<TypedValues> outVals{{makeDecimal("25")},
                                     {makeDecimal("50")},
                                     {makeDecimal("75")},
                                     {makeDecimal("100")},
                                     {makeDecimal("125")}};


    std::vector<TypedValues> moreInputKeys{{makeDecimal("10"), makeInt32(10)},
                                           {makeDecimal("10"), makeInt32(10)}};
    std::vector<TypedValues> moreOutVals{{makeDecimal("1000")}, {makeDecimal("1000")}};


    std::vector<TypedValues> addlInputKeys{{makeDecimal("8"), makeInt32(7)},
                                           {makeDecimal("0"), makeInt32(0)},
                                           {makeDecimal("8"), makeInt32(4)},
                                           {makeDecimal("8"), makeInt32(8)},
                                           {makeDecimal("0"), makeInt32(3)}};

    std::vector<TypedValues> addlOutVals{{makeDecimal("150")},
                                         {makeDecimal("175")},
                                         {makeDecimal("200")},
                                         {makeDecimal("225")},
                                         {makeDecimal("250")}};

    std::vector<bool> bitset{false, true, true, true, true};

    testTopBottomN(inputKeys,
                   outVals,
                   moreInputKeys,
                   moreOutVals,
                   addlInputKeys,
                   addlOutVals,
                   sortSpec,
                   bitset);
}

TEST_F(SBEBlockTopBottomTest, TopBottomNSingleKeyArrayOutputTest) {

    SortSpec sortSpec{BSON("a" << -1)};

    std::vector<TypedValues> inputKeys{{makeDecimal("6")},
                                       {makeDecimal("2")},
                                       {makeNull()},
                                       {makeDecimal("1")},
                                       {makeDecimal("5")}};

    std::vector<TypedValues> outVals{{makeDecimal("25"), makeInt32(10)},
                                     {makeDecimal("50"), makeInt32(20)},
                                     {makeDecimal("75"), makeInt32(30)},
                                     {makeDecimal("100"), makeInt32(40)},
                                     {makeDecimal("125"), makeInt32(50)}};


    std::vector<TypedValues> moreInputKeys{{makeDecimal("10")}, {makeDecimal("10")}};
    std::vector<TypedValues> moreOutVals{{makeDecimal("1000"), makeInt32(1000)},
                                         {makeDecimal("1000"), makeInt32(1000)}};


    std::vector<TypedValues> addlInputKeys{{makeDecimal("7")},
                                           {makeDecimal("0")},
                                           {makeDecimal("4")},
                                           {makeDecimal("8")},
                                           {makeDecimal("3")}};

    std::vector<TypedValues> addlOutVals{{makeDecimal("150"), makeInt32(150)},
                                         {makeDecimal("175"), makeInt32(175)},
                                         {makeDecimal("200"), makeInt32(200)},
                                         {makeDecimal("225"), makeInt32(225)},
                                         {makeDecimal("250"), makeInt32(250)}};

    std::vector<bool> bitset{false, true, true, true, true};

    testTopBottomN(inputKeys,
                   outVals,
                   moreInputKeys,
                   moreOutVals,
                   addlInputKeys,
                   addlOutVals,
                   sortSpec,
                   bitset);
}

TEST_F(SBEBlockTopBottomTest, TopBottomNMultipleKeyArrayOutputTest) {

    SortSpec sortSpec{BSON("a" << -1 << "b" << -1)};

    std::vector<TypedValues> inputKeys{{makeDecimal("6"), makeInt32(5)},
                                       {makeDecimal("1"), makeInt32(1)},
                                       {makeNull(), makeInt32(3)},
                                       {makeDecimal("1"), makeInt32(2)},
                                       {makeDecimal("6"), makeInt32(4)}};

    std::vector<TypedValues> outVals{{makeDecimal("25"), makeInt32(10)},
                                     {makeDecimal("50"), makeInt32(20)},
                                     {makeDecimal("75"), makeInt32(30)},
                                     {makeDecimal("100"), makeInt32(40)},
                                     {makeDecimal("125"), makeInt32(50)}};


    std::vector<TypedValues> moreInputKeys{{makeDecimal("10"), makeInt32(10)},
                                           {makeDecimal("10"), makeInt32(10)}};
    std::vector<TypedValues> moreOutVals{{makeDecimal("1000"), makeInt32(1000)},
                                         {makeDecimal("1000"), makeInt32(1000)}};


    std::vector<TypedValues> addlInputKeys{{makeDecimal("7"), makeInt32(7)},
                                           {makeDecimal("0"), makeInt32(0)},
                                           {makeDecimal("0"), makeInt32(4)},
                                           {makeDecimal("0"), makeInt32(8)},
                                           {makeDecimal("7"), makeInt32(3)}};

    std::vector<TypedValues> addlOutVals{{makeDecimal("150"), makeInt32(150)},
                                         {makeDecimal("175"), makeInt32(175)},
                                         {makeDecimal("200"), makeInt32(200)},
                                         {makeDecimal("225"), makeInt32(225)},
                                         {makeDecimal("250"), makeInt32(250)}};

    std::vector<bool> bitset{false, true, true, true, true};

    testTopBottomN(inputKeys,
                   outVals,
                   moreInputKeys,
                   moreOutVals,
                   addlInputKeys,
                   addlOutVals,
                   sortSpec,
                   bitset);
}

TEST_F(SBEBlockTopBottomTest, TestArgMinMaxFastPath) {
    // Test the argMinMax fast path with more than one iteration

    SortSpec sortSpec{BSON("a" << -1)};

    std::vector<TypedValues> inputKeysFirstBlock{
        {makeInt32(6)}, {makeInt32(2)}, {makeInt32(4)}, {makeInt32(1)}, {makeInt32(5)}};

    std::vector<TypedValues> outValsFirstBlock{
        {makeInt32(25)}, {makeInt32(50)}, {makeInt32(75)}, {makeInt32(100)}, {makeInt32(125)}};

    std::vector<TypedValues> inputKeysSecondBlock{
        {makeInt32(0)}, {makeInt32(10)}, {makeInt32(3)}, {makeInt32(7)}, {makeInt32(5)}};

    std::vector<TypedValues> outValsSecondBlock{
        {makeInt32(55)}, {makeInt32(60)}, {makeInt32(70)}, {makeInt32(90)}, {makeInt32(25)}};

    std::vector<bool> allTrueBitset{true, true, true, true, true};

    runHandwrittenTest<value::Int32Block, value::HeterogeneousBlock, true>(
        {inputKeysFirstBlock, inputKeysSecondBlock},
        {outValsFirstBlock, outValsSecondBlock},
        allTrueBitset,
        sortSpec,
        1 /*maxSize*/,
        2 /*numIters*/);
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

TEST_F(SBEBlockTopBottomTest, TopBottomNHomogeneousTest) {
    // Test that any homogeneous fast paths don't affect correctness.

    auto makeDecimalBlock = []<typename BlockType = TestBlock>(size_t count) {
        auto decimalBlock = std::make_unique<BlockType>();
        for (size_t i = 0; i < count; ++i) {
            auto [outValArrTag, outValArrVal] = value::makeNewArray();
            auto* outValArr = value::getArrayView(outValArrVal);
            outValArr->push_back(value::makeCopyDecimal(Decimal128(i)));
            outValArr->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<size_t>(i));
            decimalBlock->push_back(outValArrTag, outValArrVal);
        }
        return decimalBlock;
    };

    auto runTest = [this]<bool HomogeneousBitset = false>(
                       std::vector<std::unique_ptr<value::ValueBlock>>& keyBlocks,
                       std::vector<std::unique_ptr<value::ValueBlock>>& valBlocks,
                       std::vector<bool> bitset) {
        std::vector<int> sortDirections{-1 /* descending */, 1 /* ascending */};
        for (int32_t sd : sortDirections) {
            SortSpec sortSpec{BSON("sortField" << sd)};

            size_t maxSize = 1;
            auto [topNFinal, bottomNFinal] =
                executeAndFinalizeTopBottomN<HomogeneousBitset>(keyBlocks,
                                                                valBlocks,
                                                                makeEmptyState(maxSize),
                                                                makeEmptyState(maxSize),
                                                                bitset,
                                                                sortSpec);

            value::ValueGuard topNGuard{topNFinal};
            value::ValueGuard bottomNGuard{bottomNFinal};

            verifyTopBottomNOutput(keyBlocks, topNFinal, sortSpec, bitset, "valueBlockAggTopN");
            verifyTopBottomNOutput(
                keyBlocks, bottomNFinal, sortSpec, bitset, "valueBlockAggBottomN");
        }
    };

    auto runArgMinMaxTest = [&, this]<typename BlockType, typename T>() {
        std::vector<std::unique_ptr<value::ValueBlock>> keyBlocks;
        keyBlocks.push_back(makeTestHomogeneousBlock<BlockType, T>(false /* inclNothing */,
                                                                   false /* multipleNaNs */));
        std::vector<std::unique_ptr<value::ValueBlock>> valBlocks;
        size_t count = keyBlocks[0]->count();
        // The argMin/Max should use `at()` on the output block instead of calling extract().
        valBlocks.push_back(makeDecimalBlock.template operator()<UnextractableTestBlock>(count));
        std::vector<bool> trueBitset(count, true);

        runTest.template operator()<true /* HomogeneousBitset */>(keyBlocks, valBlocks, trueBitset);
    };

    auto buildBitsets = [](size_t count) {
        // Can only store up to 64 bits in one c++ variable.
        invariant(count < 64);
        std::vector<std::vector<bool>> bitsets;
        for (size_t i = 0; i < pow(2, count); ++i) {
            // Represent the bits of i as a bitset of size count.
            boost::dynamic_bitset<size_t> temp(count /* numBits */, i /* value */);
            std::vector<bool> bitset(count);
            for (size_t j = 0; j < temp.size(); ++j) {
                bitset[j] = temp[j];
            }
            bitsets.push_back(bitset);
        }

        return bitsets;
    };

    auto runHomogeneousOracleTest = [&, this]<typename BlockType, typename T>() {
        std::vector<std::unique_ptr<value::ValueBlock>> keyBlocks;
        keyBlocks.push_back(makeTestHomogeneousBlock<BlockType, T>(false /* inclNothing */,
                                                                   false /* multipleNaNs */));
        size_t count = keyBlocks[0]->count();
        std::vector<std::unique_ptr<value::ValueBlock>> valBlocks;
        valBlocks.push_back(makeDecimalBlock(count));

        topBottomNOracleTest(keyBlocks, valBlocks, buildBitsets(count), 1 /* maxSizeMax */);
    };

    {
        // Int32Block test: {-1, 0, 1, min(int32_t), max(int32_t)}
        {
            std::vector<std::unique_ptr<value::ValueBlock>> keyBlocks;
            keyBlocks.push_back(
                makeTestHomogeneousBlock<value::Int32Block, int32_t>(false /* inclNothing */));
            std::vector<std::unique_ptr<value::ValueBlock>> valBlocks;
            size_t count = keyBlocks[0]->count();
            valBlocks.push_back(makeDecimalBlock(count));
            std::vector<bool> trueBitset(count, true);

            runTest(keyBlocks, valBlocks, trueBitset);

            {
                // We shouldn't extract the output block if the bitset is all false.
                std::vector<std::unique_ptr<value::ValueBlock>> tempValBlocks;
                tempValBlocks.push_back(
                    makeDecimalBlock.template operator()<UnextractableTestBlock>(count));
                std::vector<bool> falseBitset(trueBitset.size(), false);

                runTest(keyBlocks, tempValBlocks, falseBitset);
            }

            std::vector<bool> mixedBitset(trueBitset.size(), true);
            // Set bits for min, max to false.
            mixedBitset[trueBitset.size() - 1] = false;
            mixedBitset[trueBitset.size() - 2] = false;

            runTest(keyBlocks, valBlocks, mixedBitset);
        }

        runHomogeneousOracleTest.template operator()<value::Int32Block, int32_t>();

        runArgMinMaxTest.template operator()<value::Int32Block, int32_t>();
    }

    {
        // Int64Block test: {-1, 0, 1, min(int64_t), max(int64_t)}
        runHomogeneousOracleTest.template operator()<value::Int64Block, int64_t>();

        runArgMinMaxTest.template operator()<value::Int64Block, int64_t>();
    }

    {
        // DateBlock test: {-1, 0, 1, min(int64_t), max(int64_t)}
        runHomogeneousOracleTest.template operator()<value::DateBlock, int64_t>();

        runArgMinMaxTest.template operator()<value::DateBlock, int64_t>();
    }

    {
        // DoubleBlock test:
        //   {-1, 0, 1, lowest(double), max(double), quiet_NaN, -inf, +inf}
        // Since NaN == NaN in MQL semantics, we will only test with one NaN in the block.
        runHomogeneousOracleTest.template operator()<value::DoubleBlock, double>();

        runArgMinMaxTest.template operator()<value::DoubleBlock, double>();
    }

    {
        // BoolBlock test: {false, true}
        // BoolBlocks shouldn't use the fast path but verify that the output is still correct.
        runHomogeneousOracleTest.template operator()<value::BoolBlock, bool>();

        // BoolBlocks should use the argMin/Max fast path.
        runArgMinMaxTest.template operator()<value::BoolBlock, bool>();
    }
}

TEST_F(SBEBlockTopBottomTest, TopBottomNLazyExtractionTest) {
    // Field path "a"
    std::vector<TypedValues> inputKeysLow{
        {makeDecimal("1")}, {makeDecimal("2")}, {makeDecimal("3")}, {makeDecimal("4")}};

    std::vector<TypedValues> inputKeysHigh{
        {makeDecimal("5")}, {makeDecimal("6")}, {makeDecimal("7")}, {makeDecimal("8")}};

    std::vector<TypedValues> outVals1{
        {makeDecimal("1")}, {makeDecimal("2")}, {makeDecimal("3")}, {makeDecimal("4")}};

    std::vector<TypedValues> outVals2{
        {makeDecimal("5")}, {makeDecimal("6")}, {makeDecimal("7")}, {makeDecimal("8")}};

    std::vector<bool> bitset(4, true);

    auto lowMin = makeDecimal("1");
    value::ValueGuard lowMinGuard{lowMin};
    auto lowMax = makeDecimal("4");
    value::ValueGuard lowMaxGuard{lowMax};

    auto highMin = makeDecimal("5");
    value::ValueGuard highMinGuard{highMin};
    auto highMax = makeDecimal("8");
    value::ValueGuard highMaxGuard{highMax};

    auto runExtractionTest = [this]<typename InBlockType>(
                                 std::vector<std::vector<TypedValues>> inputKeysVec,
                                 std::vector<std::vector<TypedValues>> outValsVec,
                                 std::vector<bool> bitset,
                                 size_t maxSize,
                                 bool isAscending,
                                 size_t numIters = 1,
                                 std::vector<TypedValues> inputKeyMins = {},
                                 std::vector<TypedValues> inputKeyMaxs = {}) {
        invariant(inputKeysVec.size() == numIters && outValsVec.size() == numIters);
        invariant((inputKeyMins.empty() && inputKeyMaxs.empty()) ||
                  (inputKeyMins.size() == numIters && inputKeyMaxs.size() == numIters));

        SortSpec sortSpec{BSON("sortField" << (isAscending ? 1 : -1))};

        auto topNState = makeEmptyState(maxSize);
        std::pair<value::TypeTags, value::Value> bottomNState{value::TypeTags::Nothing, 0u};
        size_t startIdx = 0;

        // Initialize the combined blocks.
        std::vector<std::unique_ptr<value::ValueBlock>> combinedKeyBlocks;
        for (size_t j = 0;
             !inputKeysVec.empty() && !inputKeysVec[0].empty() && j < inputKeysVec[0][0].size();
             ++j) {
            combinedKeyBlocks.push_back(std::make_unique<value::HeterogeneousBlock>());
        }

        for (size_t iter = 0; iter < numIters - 1; ++iter) {
            // Only the last blocks can be unextractable or we will not be able to populate an
            // intermediate heap.
            auto [keyBlocks, valBlocks] = makeBlockTopBottomNInputs<TestBlock, TestBlock>(
                inputKeysVec[iter],
                outValsVec[iter],
                startIdx,
                inputKeyMins.empty() ? TypedValues{} : inputKeyMins[iter],
                inputKeyMaxs.empty() ? TypedValues{} : inputKeyMaxs[iter]);
            startIdx += inputKeysVec[iter].size();

            std::tie(topNState, bottomNState) =
                executeBlockTopBottomN(keyBlocks, valBlocks, topNState, bitset, sortSpec);

            // Add to the keys we have encountered so far.
            addToCombinedBlocks(combinedKeyBlocks, inputKeysVec[iter]);

            // Verify that intermediate results are still correct.
            auto [topNInter, _] = finalizeTopBottomN(topNState, sortSpec);
            value::ValueGuard topNInterGuard{topNInter};

            verifyTopBottomNOutput(
                combinedKeyBlocks, topNInter, sortSpec, bitset, "valueBlockAggTopN");
        }

        auto [keyBlocks, valBlocks] =
            makeBlockTopBottomNInputs<InBlockType, UnextractableTestBlock>(
                inputKeysVec.back(),
                outValsVec.back(),
                startIdx,
                inputKeyMins.empty() ? TypedValues{} : inputKeyMins.back(),
                inputKeyMaxs.empty() ? TypedValues{} : inputKeyMaxs.back());

        bottomNState = {value::TypeTags::Nothing, 0u};
        auto [topNFinal, _] = executeAndFinalizeTopBottomN(
            keyBlocks, valBlocks, topNState, bottomNState, bitset, sortSpec);

        value::ValueGuard topNFinalGuard{topNFinal};

        // Add to the keys we have encountered so far.
        addToCombinedBlocks(combinedKeyBlocks, inputKeysVec.back());

        verifyTopBottomNOutput(combinedKeyBlocks, topNFinal, sortSpec, bitset, "valueBlockAggTopN");
    };

    {
        // Input blocks don't have min or max set. The second input block will be extracted but
        // the second output shouldn't be extracted since combineBlockNativeHashAgg lazily
        // extracts.
        runExtractionTest.template operator()<TestBlock>({inputKeysLow, inputKeysHigh},
                                                         {outVals1, outVals2},
                                                         bitset,
                                                         4 /* maxSize */,
                                                         true /* isAscending */,
                                                         2 /* numIters */);

        // Descending sort shouldn't extract the output now that the high keys come first.
        runExtractionTest.template operator()<TestBlock>({inputKeysHigh, inputKeysLow},
                                                         {outVals1, outVals2},
                                                         bitset,
                                                         4 /* maxSize */,
                                                         false /* isAscending */,
                                                         2 /* numIters */);
    }

    {
        // Input blocks have min and max set. Neither the second input or output block should be
        // extracted since we should be able to exit early using the min/max of the second input
        // block.
        runExtractionTest.template operator()<UnextractableTestBlock>({inputKeysLow, inputKeysHigh},
                                                                      {outVals1, outVals2},
                                                                      bitset,
                                                                      4 /* maxSize */,
                                                                      true /* isAscending */,
                                                                      2 /* numIters */,
                                                                      {{lowMin}, {highMin}},
                                                                      {{lowMax}, {highMax}});

        // Descending sort shouldn't extract the input or output now that the high keys come
        // first.
        runExtractionTest.template operator()<UnextractableTestBlock>({inputKeysHigh, inputKeysLow},
                                                                      {outVals1, outVals2},
                                                                      bitset,
                                                                      4 /* maxSize */,
                                                                      false /* isAscending */,
                                                                      2 /* numIters */,
                                                                      {{highMin}, {lowMin}},
                                                                      {{highMax}, {lowMax}});
    }

    {
        // Input blocks don't have min and max set. Since we are doing an ascending sort and the
        // high keys come first, we should hit a tassert from trying to extract an unextractable
        // input or output block.
        ASSERT_THROWS_CODE(
            runExtractionTest.template operator()<TestBlock>({inputKeysHigh, inputKeysLow},
                                                             {outVals1, outVals2},
                                                             bitset,
                                                             4 /* maxSize */,
                                                             true /* isAscending */,
                                                             2 /* numIters */),
            DBException,
            8776400);

        // Descending sort shouldn't extract the input or output now that the high keys come
        // first.
        ASSERT_THROWS_CODE(runExtractionTest.template operator()<UnextractableTestBlock>(
                               {inputKeysHigh, inputKeysLow},
                               {outVals1, outVals2},
                               bitset,
                               4 /* maxSize */,
                               true /* isAscending */,
                               2 /* numIters */,
                               {{highMin}, {lowMin}},
                               {{highMax}, {lowMax}}),
                           DBException,
                           8776400);
    }

    release2dValueVector(std::move(inputKeysLow));
    release2dValueVector(std::move(inputKeysHigh));
    release2dValueVector(std::move(outVals1));
    release2dValueVector(std::move(outVals2));
}
}  // namespace mongo::sbe
