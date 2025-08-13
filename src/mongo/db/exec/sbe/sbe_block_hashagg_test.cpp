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

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/sbe_block_test_helpers.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/block_hashagg.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"

#include <algorithm>
#include <functional>
#include <map>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {

namespace {
// Represents one "bucket's" worth of data to be fed into the group stage including:
// - A group by key (either 'scalarId' or 'ids')
// - A selectivity bitset
// - A vector of blocks, one for each column
//
// For example:
// group keys        bitset      'a'       'b'
//  99  101       |    true   |  'foo'  |  'bar'
//  99  100       |    false  |  'bar'  |  'bar'
//  99  101       |    true   |  'foo'  |  'foo'
struct Bucket {
    // Only one of these may be set.
    std::pair<value::TypeTags, value::Value> scalarId{value::TypeTags::Nothing, 0};
    std::vector<CopyableValueBlock> ids;

    std::vector<bool> bitset;
    std::vector<CopyableValueBlock> dataBlocks;
};

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
        constexpr bool kDebugTest = false;
        if (kDebugTest) {
            std::cout << "Result data...\n";
            std::cout << result << std::endl;

            std::cout << "Expected result\n";
            for (const auto& [keys, vals] : expectedMap) {
                for (auto k : keys) {
                    std::cout << k << ", ";
                }
                std::cout << "--->";
                for (auto v : vals) {
                    std::cout << v << ", ";
                }
                std::cout << std::endl;
            }
        }

        ASSERT_EQ(result.first, value::TypeTags::Array);
        auto resultArr = value::getArrayView(result.second);

        size_t ebsIndex = 0;
        size_t keySize = expectedMap.empty() ? 0 : expectedMap.begin()->first.size();
        for (auto [subArrTag, subArrVal] : resultArr->values()) {
            ASSERT_EQ(subArrTag, value::TypeTags::Array);

            // The first "row" in the vector stores the keys, and each subsequent row stores the
            // value of each accumulator. results[0][1] gives you the {tag, val} of the second key.
            // results[1][2] gives you the {tag, val} of the first accumulator for the third group.
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

    void runBlockHashAggTestHelper(
        const std::vector<Bucket>& inputData,
        AccNamesVector accNames,
        TestResultType expectedResultsMap,
        std::vector<size_t> expectedOutputBlockSizes,
        bool allowDiskUse,
        bool forceIncreasedSpilling,
        const AssertStageStatsFn& assertStageStats = AssertStageStatsFn{}) {

        // The user may specify a list of block IDs or a 'scalarId'.
        for (auto& bucket : inputData) {
            invariant(bucket.ids.empty() != /* XOR */
                      (bucket.scalarId.first == value::TypeTags::Nothing));

            invariant(inputData.empty() ||
                      (bucket.ids.size() == inputData[0].ids.size() &&
                       bucket.dataBlocks.size() == inputData[0].dataBlocks.size()));
        }

        size_t keySize = 0;
        size_t numScanSlots = 3;  // Default.

        if (!inputData.empty()) {
            keySize = inputData[0].ids.size() ? inputData[0].ids.size() : 1;
            numScanSlots = keySize + 1 /* bitset */ + inputData[0].dataBlocks.size();
        }

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

            BlockAggExprTupleVector aggs;

            size_t scanSlotIdx = keySize + 1;
            for (const auto& [blockAcc, rowAcc, mergeAcc] : accNames) {
                auto outputSlot = generateSlotId();
                std::unique_ptr<EExpression> blockAccFunc;
                std::unique_ptr<EExpression> rowAccFunc;

                if (blockAcc == "valueBlockAggCount") {
                    // valueBlockAggCount is the exception - it takes just the bitset.
                    blockAccFunc = makeFunction(blockAcc, makeE<EVariable>(accumulatorBitset));
                    rowAccFunc = makeFunction(rowAcc);
                } else {
                    dataInSlots.push_back(scanSlots[scanSlotIdx++]);

                    auto internalSlot = generateSlotId();
                    accDataSlots.push_back(internalSlot);

                    blockAccFunc = makeFunction(blockAcc,
                                                makeE<EVariable>(accumulatorBitset),
                                                makeE<EVariable>(internalSlot));
                    rowAccFunc = makeFunction(rowAcc, makeE<EVariable>(internalSlot));
                }

                aggs.emplace_back(
                    outputSlot,
                    BlockAggExprTuple{nullptr, std::move(blockAccFunc), std::move(rowAccFunc)});

                outputSlots.push_back(outputSlot);

                auto mergeInternalSlot = generateSlotId();
                mergingExprs.emplace_back(
                    mergeInternalSlot, makeFunction(mergeAcc, makeE<EVariable>(mergeInternalSlot)));
            }

            auto outStage = makeS<BlockHashAggStage>(std::move(scanStage),
                                                     idSlots,
                                                     bitsetInSlot,
                                                     dataInSlots,
                                                     accDataSlots,
                                                     accumulatorBitset,
                                                     std::move(aggs),
                                                     allowDiskUse,
                                                     std::move(mergingExprs),
                                                     nullptr /* yieldPolicy */,
                                                     kEmptyPlanNodeId,
                                                     true,
                                                     forceIncreasedSpilling);
            return std::make_pair(outputSlots, std::move(outStage));
        };

        // Convert the bucket to an array that will be consumed.
        std::vector<TypedValue> bucketVals;
        for (auto& bucket : inputData) {
            auto [arrTag, arrVal] = value::makeNewArray();
            value::Array* arr = value::getArrayView(arrVal);

            // Append groupby keys.
            if (!bucket.ids.empty()) {
                for (auto& id : bucket.ids) {
                    auto clone = id->clone();
                    arr->push_back(value::TypeTags::valueBlock,
                                   value::bitcastFrom<value::ValueBlock*>(clone.release()));
                }
            } else {
                auto [cpTag, cpVal] =
                    value::copyValue(bucket.scalarId.first, bucket.scalarId.second);
                arr->push_back(cpTag, cpVal);
            }

            // Append corresponding bitset.
            auto bitsetBlock = makeBoolBlock(bucket.bitset);
            arr->push_back({sbe::value::TypeTags::valueBlock,
                            value::bitcastFrom<value::ValueBlock*>(bitsetBlock.release())});

            for (const auto& block : bucket.dataBlocks) {
                auto clone = block->clone();
                arr->push_back(value::TypeTags::valueBlock,
                               value::bitcastFrom<value::ValueBlock*>(clone.release()));
            }

            bucketVals.push_back(std::pair(arrTag, arrVal));
        }

        TypedValue inputDataArray = makeArray(std::move(bucketVals));

        // Test forceSpill when allowDiskUse is set to true and forceIncreasedSpilling is set to
        // false.
        bool forceSpill = allowDiskUse && !forceIncreasedSpilling;
        auto result = runTestMulti(numScanSlots,
                                   inputDataArray.first,
                                   inputDataArray.second,
                                   makeFn,
                                   forceSpill,
                                   assertStageStats);
        value::ValueGuard resultGuard{result};
        assertResultMatchesMap(result, expectedResultsMap, expectedOutputBlockSizes);
    }

