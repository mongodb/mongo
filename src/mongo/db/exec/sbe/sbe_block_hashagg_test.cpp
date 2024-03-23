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

#include <algorithm>
#include <functional>
#include <map>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/sbe_block_test_helpers.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/block_hashagg.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::sbe {

namespace {
typedef std::map<std::vector<int32_t>, std::vector<int32_t>> TestResultType;
using TypedValue = std::pair<value::TypeTags, value::Value>;
}  // namespace

using AccNamesVector = std::vector<std::tuple<std::string, std::string, std::string>>;

class BlockHashAggStageTest : public PlanStageTestFixture {
public:
    void setUp() override {
        PlanStageTestFixture::setUp();
        _globalLock = std::make_unique<Lock::GlobalLock>(operationContext(), MODE_IS);
    }

    void tearDown() override {
        _globalLock.reset();
        PlanStageTestFixture::tearDown();
    }

    static std::vector<TypedValue> unpackBlock(TypedValue blockPair, size_t expectedBlockSize) {
        auto [blockTag, blockVal] = blockPair;
        ASSERT_EQ(blockTag, value::TypeTags::valueBlock);
        auto deblocked = value::bitcastTo<value::ValueBlock*>(blockVal)->extract();

        ASSERT_EQ(deblocked.count(), expectedBlockSize);
        std::vector<TypedValue> res(deblocked.count());
        for (size_t i = 0; i < deblocked.count(); ++i) {
            res[i] = deblocked[i];
        }
        return res;
    }

    static std::vector<std::vector<TypedValue>> unpackArrayOfBlocks(value::Value arrayVal,
                                                                    size_t expectedBlockSize) {
        auto arr = value::getArrayView(arrayVal);
        std::vector<std::vector<TypedValue>> result;
        for (size_t i = 0; i < arr->size(); i++) {
            result.emplace_back(unpackBlock(arr->getAt(i), expectedBlockSize));
        }
        return result;
    }

    static TypedValue makeArray(std::vector<TypedValue> vals) {
        auto [arrTag, arrVal] = value::makeNewArray();
        value::ValueGuard guard(arrTag, arrVal);
        for (auto [t, v] : vals) {
            value::getArrayView(arrVal)->push_back(t, v);
        }
        guard.reset();
        return {arrTag, arrVal};
    }

    // This helper takes an array of groupby results and compares to the expectedMap of group ID to
    // a list of accumulator results.
    static void assertResultMatchesMap(TypedValue result,
                                       TestResultType expectedMap,
                                       std::vector<size_t> expectedOutputBlockSizes) {
        ASSERT_EQ(result.first, value::TypeTags::Array);
        auto resultArr = value::getArrayView(result.second);

        size_t ebsIndex = 0;
        size_t keySize = expectedMap.empty() ? 0 : expectedMap.begin()->first.size();
        for (auto [subArrTag, subArrVal] : resultArr->values()) {
            ASSERT_EQ(subArrTag, value::TypeTags::Array);

            // The first "row" in the vector stores the keys, and each subsequent row stores the
            // value of each accumulator. results[0][1] gives you the {tag, val} of the second key.
            // results[1][2] gives you the {tag, val} of the first accumlator for the third group.
            auto results = unpackArrayOfBlocks(subArrVal, expectedOutputBlockSizes[ebsIndex++]);

            // Iterate over each key.
            for (size_t rowIdx = 0; rowIdx < results[0].size(); ++rowIdx) {
                std::vector<int32_t> key(keySize);
                for (size_t keyIdx = 0; keyIdx < keySize; ++keyIdx) {
                    ASSERT_EQ(results[keyIdx][rowIdx].first, value::TypeTags::NumberInt32);
                    key[keyIdx] = value::bitcastTo<int32_t>(results[keyIdx][rowIdx].second);
                }

                auto expectedVals = expectedMap.at(key);
                ASSERT_EQ(results.size(), expectedVals.size() + keySize);

                // Check the expected results for each accumulator.
                for (size_t accIdx = 0; accIdx < expectedVals.size(); accIdx++) {
                    assertValuesEqual(results[accIdx + keySize][rowIdx].first,
                                      results[accIdx + keySize][rowIdx].second,
                                      value::TypeTags::NumberInt32,
                                      value::bitcastTo<int32_t>(expectedVals[accIdx]));
                }

                // Delete from the expected map so we know we get the results exactly once.
                expectedMap.erase(key);
            }
        }
        ASSERT(expectedMap.empty());
    }