    static inline const AssertStageStatsFn assertPeakMemGreaterThanZero =
        [](const SpecificStats* statsGeneric) {
            const auto* stats = static_cast<const BlockHashAggStats*>(statsGeneric);
            ASSERT_NE(stats, nullptr);
            ASSERT_GT(stats->peakTrackedMemBytes, 0);
        };

    static inline const AssertStageStatsFn assertPeakMemIsZero =
        [](const SpecificStats* statsGeneric) {
            const auto* stats = static_cast<const BlockHashAggStats*>(statsGeneric);
            ASSERT_NE(stats, nullptr);
            ASSERT_EQ(stats->peakTrackedMemBytes, 0);
        };

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
     *   inputData - A vector of "Buckets," each of which represents a tuple:
     *         - ids is the vector of groupby-id blocks.
     *         - bitset is the selectivity bitset, indicating which values to keep/ignore.
     *         - dataBlocks is a vector of blocks used by the groupby accumulators.
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
     *
     *  assertNoSpillStats - Function to check stage stats at the conclusion of the test for a test
     *     where allowDiskUse is set to false.
     *
     * assertFirstBatchSpillingStats - Function to check stage stats at the conclusion of the test
     *     for a test where allowDiskUse is set to false and forcedIncreaseSpilling is true.
     *
     * assertForceSpillingStats - Function to check stage stats at the conclusion of the test
     *     for a test where allowDiskUse is set to false and forcedIncreaseSpilling is false.
     */
    void runBlockHashAggTest(
        const std::vector<Bucket>& buckets,
        AccNamesVector accNames,
        TestResultType expectedResultsMap,
        std::vector<size_t> expectedOutputBlockSizes,
        const AssertStageStatsFn& assertNoSpillStats = assertPeakMemGreaterThanZero,
        const AssertStageStatsFn& assertFirstBatchSpillingStats = assertPeakMemIsZero,
        const AssertStageStatsFn& assertForceSpillingStats = AssertStageStatsFn{}) {

        runBlockHashAggTestHelper(buckets,
                                  accNames,
                                  expectedResultsMap,
                                  expectedOutputBlockSizes,
                                  false /* allowDiskUse */,
                                  false /* forceIncreasedSpilling */,
                                  assertNoSpillStats);
        runBlockHashAggTestHelper(buckets,
                                  accNames,
                                  expectedResultsMap,
                                  expectedOutputBlockSizes,
                                  true /* allowDiskUse */,
                                  true /* forceIncreasedSpilling */,
                                  assertFirstBatchSpillingStats);
        runBlockHashAggTestHelper(buckets,
                                  accNames,
                                  expectedResultsMap,
                                  expectedOutputBlockSizes,
                                  true /* allowDiskUse */,
                                  false /* forceIncreasedSpilling */,
                                  assertForceSpillingStats);

    }  // runBlockHashAggTest

private:
    std::unique_ptr<Lock::GlobalLock> _globalLock;
};