    template <typename... BlockData>
    static TypedValue makeInputArray(int32_t id, std::vector<bool> bitset, BlockData... blockData) {
        auto [arrTag, arrVal] = value::makeNewArray();
        value::ValueGuard guard(arrTag, arrVal);
        auto arr = value::getArrayView(arrVal);

        // Append groupBy key.
        arr->push_back(makeInt32(id));
        // Append bitset block.
        auto bitsetBlock = makeBoolBlock(bitset);
        arr->push_back({sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(bitsetBlock.release())});
        // Append data.
        (arr->push_back(makeHeterogeneousBlockTagVal(blockData)), ...);
        guard.reset();
        return {arrTag, arrVal};
    }

    template <typename... BlockData>
    static TypedValue makeInputArray(std::vector<std::vector<TypedValue>> ids,
                                     std::vector<bool> bitset,
                                     BlockData... blockData) {
        auto [arrTag, arrVal] = value::makeNewArray();
        value::Array* arr = value::getArrayView(arrVal);

        // Append groupby keys.
        for (auto& id : ids) {
            arr->push_back(makeHeterogeneousBlockTagVal(id));
        }
        // Append corresponding bitset.
        auto bitsetBlock = makeBoolBlock(bitset);
        arr->push_back({sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(bitsetBlock.release())});
        // Append data.
        (arr->push_back(makeHeterogeneousBlockTagVal(blockData)), ...);
        return {arrTag, arrVal};
    }

    void runBlockHashAggTestHelper(TypedValue inputData,
                                   size_t keySize,
                                   size_t numScanSlots,
                                   AccNamesVector accNames,
                                   TestResultType expectedResultsMap,
                                   std::vector<size_t> expectedOutputBlockSizes,
                                   bool spillToDisk) {
        auto makeFn = [&](value::SlotVector scanSlots, std::unique_ptr<PlanStage> scanStage) {
            value::SlotVector idSlots;
            value::SlotVector outputSlots;
            for (size_t i = 0; i < keySize; ++i) {
                idSlots.push_back(scanSlots[i]);
                outputSlots.push_back(idSlots[i]);
            }

            auto bitsetInSlot = scanSlots[keySize];

            value::SlotVector dataInSlots;
            value::SlotVector accDataSlots;
            SlotExprPairVector mergingExprs;

            auto accumulatorBitset = generateSlotId();

            AggExprTupleVector aggs;

            size_t scanSlotIdx = keySize + 1;
            for (const auto& [blockAcc, rowAcc, mergeAcc] : accNames) {
                auto outputSlot = generateSlotId();
                std::unique_ptr<EExpression> blockAccFunc;
                std::unique_ptr<EExpression> rowAccFunc;

                if (blockAcc == "valueBlockAggCount") {
                    // valueBlockAggCount is the exception - it takes just the bitset.
                    blockAccFunc =
                        stage_builder::makeFunction(blockAcc, makeE<EVariable>(accumulatorBitset));
                    rowAccFunc = stage_builder::makeFunction(rowAcc);
                } else {
                    dataInSlots.push_back(scanSlots[scanSlotIdx++]);

                    auto internalSlot = generateSlotId();
                    accDataSlots.push_back(internalSlot);

                    blockAccFunc = stage_builder::makeFunction(blockAcc,
                                                               makeE<EVariable>(accumulatorBitset),
                                                               makeE<EVariable>(internalSlot));
                    rowAccFunc =
                        stage_builder::makeFunction(rowAcc, makeE<EVariable>(internalSlot));
                }

                aggs.emplace_back(
                    outputSlot,
                    AggExprTuple{nullptr, std::move(blockAccFunc), std::move(rowAccFunc)});

                outputSlots.push_back(outputSlot);

                auto mergeInternalSlot = generateSlotId();
                mergingExprs.emplace_back(
                    mergeInternalSlot,
                    stage_builder::makeFunction(mergeAcc, makeE<EVariable>(mergeInternalSlot)));
            }

            auto outStage = makeS<BlockHashAggStage>(std::move(scanStage),
                                                     idSlots,
                                                     bitsetInSlot,
                                                     dataInSlots,
                                                     accDataSlots,
                                                     accumulatorBitset,
                                                     std::move(aggs),
                                                     spillToDisk /* allowDiskUse */,
                                                     std::move(mergingExprs),
                                                     nullptr /* yieldPolicy */,
                                                     kEmptyPlanNodeId,
                                                     true,
                                                     spillToDisk /* forceIncreasedSpilling */);
            return std::make_pair(outputSlots, std::move(outStage));
        };

        auto result = runTestMulti(numScanSlots, inputData.first, inputData.second, makeFn);
        value::ValueGuard resultGuard{result};
        assertResultMatchesMap(result, expectedResultsMap, expectedOutputBlockSizes);
    }  // runBlockHashAggTestHelper