TEST_F(BlockHashAggStageTest, NoData) {
    std::vector<Bucket> buckets;
    // We should have an empty block with no data.
    TestResultType expected = {};
    runBlockHashAggTest(
        buckets, {{"valueBlockAggMin", "min", "min"}}, expected, {}, assertPeakMemIsZero);
}

TEST_F(BlockHashAggStageTest, AllDataFiltered) {
    std::vector<Bucket> buckets = {Bucket{.ids = {makeInt32sBlock({0, 1, 2})},
                                          .bitset = {false, false, false},
                                          .dataBlocks = {makeInt32sBlock({50, 20, 30})}}};
    // We should have an empty block with no data.
    TestResultType expected = {};
    runBlockHashAggTest(
        buckets, {{"valueBlockAggMin", "min", "min"}}, expected, {}, assertPeakMemIsZero);
}

TEST_F(BlockHashAggStageTest, ScalarKeySingleAccumulatorMin) {
    // Each entry is ID followed by bitset followed by a block of data. For example
    // [groupid, [block bitset values], [block data values]]

    std::vector<Bucket> buckets = {Bucket{.scalarId = makeInt32(0),
                                          .bitset = {true, true, false},
                                          .dataBlocks = {makeInt32sBlock({50, 20, 30})}},
                                   Bucket{.scalarId = makeInt32(2),
                                          .bitset = {false, true, true},
                                          .dataBlocks = {makeInt32sBlock({40, 30, 60})}},
                                   Bucket{.scalarId = makeInt32(1),
                                          .bitset = {true, true, true},
                                          .dataBlocks = {makeInt32sBlock({70, 80, 10})}},
                                   Bucket{.scalarId = makeInt32(2),
                                          .bitset = {false, false, false},
                                          .dataBlocks = {makeInt32sBlock({10, 20, 30})}},
                                   Bucket{.scalarId = makeInt32(2),
                                          .bitset = {true, false, true},
                                          .dataBlocks = {makeInt32sBlock({30, 40, 50})}}};
    /*
     * 0 -> min(50, 20) = 20
     * 1 -> min(70, 80, 10) = 10
     * 2 -> min(30, 60, 30, 50) = 30
     */
    TestResultType expected = {{{0}, {20}}, {{1}, {10}}, {{2}, {30}}};
    runBlockHashAggTest(buckets, {{"valueBlockAggMin", "min", "min"}}, expected, {3});
}