    /**
     * Given the data input, the number of slots the stage requires, accumulators used, and
     * expected output, runs the BlockHashAgg stage and asserts that we get correct results.
     * (SBE $group is implemented by sbe::BlockHashAggStage.)
     *
     * *******************************************************************************************
     * IMPORTANT:
     *    This tests the BlockHashAggStage in isolation and only supports scalar outputs from
     *    single accumulator calls. Many accumulators (e.g. $sum) output a 3- or 4-value
     *    accumulator Array, or even multiple such arrays (e.g. $avg), and require a "finalize"
     *    step to convert these to a scalar. However, the finalize step is not part of
     *    BlockHashAggStage but rather is a downstream step added by the stage builder. Thus
     *    this unit test cannot be used with any accumulators that require a finalize step.
     * *******************************************************************************************
     *
     * In the following Inputs section, [ ] is used to represent both SBE TypeTags::Array arrays and
     * C++ std::vectors, as this is the most common way to denote vectors or arrays, whereas { }
     * instead looks like it is trying to indicate a JSON object. (Note TypeTags::Array itself is a
     * wrapper around a C++ std::vector.) The test cases themselves initialize these using C++
     * literal array initializers, which confusingly use { }, as these really represent C++
     * constructor calls. Hopefully this note will help avoid confusion.
     *
     * Inputs
     *
     *   inputData - A [tag, val] pair representing the inputs to the $group stage, where
     *     tag - Always TypeTags::Array.
     *     val - An array of arrays. Each child array represents a subset of docs for one group_id
     *        and is of the form
     *          [group_id, [bitset], [optional_field_A_vals], ... [optional_field_N_vals]]
     *        where
     *          group_id is the value of this subset's $group key.
     *          [bitset] is an Array of booleans of the same length as the block indicating which
     *            entries in the block should be included (true). The others (false) are filtered
     *            out (by a $match expression).
     *          [optional_field_X_vals], if present, is an Array of the same length as the block,
     *            each of whose entries represents one value for field 'X' in a doc in this block.
     *        In the code, "blocks" are 1D vectors, so each array of optional_field_X_vals is a
     *        block and the bitset is a pseudo-block, but all of these are parallel with each other
     *        from the same subset of docs. Note that a single group_id may also have more than one
     *        child array representing it in 'inputData'.
     *
     *   keySize - If the $group key is a scalar, this is 1, else the key is multipart in the form
     *     of a vector with K entries, in which case 'keySize' is set to K.
     *
     *   numScanSlots - The total number of distinct input sources. These are:
     *       o Each component of the $group key
     *       o The bitset
     *       o Each field scanned from the input docs (== each non-$count accumulator)
     *     $count does not add a slot as it consumes the bitset, whereas every other accumulator
     *     consumes a scanned input field.
     *
     *   accNames - A vector of vectors. Each child vector represents one accumulator to be applied
     *     to the input data and is of the form
     *       [block_accumulator_fn_name, row_accumulator_fn_name, merge_accumulator_fn_name]
     *     For example:
     *       - The $min accumulator is represented by ["valueBlockAggMin", "min", "min"].
     *       - The $count accumulator is represented by ["valueBlockAggCount", "count", "sum"].
     *     The accumulators will be applied left-to-right to the fields included in 'inputData'
     *     except that the $count accumulator always applies to the [bitset] Array, no matter where
     *     or how many times it appears in the 'accNames' vector. I.e. the first non-$count
     *     accumulator will apply to the first Array of field values, the second non-$count
     *     accumulator will apply to the second Array of field values, and so on.
     *
     *   expectedResultsMap - Expected results in the form of a map from group_id to an array of
     *     accumulator output values in the order the accumulators were specified, e.g.
     *       [[group_id_0], [acc_0_resullt, acc_1_result, acc_2_result],
     *        ...
     *        [group_id_N], [acc_0_resullt, acc_1_result, acc_2_result]]
     *
     *   expectedOutputBlockSizes - A vector whose number of entries corresponds to the number of
     *     output blocks the $group stage is expected to produce and whose 'i'th entry gives the
     *     expected length of the 'i'th output block. This stage produces one output block entry per
     *     unique group_id, and the stage's output block size is capped at
     *     BlockHashAggStage::kBlockOutSize. Thus if there are more than 'kBlockOutSize' unique
     *     group_ids, the stage will produce more than one output block. All but the last one will
     *     be of size 'kBlockOutSize', while the last one may be smaller (anywhere in the range 1 to
     *     'kBlockOutSize').
     */
    void runBlockHashAggTest(TypedValue inputData,
                             size_t keySize,
                             size_t numScanSlots,
                             AccNamesVector accNames,
                             TestResultType expectedResultsMap,
                             std::vector<size_t> expectedOutputBlockSizes) {
        runBlockHashAggTestHelper(value::copyValue(inputData.first, inputData.second),
                                  keySize,
                                  numScanSlots,
                                  accNames,
                                  expectedResultsMap,
                                  expectedOutputBlockSizes,
                                  false /* spillToDisk */);
        runBlockHashAggTestHelper(inputData,
                                  keySize,
                                  numScanSlots,
                                  accNames,
                                  expectedResultsMap,
                                  expectedOutputBlockSizes,
                                  true /* spillToDisk */);
    }  // runBlockHashAggTest

private:
    std::unique_ptr<Lock::GlobalLock> _globalLock;
};