TEST_F(BlockHashAggStageTest, ScalarKeySingleAccumulatorMinForceSpill) {
    auto assertFirstBatchSpillingStageStats = [](const SpecificStats* statsGeneric) {
        const auto* stats = dynamic_cast<const BlockHashAggStats*>(statsGeneric);
        ASSERT_NE(stats, nullptr);
        ASSERT_TRUE(stats->usedDisk);
        ASSERT_EQ(stats->spillingStats.getSpills(), BlockHashAggStage::kBlockOutSize * 5);
        ASSERT_EQ(stats->spillingStats.getSpilledRecords(), BlockHashAggStage::kBlockOutSize * 5);
        ASSERT_EQ(stats->spillingStats.getSpilledBytes(),
                  14 * BlockHashAggStage::kBlockOutSize * 5);
        // Since forceIncreasedSpilling is true, we spill without using any memory.
        ASSERT_EQ(stats->peakTrackedMemBytes, 0);
    };
    auto assertForceSpillingStageStats = [](const SpecificStats* statsGeneric) {
        const auto* stats = dynamic_cast<const BlockHashAggStats*>(statsGeneric);
        ASSERT_NE(stats, nullptr);
        ASSERT_TRUE(stats->usedDisk);
        ASSERT_EQ(stats->spillingStats.getSpills(), 1);
        ASSERT_EQ(stats->spillingStats.getSpilledRecords(),
                  BlockHashAggStage::kBlockOutSize *
                      2);  // It spills 2 blocks because it spills after the 3rd out of 5 blocks has
                           // been consumed.
        ASSERT_EQ(stats->spillingStats.getSpilledBytes(),
                  14 * BlockHashAggStage::kBlockOutSize * 2);
        ASSERT_GT(stats->peakTrackedMemBytes, 0);
    };

    TestResultType expected;
    auto addToExpected = [&expected](int32_t id, bool exists, int32_t data) {
        if (exists) {
            expected.emplace(std::vector<int32_t>{id}, std::vector<int32_t>{0});
            expected[std::vector<int32_t>{id}][0] += data;
        }
    };

    std::vector<Bucket> buckets;
    std::vector<size_t> expectedOutputBlockSizes(5, BlockHashAggStage::kBlockOutSize);

    // Create kBlockOutSize * 5 group ids, so that the output is 5 blocks of size kBlockOutSize.
    for (size_t id = 0; id < BlockHashAggStage::kBlockOutSize * 5; ++id) {
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
        buckets.push_back(Bucket{.ids = {makeInt32sBlock(ids)},
                                 .bitset = bitmap,
                                 .dataBlocks = {makeInt32sBlock(data)}});
    }

    runBlockHashAggTest(buckets,
                        {{"valueBlockAggSum", "sum", "sum"}},
                        expected,
                        expectedOutputBlockSizes,
                        assertPeakMemGreaterThanZero,
                        assertFirstBatchSpillingStageStats,
                        assertForceSpillingStageStats);
}

TEST_F(BlockHashAggStageTest, ScalarKeyCount) {
    // Each entry is ID followed by a bitset.
    std::vector<Bucket> buckets = {Bucket{.scalarId = makeInt32(0), .bitset = {true, true, true}},
                                   Bucket{.scalarId = makeInt32(0), .bitset = {true, false, true}},
                                   Bucket{.scalarId = makeInt32(1), .bitset = {true, false, true}},
                                   Bucket{.scalarId = makeInt32(1), .bitset = {true, true, false}}};
    TestResultType expected = {{{0}, {5}}, {{1}, {4}}};
    runBlockHashAggTest(buckets, {{"valueBlockAggCount", "count", "sum"}}, expected, {2});
}

TEST_F(BlockHashAggStageTest, ScalarKeySum) {
    // Each entry is ID followed by bitset followed by a block of data.
    std::vector<Bucket> buckets{Bucket{.scalarId = makeInt32(0),
                                       .bitset = {true, true, false},
                                       .dataBlocks = {makeInt32sBlock({1, 2, 3})}},
                                Bucket{.scalarId = makeInt32(2),
                                       .bitset = {false, true, true},
                                       .dataBlocks = {makeInt32sBlock({4, 5, 6})}},
                                Bucket{.scalarId = makeInt32(1),
                                       .bitset = {true, true, true},
                                       .dataBlocks = {makeInt32sBlock({7, 8, 9})}},
                                Bucket{.scalarId = makeInt32(2),
                                       .bitset = {false, false, false},
                                       .dataBlocks = {makeInt32sBlock({10, 11, 12})}},
                                Bucket{.scalarId = makeInt32(2),
                                       .bitset = {true, false, true},
                                       .dataBlocks = {makeInt32sBlock({13, 14, 15})}}};


    /*
     * 0 -> 1+2 = 3
     * 1 -> 7+8+9 = 24
     * 2 -> 5+6+13+15 = 39
     */
    TestResultType expected = {{{0}, {3}}, {{1}, {24}}, {{2}, {39}}};
    runBlockHashAggTest(buckets, {{"valueBlockAggSum", "sum", "sum"}}, expected, {3});
}

TEST_F(BlockHashAggStageTest, ScalarKeyMultipleAccumulators) {
    std::vector<Bucket> buckets = {
        Bucket{.scalarId = makeInt32(100),
               .bitset = {true, true, false},
               .dataBlocks = {makeInt32sBlock({200, 100, 150}), makeInt32sBlock({2, 4, 7})}},
        Bucket{.scalarId = makeInt32(100),
               .bitset = {false, true, true},
               .dataBlocks = {makeInt32sBlock({50, 90, 60}), makeInt32sBlock({-100, 20, 3})}},
        Bucket{.scalarId = makeInt32(50),
               .bitset = {true, true, true},
               .dataBlocks = {makeInt32sBlock({200, 100, 150}), makeInt32sBlock({-150, 150, 20})}},
        Bucket{.scalarId = makeInt32(25),
               .bitset = {true, false, false},
               .dataBlocks = {makeInt32sBlock({20, 75, 10}), makeInt32sBlock({0, 20, -20})}},
        Bucket{.scalarId = makeInt32(50),
               .bitset = {true, false, true},
               .dataBlocks = {makeInt32sBlock({75, 75, 75}), makeInt32sBlock({-2, 5, 8})}}};

    /*
     * 25  -> min(20) = 20, count=1, min(0) = 0
     * 50  -> min(200, 100, 150, 75, 75) = 75, count = 5, min(-150, 150, 20, -2, 8) = -150
     * 100 -> min(200, 100, 90, 60) = 60, count = 4, min(2, 4, 20, 3) = 2
     */
    TestResultType expected = {{{25}, {20, 1, 0}}, {{50}, {75, 5, -150}}, {{100}, {60, 4, 2}}};
    runBlockHashAggTest(buckets,
                        {{"valueBlockAggMin", "min", "min"},
                         {"valueBlockAggCount", "count", "sum"},
                         {"valueBlockAggMin", "min", "min"}},
                        expected,
                        {3});
}

TEST_F(BlockHashAggStageTest, Count) {
    std::vector<Bucket> buckets = {
        Bucket{.ids = {makeInt32sBlock({99, 101, 99})}, .bitset = {true, true, true}},
        Bucket{.ids = {makeInt32sBlock({99, 99, 101})}, .bitset = {true, false, true}},
        Bucket{.ids = {makeInt32sBlock({99, 101, 101})}, .bitset = {true, false, true}},
        Bucket{.ids = {makeInt32sBlock({101, 101, 99})}, .bitset = {true, true, false}}};

    TestResultType expected = {{{99}, {4}}, {{101}, {5}}};
    runBlockHashAggTest(buckets, {{"valueBlockAggCount", "count", "sum"}}, expected, {2});
}

TEST_F(BlockHashAggStageTest, CountWithMonoBlockKeys) {
    std::vector<Bucket> buckets{
        Bucket{
            .ids = {makeMonoBlock(makeInt32(99), 3)},  // groupBys
            .bitset = {true, true, true}               // bitset
        },
        Bucket{.ids = {makeMonoBlock(makeInt32(101), 3)}, .bitset = {true, false, true}},
        Bucket{.ids = {makeInt32sBlock({99, 101, 101})}, .bitset = {true, false, true}},
        Bucket{.ids = {makeInt32sBlock({101, 101, 99})}, .bitset = {true, true, false}},
    };

    TestResultType expected = {{{99}, {4}}, {{101}, {5}}};
    runBlockHashAggTest(buckets, {{"valueBlockAggCount", "count", "sum"}}, expected, {2});
}

TEST_F(BlockHashAggStageTest, CountWithMonoBlockKeysCompound) {
    std::vector<Bucket> buckets = {
        Bucket{.ids = {makeMonoBlock(makeInt32(99), 3), makeInt32sBlock({1000, 1001, 1000})},
               .bitset = {true, true, true}},
        Bucket{.ids = {makeMonoBlock(makeInt32(101), 3), makeMonoBlock(makeInt32(1000), 3)},
               .bitset = {true, false, true}},
        Bucket{.ids = {makeInt32sBlock({99, 101, 101}), makeMonoBlock(makeInt32(1001), 3)},
               .bitset = {true, false, true}},
        Bucket{.ids = {makeInt32sBlock({101, 101, 99}), makeInt32sBlock({1000, 1000, 1001})},
               .bitset = {true, true, false}}};
    TestResultType expected = {
        {{99, 1000}, {2}}, {{99, 1001}, {2}}, {{101, 1000}, {4}}, {{101, 1001}, {1}}};
    runBlockHashAggTest(buckets, {{"valueBlockAggCount", "count", "sum"}}, expected, {4}
                        // Expected output block sizes.
    );
}