TEST_F(BlockHashAggStageTest, NoData) {
    auto [inputTag, inputVal] = makeArray({});
    // We should have an empty block with no data.
    TestResultType expected = {};
    runBlockHashAggTest(std::make_pair(inputTag, inputVal),
                        0,
                        3,
                        {{"valueBlockAggMin", "min", "min"}},
                        expected,
                        {});
}

TEST_F(BlockHashAggStageTest, AllDataFiltered) {
    // All data has "false" for bitset.
    auto [inputTag, inputVal] = makeArray(
        {makeInputArray({makeInt32s({0, 1, 2})}, {false, false, false}, makeInt32s({50, 20, 30}))});
    // We should have an empty block with no data.
    TestResultType expected = {};
    runBlockHashAggTest(std::make_pair(inputTag, inputVal),
                        1,
                        3,
                        {{"valueBlockAggMin", "min", "min"}},
                        expected,
                        {});
}

TEST_F(BlockHashAggStageTest, ScalarKeySingleAccumulatorMin) {
    // Each entry is ID followed by bitset followed by a block of data. For example
    // [groupid, [block bitset values], [block data values]]
    auto [inputTag, inputVal] =
        makeArray({makeInputArray(0, {true, true, false}, makeInt32s({50, 20, 30})),
                   makeInputArray(2, {false, true, true}, makeInt32s({40, 30, 60})),
                   makeInputArray(1, {true, true, true}, makeInt32s({70, 80, 10})),
                   makeInputArray(2, {false, false, false}, makeInt32s({10, 20, 30})),
                   makeInputArray(2, {true, false, true}, makeInt32s({30, 40, 50}))});
    /*
     * 0 -> min(50, 20) = 20
     * 1 -> min(70, 80, 10) = 10
     * 2 -> min(30, 60, 30, 50) = 30
     */
    TestResultType expected = {{{0}, {20}}, {{1}, {10}}, {{2}, {30}}};
    runBlockHashAggTest(std::make_pair(inputTag, inputVal),
                        1,
                        3,
                        {{"valueBlockAggMin", "min", "min"}},
                        expected,
                        {3});
}

TEST_F(BlockHashAggStageTest, ScalarKeyCount) {
    // Each entry is ID followed by a bitset.
    auto [inputTag, inputVal] = makeArray({makeInputArray(0, {true, true, true}),
                                           makeInputArray(0, {true, false, true}),
                                           makeInputArray(1, {true, false, true}),
                                           makeInputArray(1, {true, true, false})});
    TestResultType expected = {{{0}, {5}}, {{1}, {4}}};
    runBlockHashAggTest(std::make_pair(inputTag, inputVal),
                        1,
                        3,
                        {{"valueBlockAggCount", "count", "sum"}},
                        expected,
                        {2});
}

TEST_F(BlockHashAggStageTest, ScalarKeySum) {
    // Each entry is ID followed by bitset followed by a block of data.
    auto [inputTag, inputVal] =
        makeArray({makeInputArray(0, {true, true, false}, makeInt32s({1, 2, 3})),
                   makeInputArray(2, {false, true, true}, makeInt32s({4, 5, 6})),
                   makeInputArray(1, {true, true, true}, makeInt32s({7, 8, 9})),
                   makeInputArray(2, {false, false, false}, makeInt32s({10, 11, 12})),
                   makeInputArray(2, {true, false, true}, makeInt32s({13, 14, 15}))});
    /*
     * 0 -> 1+2 = 3
     * 1 -> 7+8+9 = 24
     * 2 -> 5+6+13+15 = 39
     */
    TestResultType expected = {{{0}, {3}}, {{1}, {24}}, {{2}, {39}}};
    runBlockHashAggTest(std::make_pair(inputTag, inputVal),
                        1,
                        3,
                        {{"valueBlockAggSum", "sum", "sum"}},
                        expected,
                        {3});
}

TEST_F(BlockHashAggStageTest, ScalarKeyMultipleAccumulators) {
    // Each entry is ID followed by bitset followed by block A and block B.
    auto [inputTag, inputVal] = makeArray(
        {makeInputArray(
             100, {true, true, false}, makeInt32s({200, 100, 150}), makeInt32s({2, 4, 7})),
         makeInputArray(
             100, {false, true, true}, makeInt32s({50, 90, 60}), makeInt32s({-100, 20, 3})),
         makeInputArray(
             50, {true, true, true}, makeInt32s({200, 100, 150}), makeInt32s({-150, 150, 20})),
         makeInputArray(
             25, {true, false, false}, makeInt32s({20, 75, 10}), makeInt32s({0, 20, -20})),
         makeInputArray(
             50, {true, false, true}, makeInt32s({75, 75, 75}), makeInt32s({-2, 5, 8}))});
    /*
     * 25  -> min(20) = 20, count=1, min(0) = 0
     * 50  -> min(200, 100, 150, 75, 75) = 75, count = 5, min(-150, 150, 20, -2, 8) = -150
     * 100 -> min(200, 100, 90, 60) = 60, count = 4, min(2, 4, 20, 3) = 2
     */
    TestResultType expected = {{{25}, {20, 1, 0}}, {{50}, {75, 5, -150}}, {{100}, {60, 4, 2}}};
    runBlockHashAggTest(std::make_pair(inputTag, inputVal),
                        1,
                        4,
                        {{"valueBlockAggMin", "min", "min"},
                         {"valueBlockAggCount", "count", "sum"},
                         {"valueBlockAggMin", "min", "min"}},
                        expected,
                        {3});
}

TEST_F(BlockHashAggStageTest, Count) {
    // Each entry is ID followed by a bitset.
    auto [inputTag, inputVal] =
        makeArray({makeInputArray({makeInt32s({0, 1, 0})}, {true, true, true}),
                   makeInputArray({makeInt32s({0, 0, 1})}, {true, false, true}),
                   makeInputArray({makeInt32s({0, 1, 1})}, {true, false, true}),
                   makeInputArray({makeInt32s({1, 1, 0})}, {true, true, false})});
    TestResultType expected = {{{0}, {4}}, {{1}, {5}}};
    runBlockHashAggTest(std::make_pair(inputTag, inputVal),
                        1,
                        3,
                        {{"valueBlockAggCount", "count", "sum"}},
                        expected,
                        {2});
}

TEST_F(BlockHashAggStageTest, SumBlockGroupByKey) {
    // Each entry is ID followed by bitset followed by a block of data.
    auto [inputTag, inputVal] = makeArray(
        {makeInputArray({makeInt32s({0, 0, 0})}, {true, true, false}, makeInt32s({1, 2, 3})),
         makeInputArray({makeInt32s({2, 2, 2})}, {false, true, true}, makeInt32s({4, 5, 6})),
         makeInputArray({makeInt32s({1, 1, 1})}, {true, true, true}, makeInt32s({7, 8, 9})),
         makeInputArray({makeInt32s({2, 2, 2})}, {false, false, false}, makeInt32s({10, 11, 12})),
         makeInputArray({makeInt32s({2, 2, 2})}, {true, false, true}, makeInt32s({13, 14, 15}))});

    /*
     * 0 -> 1+2 = 3
     * 1 -> 7+8+9 = 24
     * 2 -> 5+6+13+15 = 39
     */
    TestResultType expected = {{{0}, {3}}, {{1}, {24}}, {{2}, {39}}};
    runBlockHashAggTest(std::make_pair(inputTag, inputVal),
                        1,
                        3,
                        {{"valueBlockAggSum", "sum", "sum"}},
                        expected,
                        {3});
}