TEST_F(BlockHashAggStageTest, SumBlockGroupByKey) {
    // Each entry is ID followed by bitset followed by a block of data.
    std::vector<Bucket> buckets = {Bucket{.ids = {makeInt32sBlock({0, 0, 0})},
                                          .bitset = {true, true, false},
                                          .dataBlocks = {makeInt32sBlock({1, 2, 3})}},
                                   Bucket{.ids = {makeInt32sBlock({2, 2, 2})},
                                          .bitset = {false, true, true},
                                          .dataBlocks = {makeInt32sBlock({4, 5, 6})}},
                                   Bucket{.ids = {makeInt32sBlock({1, 1, 1})},
                                          .bitset = {true, true, true},
                                          .dataBlocks = {makeInt32sBlock({7, 8, 9})}},
                                   Bucket{.ids = {makeInt32sBlock({2, 2, 2})},
                                          .bitset = {false, false, false},
                                          .dataBlocks = {makeInt32sBlock({10, 11, 12})}},
                                   Bucket{.ids = {makeInt32sBlock({2, 2, 2})},
                                          .bitset = {true, false, true},
                                          .dataBlocks = {makeInt32sBlock({13, 14, 15})}}};

    /*
     * 0 -> 1+2 = 3
     * 1 -> 7+8+9 = 24
     * 2 -> 5+6+13+15 = 39
     */
    TestResultType expected = {{{0}, {3}}, {{1}, {24}}, {{2}, {39}}};
    runBlockHashAggTest(buckets, {{"valueBlockAggSum", "sum", "sum"}}, expected, {3});
}

// Similar to the test above, but we change the groupby keys so they are different within each
// block.
TEST_F(BlockHashAggStageTest, SumDifferentBlockGroupByKeys) {
    // Each entry is ID followed by bitset followed by a block of data.
    std::vector<Bucket> buckets = {Bucket{.ids = {makeInt32sBlock({1, 2, 3})},
                                          .bitset = {true, true, false},
                                          .dataBlocks = {makeInt32sBlock({1, 2, 3})}},
                                   Bucket{.ids = {makeInt32sBlock({2, 2, 2})},
                                          .bitset = {false, true, true},
                                          .dataBlocks = {makeInt32sBlock({4, 5, 6})}},
                                   Bucket{.ids = {makeInt32sBlock({3, 2, 1})},
                                          .bitset = {true, true, true},
                                          .dataBlocks = {makeInt32sBlock({7, 8, 9})}},
                                   Bucket{.ids = {makeInt32sBlock({2, 3, 4})},
                                          .bitset = {false, true, true},
                                          .dataBlocks = {makeInt32sBlock({10, 11, 12})}},
                                   Bucket{.ids = {makeInt32sBlock({2, 3, 4})},
                                          .bitset = {false, false, false},
                                          .dataBlocks = {makeInt32sBlock({0, 5, 4})}},
                                   Bucket{.ids = {makeInt32sBlock({1, 1, 2})},
                                          .bitset = {true, true, true},
                                          .dataBlocks = {makeInt32sBlock({13, 14, 15})}}};

    /*
     * 1 -> 1+9+13+14  = 37
     * 2 -> 2+5+6+8+15 = 36
     * 3 -> 7+11       = 18
     * 4 -> 12         = 12
     */
    TestResultType expected = {{{1}, {37}}, {{2}, {36}}, {{3}, {18}}, {{4}, {12}}};
    runBlockHashAggTest(buckets, {{"valueBlockAggSum", "sum", "sum"}}, expected, {4});
}

// Similar test as above but the "2" key appears in every block but is always false, so we make
// sure it's missing.
TEST_F(BlockHashAggStageTest, SumDifferentBlockGroupByKeysMissingKey) {
    // Each entry is ID followed by bitset followed by a block of data.
    // Mix high partition rows with low partition.
    std::vector<Bucket> buckets = {
        Bucket{.ids = {makeInt32sBlock({1, 2, 3, 5, 6, 7})},
               .bitset = {true, false, false, true, true, true},
               .dataBlocks = {makeInt32sBlock({1, 2, 3, 4, 5, 6})}},
        Bucket{.ids = {makeInt32sBlock({2, 2, 2})},
               .bitset = {false, false, false},
               .dataBlocks = {makeInt32sBlock({4, 5, 6})}},
        Bucket{.ids = {makeInt32sBlock({3, 2, 1, 7, 6, 5})},
               .bitset = {true, false, true, false, true, true},
               .dataBlocks = {makeInt32sBlock({7, 8, 9, 1, 2, 3})}},
        Bucket{.ids = {makeInt32sBlock({2, 3, 4, 6, 7, 5})},
               .bitset = {false, true, true, true, true, false},
               .dataBlocks = {makeInt32sBlock({10, 11, 12, 15, 15, 15})}},
        Bucket{.ids = {makeInt32sBlock({2, 3, 4})},
               .bitset = {false, false, false},
               .dataBlocks = {makeInt32sBlock({0, 5, 4})}},
        Bucket{.ids = {makeInt32sBlock({1, 1, 2})},
               .bitset = {true, true, false},
               .dataBlocks = {makeInt32sBlock({13, 14, 15})}}};

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
    runBlockHashAggTest(buckets, {{"valueBlockAggSum", "sum", "sum"}}, expected, {6});
}