// Similar to the test above, but we change the groupby keys so they are different within each
// block.
TEST_F(BlockHashAggStageTest, SumDifferentBlockGroupByKeys) {
    // Each entry is ID followed by bitset followed by a block of data.
    auto [inputTag, inputVal] = makeArray(
        {makeInputArray({makeInt32s({1, 2, 3})}, {true, true, false}, makeInt32s({1, 2, 3})),
         makeInputArray({makeInt32s({2, 2, 2})}, {false, true, true}, makeInt32s({4, 5, 6})),
         makeInputArray({makeInt32s({3, 2, 1})}, {true, true, true}, makeInt32s({7, 8, 9})),
         makeInputArray({makeInt32s({2, 3, 4})}, {false, true, true}, makeInt32s({10, 11, 12})),
         makeInputArray({makeInt32s({2, 3, 4})}, {false, false, false}, makeInt32s({0, 5, 4})),
         makeInputArray({makeInt32s({1, 1, 2})}, {true, true, true}, makeInt32s({13, 14, 15}))});

    /*
     * 1 -> 1+9+13+14  = 37
     * 2 -> 2+5+6+8+15 = 36
     * 3 -> 7+11       = 18
     * 4 -> 12         = 12
     */
    TestResultType expected = {{{1}, {37}}, {{2}, {36}}, {{3}, {18}}, {{4}, {12}}};
    runBlockHashAggTest(std::make_pair(inputTag, inputVal),
                        1,
                        3,
                        {{"valueBlockAggSum", "sum", "sum"}},
                        expected,
                        {4});
}

// Similar test as above but the "2" key appears in every block but is always false, so we make
// sure it's missing.
TEST_F(BlockHashAggStageTest, SumDifferentBlockGroupByKeysMissingKey) {
    // Each entry is ID followed by bitset followed by a block of data.
    // Mix high partition rows with low partition.
    auto [inputTag, inputVal] = makeArray(
        {makeInputArray({makeInt32s({1, 2, 3, 5, 6, 7})},
                        {true, false, false, true, true, true},
                        makeInt32s({1, 2, 3, 4, 5, 6})),
         makeInputArray({makeInt32s({2, 2, 2})}, {false, false, false}, makeInt32s({4, 5, 6})),
         makeInputArray({makeInt32s({3, 2, 1, 7, 6, 5})},
                        {true, false, true, false, true, true},
                        makeInt32s({7, 8, 9, 1, 2, 3})),
         makeInputArray({makeInt32s({2, 3, 4, 6, 7, 5})},
                        {false, true, true, true, true, false},
                        makeInt32s({10, 11, 12, 15, 15, 15})),
         makeInputArray({makeInt32s({2, 3, 4})}, {false, false, false}, makeInt32s({0, 5, 4})),
         makeInputArray({makeInt32s({1, 1, 2})}, {true, true, false}, makeInt32s({13, 14, 15}))});

    /*
     * 1 -> 1+9+13+14  = 37
     * 2 -> missing
     * 3 -> 7+11       = 18
     * 4 -> 12         = 12
     * 5 -> 4+3        = 7
     * 6 -> 5+2+15     = 22
     * 7 -> 6+15       = 21
     */
    TestResultType expected = {
        {{1}, {37}}, {{3}, {18}}, {{4}, {12}}, {{5}, {7}}, {{6}, {22}}, {{7}, {21}}};
    runBlockHashAggTest(std::make_pair(inputTag, inputVal),
                        1,
                        3,
                        {{"valueBlockAggSum", "sum", "sum"}},
                        expected,
                        {6});
}

TEST_F(BlockHashAggStageTest, MultipleAccumulatorsDifferentBlockGroupByKeys) {
    // Each entry is ID followed by bitset followed by block A and block B.
    auto [inputTag, inputVal] = makeArray({makeInputArray({makeInt32s({25, 50, 100})},
                                                          {true, true, false},
                                                          makeInt32s({200, 100, 150}),
                                                          makeInt32s({2, 4, 7})),
                                           makeInputArray({makeInt32s({50, 50, 50})},
                                                          {false, true, true},
                                                          makeInt32s({50, 90, 60}),
                                                          makeInt32s({-100, 20, 3})),
                                           makeInputArray({makeInt32s({25, 25, 100})},
                                                          {true, true, true},
                                                          makeInt32s({200, 100, 150}),
                                                          makeInt32s({-150, 150, 2})),
                                           makeInputArray({makeInt32s({100, 50, 25})},
                                                          {true, false, false},
                                                          makeInt32s({20, 75, 10}),
                                                          makeInt32s({0, 20, -20})),
                                           makeInputArray({makeInt32s({100, 25, 50})},
                                                          {true, false, true},
                                                          makeInt32s({75, 75, 75}),
                                                          makeInt32s({-2, 5, 8}))});

    /*
     * 25  -> min(200, 200, 100) = 100, count = 3, min(2, -150, 150) = -150
     * 50  -> min(100, 90, 60, 75) = 60, count = 4, min(4, 20, 3, 8) = 3
     * 100 -> min(150, 20, 75) = 20, count = 3, min(20, 0, -2) = -2
     */
    TestResultType expected = {{{25}, {100, 3, -150}}, {{50}, {60, 4, 3}}, {{100}, {20, 3, -2}}};
    runBlockHashAggTest(std::make_pair(inputTag, inputVal),
                        1,
                        4,
                        {{"valueBlockAggMin", "min", "min"},
                         {"valueBlockAggCount", "count", "sum"},
                         {"valueBlockAggMin", "min", "min"}},
                        expected,
                        {3});
}