TEST_F(BlockHashAggStageTest, MultipleAccumulatorsDifferentBlockGroupByKeys) {
    // Each entry is ID followed by bitset followed by block A and block B.
    std::vector<Bucket> buckets = {
        Bucket{.ids = {makeInt32sBlock({25, 50, 100})},
               .bitset = {true, true, false},
               .dataBlocks = {makeInt32sBlock({200, 100, 150}), makeInt32sBlock({2, 4, 7})}},
        Bucket{.ids = {makeInt32sBlock({50, 50, 50})},
               .bitset = {false, true, true},
               .dataBlocks = {makeInt32sBlock({50, 90, 60}), makeInt32sBlock({-100, 20, 3})}},
        Bucket{.ids = {makeInt32sBlock({25, 25, 100})},
               .bitset = {true, true, true},
               .dataBlocks = {makeInt32sBlock({200, 100, 150}), makeInt32sBlock({-150, 150, 2})}},
        Bucket{.ids = {makeInt32sBlock({100, 50, 25})},
               .bitset = {true, false, false},
               .dataBlocks = {makeInt32sBlock({20, 75, 10}), makeInt32sBlock({0, 20, -20})}},
        Bucket{.ids = {makeInt32sBlock({100, 25, 50})},
               .bitset = {true, false, true},
               .dataBlocks = {makeInt32sBlock({75, 75, 75}), makeInt32sBlock({-2, 5, 8})}}};

    /*
     * 25  -> min(200, 200, 100) = 100, count = 3, min(2, -150, 150) = -150
     * 50  -> min(100, 90, 60, 75) = 60, count = 4, min(4, 20, 3, 8) = 3
     * 100 -> min(150, 20, 75) = 20, count = 3, min(20, 0, -2) = -2
     */
    TestResultType expected = {{{25}, {100, 3, -150}}, {{50}, {60, 4, 3}}, {{100}, {20, 3, -2}}};
    runBlockHashAggTest(buckets,
                        {{"valueBlockAggMin", "min", "min"},
                         {"valueBlockAggCount", "count", "sum"},
                         {"valueBlockAggMin", "min", "min"}},
                        expected,
                        {3});
}

TEST_F(BlockHashAggStageTest, CountCompoundKey) {
    // Each entry is ID followed by a bitset.
    std::vector<Bucket> buckets = {
        Bucket{.ids = {makeInt32sBlock({0, 1, 0}), makeInt32sBlock({0, 0, 1})},
               .bitset = {true, true, true}},
        Bucket{.ids = {makeInt32sBlock({0, 0, 1}), makeInt32sBlock({0, 1, 0})},
               .bitset = {true, false, true}},
        Bucket{.ids = {makeInt32sBlock({0, 1, 1}), makeInt32sBlock({1, 0, 1})},
               .bitset = {true, false, true}},
        Bucket{.ids = {makeInt32sBlock({1, 1, 0}), makeInt32sBlock({1, 1, 1})},
               .bitset = {true, true, false}}};
    TestResultType expected = {{{0, 0}, {2}}, {{0, 1}, {2}}, {{1, 0}, {2}}, {{1, 1}, {3}}};
    runBlockHashAggTest(buckets, {{"valueBlockAggCount", "count", "sum"}}, expected, {4});
}

// TODO SERVER-85731 Revisit input sizes if kMaxNumPartitionsForTokenizedPath changes.
TEST_F(BlockHashAggStageTest, SumCompoundKeysMissingKey) {
    // Each entry is ID followed by bitset followed by a block of data.
    // Mix high partition rows with low partition.
    std::vector<Bucket> buckets = {
        Bucket{.ids = {makeInt32sBlock({1, 2, 3, 5, 6, 7}), makeInt32sBlock({2, 3, 4, 6, 7, 8})},
               .bitset = {true, false, false, true, true, true},
               .dataBlocks = {makeInt32sBlock({1, 2, 3, 4, 5, 6})}},
        Bucket{.ids = {makeInt32sBlock({2, 2, 2}), makeInt32sBlock({3, 3, 3})},
               .bitset = {false, false, false},
               .dataBlocks = {makeInt32sBlock({4, 5, 6})}},
        Bucket{.ids = {makeInt32sBlock({3, 2, 1, 7, 6, 5}), makeInt32sBlock({4, 3, 2, 8, 7, 6})},
               .bitset = {true, false, true, false, true, true},
               .dataBlocks = {makeInt32sBlock({7, 8, 9, 1, 2, 3})}},
        Bucket{.ids = {makeInt32sBlock({2, 3, 4, 6, 7, 5}), makeInt32sBlock({3, 4, 5, 7, 8, 6})},
               .bitset = {false, true, true, true, true, false},
               .dataBlocks = {makeInt32sBlock({10, 11, 12, 15, 15, 15})}},
        Bucket{.ids = {makeInt32sBlock({2, 3, 4}), makeInt32sBlock({3, 4, 5})},
               .bitset = {false, false, false},
               .dataBlocks = {makeInt32sBlock({0, 5, 4})}},
        Bucket{.ids = {makeInt32sBlock({1, 1, 2}), makeInt32sBlock({2, 2, 3})},
               .bitset = {true, true, false},
               .dataBlocks = {makeInt32sBlock({13, 14, 15})}}};

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
    runBlockHashAggTest(buckets, {{"valueBlockAggSum", "sum", "sum"}}, expected, {6});
}