TEST_F(BlockHashAggStageTest, CountCompoundKey) {
    // Each entry is ID followed by a bitset.
    auto [inputTag, inputVal] = makeArray(
        {makeInputArray({makeInt32s({0, 1, 0}), makeInt32s({0, 0, 1})}, {true, true, true}),
         makeInputArray({makeInt32s({0, 0, 1}), makeInt32s({0, 1, 0})}, {true, false, true}),
         makeInputArray({makeInt32s({0, 1, 1}), makeInt32s({1, 0, 1})}, {true, false, true}),
         makeInputArray({makeInt32s({1, 1, 0}), makeInt32s({1, 1, 1})}, {true, true, false})});
    TestResultType expected = {{{0, 0}, {2}}, {{0, 1}, {2}}, {{1, 0}, {2}}, {{1, 1}, {3}}};
    runBlockHashAggTest(std::make_pair(inputTag, inputVal),
                        2,
                        3,
                        {{"valueBlockAggCount", "count", "sum"}},
                        expected,
                        {4});
}

// TODO SERVER-85731 Revisit input sizes if kMaxNumPartitionsForTokenizedPath changes.
TEST_F(BlockHashAggStageTest, SumCompoundKeysMissingKey) {
    // Each entry is ID followed by bitset followed by a block of data.
    // Mix high partition rows with low partition.
    auto [inputTag, inputVal] =
        makeArray({makeInputArray({makeInt32s({1, 2, 3, 5, 6, 7}), makeInt32s({2, 3, 4, 6, 7, 8})},
                                  {true, false, false, true, true, true},
                                  makeInt32s({1, 2, 3, 4, 5, 6})),
                   makeInputArray({makeInt32s({2, 2, 2}), makeInt32s({3, 3, 3})},
                                  {false, false, false},
                                  makeInt32s({4, 5, 6})),
                   makeInputArray({makeInt32s({3, 2, 1, 7, 6, 5}), makeInt32s({4, 3, 2, 8, 7, 6})},
                                  {true, false, true, false, true, true},
                                  makeInt32s({7, 8, 9, 1, 2, 3})),
                   makeInputArray({makeInt32s({2, 3, 4, 6, 7, 5}), makeInt32s({3, 4, 5, 7, 8, 6})},
                                  {false, true, true, true, true, false},
                                  makeInt32s({10, 11, 12, 15, 15, 15})),
                   makeInputArray({makeInt32s({2, 3, 4}), makeInt32s({3, 4, 5})},
                                  {false, false, false},
                                  makeInt32s({0, 5, 4})),
                   makeInputArray({makeInt32s({1, 1, 2}), makeInt32s({2, 2, 3})},
                                  {true, true, false},
                                  makeInt32s({13, 14, 15}))});

    /*
     * {1, 2} -> 1+9+13+14  = 37
     * {2, 3} -> missing
     * {3, 4} -> 7+11       = 18
     * {4, 5} -> 12         = 12
     * {5, 6} -> 4+3        = 7
     * {6, 7} -> 5+2+15     = 22
     * {7, 8} -> 6+15       = 21
     */
    TestResultType expected = {{{1, 2}, {37}},
                               {{3, 4}, {18}},
                               {{4, 5}, {12}},
                               {{5, 6}, {7}},
                               {{6, 7}, {22}},
                               {{7, 8}, {21}}};
    runBlockHashAggTest(std::make_pair(inputTag, inputVal),
                        2,
                        4,
                        {{"valueBlockAggSum", "sum", "sum"}},
                        expected,
                        {6});
}

TEST_F(BlockHashAggStageTest, BlockOutSizeTest) {
    TestResultType expected;
    auto addToExpected = [&expected](int32_t id, bool exists, int32_t data) {
        if (exists) {
            expected.emplace(std::vector<int32_t>{id}, std::vector<int32_t>{0});
            expected[std::vector<int32_t>{id}][0] += data;
        }
    };

    std::vector<TypedValue> vals;
    // Create kBlockOutSize * 3 + 1 group ids, so that the output is 3 blocks of size
    // kBlockOutSize, and 1 block of size 1.
    for (size_t id = 0; id < BlockHashAggStage::kBlockOutSize * 3 + 1; ++id) {
        std::vector<int32_t> ids;
        std::vector<bool> bitmap;
        std::vector<int32_t> data;

        for (size_t i = 0; i < 6; i++) {
            // Every third entry will be false.
            bool exists = i % 3 != 0;
            int32_t dataPoint = i + id * 5;

            // Add to our expected result map, and to our input data.
            addToExpected(id, exists, dataPoint);
            ids.push_back(id);
            bitmap.push_back(exists);
            data.push_back(dataPoint);
        }

        auto input = makeInputArray({makeInt32s(ids)}, bitmap, makeInt32s(data));
        vals.push_back(input);
    }

    auto [inputTag, inputVal] = makeArray(vals);
    runBlockHashAggTest(std::make_pair(inputTag, inputVal),
                        1,
                        3,
                        {{"valueBlockAggSum", "sum", "sum"}},
                        expected,
                        {BlockHashAggStage::kBlockOutSize,
                         BlockHashAggStage::kBlockOutSize,
                         BlockHashAggStage::kBlockOutSize,
                         1});
}

namespace {
class MinFunctor {
public:
    template <typename T>
    T operator()(T a, T b) {
        return std::min(a, b);
    }
};
}  // namespace

TEST_F(BlockHashAggStageTest, MultipleAccumulatorsDifferentPartitionSizes) {
    // Loop from three below the partition limit to three above the partition limit, adding blocks
    // with these partition sizes. The block size will be two times this size, so there will be two
    // entries per key. For example for partitionSize=3 we would have [1,2,3,1,2,3].
    const size_t lowPartitionSize = BlockHashAggStage::kMaxNumPartitionsForTokenizedPath - 3;
    const size_t highPartitionSize = BlockHashAggStage::kMaxNumPartitionsForTokenizedPath + 3;

    TestResultType expected;
    size_t numAccs = 3;
    auto updateExpected =
        [&expected, numAccs]<typename Op>(
            std::vector<int32_t> id, size_t accIdx, bool exists, Op op, int32_t data = 1) {
            invariant(accIdx < numAccs);
            if (exists) {
                expected.emplace(id, std::vector<int32_t>(numAccs, 0));
                expected[id][accIdx] = op(expected[id][accIdx], data);
            }
        };

    std::vector<std::pair<value::TypeTags, value::Value>> vals;
    size_t keySize = 4;
    size_t i = 0;
    for (size_t partitionSize = lowPartitionSize; partitionSize <= highPartitionSize;
         partitionSize++) {
        std::vector<std::vector<int32_t>> ids(keySize, std::vector<int32_t>());
        std::vector<bool> bitmap;
        std::vector<int32_t> data1;  // sum
        std::vector<int32_t> data2;  // min

        for (size_t dupRound = 0; dupRound < 2; dupRound++) {
            for (size_t blockIndex = 0; blockIndex < partitionSize; blockIndex++) {
                std::vector<int32_t> id;
                for (size_t idIdx = 0; idIdx < keySize; ++idIdx) {
                    id.push_back(blockIndex + idIdx);
                    ids[idIdx].push_back(blockIndex + idIdx);
                }
                // Every third entry will be false.
                bool exists = i % 3 != 0;
                int32_t dataPoint1 = partitionSize * 2 + dupRound * 3 + blockIndex * 5;
                int32_t dataPoint2 = -1 * blockIndex;

                // Add to our expected result map, and to our input data.
                updateExpected(id, 0, exists, std::plus<>(), dataPoint1);  // Expected sum
                updateExpected(id, 1, exists, std::plus<>());              // Expected count
                updateExpected(id, 2, exists, MinFunctor(), dataPoint2);   // Expected min
                bitmap.push_back(exists);
                data1.push_back(dataPoint1);
                data2.push_back(dataPoint2);
                i++;
            }
        }
        std::vector<std::vector<TypedValue>> expectedKeys(keySize);
        for (size_t idIdx = 0; idIdx < keySize; ++idIdx) {
            expectedKeys[idIdx] = makeInt32s(ids[idIdx]);
        }
        auto input = makeInputArray(expectedKeys, bitmap, makeInt32s(data1), makeInt32s(data2));
        vals.push_back(input);
    }

    auto [inputTag, inputVal] = makeArray(vals);
    runBlockHashAggTest(std::make_pair(inputTag, inputVal),
                        keySize,
                        keySize + 1 /* bitset */ + 2 /* numDataBlocks */,
                        {{"valueBlockAggSum", "sum", "sum"},
                         {"valueBlockAggCount", "count", "sum"},
                         {"valueBlockAggMin", "min", "min"}},
                        expected,
                        {highPartitionSize - lowPartitionSize + 2});
}
}  // namespace mongo::sbe