// TODO SERVER-85731 Revisit input sizes if kMaxNumPartitionsForTokenizedPath changes.
// Tests that our bailout when a single tokenized block has high cardinality works correctly.
TEST_F(BlockHashAggStageTest, HighPartitionSizeTokenizeBailoutTest) {
    // Each entry is ID followed by bitset followed by a block of data.
    // Mix high partition rows with low partition.
    std::vector<Bucket> buckets = {
        // First ID block is low cardinality, second is high.
        Bucket{.ids = {makeInt32sBlock({1, 2, 1, 2, 1, 2}), makeInt32sBlock({8, 7, 6, 5, 4, 3})},
               .bitset = {true, true, true, true, true, true},
               .dataBlocks = {makeInt32sBlock({7, 8, 9, 10, 11, 12})}},
        // Both ID blocks are high cardinality.
        Bucket{.ids = {makeInt32sBlock({8, 7, 6, 5, 4, 3}), makeInt32sBlock({1, 2, 3, 4, 5, 6})},
               .bitset = {true, true, true, true, true, true},
               .dataBlocks = {makeInt32sBlock({19, 20, 21, 22, 23, 24})}}};

    /*
     * Every key ends up having a unique value.
     * {1, 8} -> 7
     * {2, 7} -> 8
     * {1, 6} -> 9
     * {2, 5} -> 10
     * {1, 4} -> 11
     * {2, 3} -> 12
     * {8, 1} -> 19
     * {7, 2} -> 20
     * {6, 3} -> 21
     * {5, 4} -> 22
     * {4, 5} -> 23
     * {3, 6} -> 24
     */
    TestResultType expected = {{{1, 8}, {7}},
                               {{2, 7}, {8}},
                               {{1, 6}, {9}},
                               {{2, 5}, {10}},
                               {{1, 4}, {11}},
                               {{2, 3}, {12}},
                               {{8, 1}, {19}},
                               {{7, 2}, {20}},
                               {{6, 3}, {21}},
                               {{5, 4}, {22}},
                               {{4, 5}, {23}},
                               {{3, 6}, {24}}};
    runBlockHashAggTest(buckets, {{"valueBlockAggSum", "sum", "sum"}}, expected, {12});
}

TEST_F(BlockHashAggStageTest, BlockOutSizeTest) {
    TestResultType expected;
    auto addToExpected = [&expected](int32_t id, bool exists, int32_t data) {
        if (exists) {
            expected.emplace(std::vector<int32_t>{id}, std::vector<int32_t>{0});
            expected[std::vector<int32_t>{id}][0] += data;
        }
    };

    std::vector<Bucket> buckets;

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
        buckets.push_back(Bucket{.ids = {makeInt32sBlock(ids)},
                                 .bitset = bitmap,
                                 .dataBlocks = {makeInt32sBlock(data)}});
    }

    runBlockHashAggTest(buckets,
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

    std::vector<Bucket> buckets;
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
        std::vector<CopyableValueBlock> expectedKeys(keySize);
        for (size_t idIdx = 0; idIdx < keySize; ++idIdx) {
            expectedKeys[idIdx] = makeInt32sBlock(ids[idIdx]);
        }
        buckets.push_back(Bucket{.ids = expectedKeys,
                                 .bitset = bitmap,
                                 .dataBlocks = {makeInt32sBlock(data1), makeInt32sBlock(data2)}});
    }

    runBlockHashAggTest(buckets,
                        {{"valueBlockAggSum", "sum", "sum"},
                         {"valueBlockAggCount", "count", "sum"},
                         {"valueBlockAggMin", "min", "min"}},
                        expected,
                        {highPartitionSize - lowPartitionSize + 2});
}
}  // namespace mongo::sbe
