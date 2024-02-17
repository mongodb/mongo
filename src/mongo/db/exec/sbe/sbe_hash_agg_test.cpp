/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

/**
 * This file contains tests for sbe::HashAggStage.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/record_id.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/scopeguard.h"

namespace mongo::sbe {

class HashAggStageTest : public PlanStageTestFixture {
public:
    void setUp() override {
        PlanStageTestFixture::setUp();
        _globalLock = std::make_unique<Lock::GlobalLock>(operationContext(), MODE_IS);
    }

    void tearDown() override {
        _globalLock.reset();
        PlanStageTestFixture::tearDown();
    }

    void performHashAggWithSpillChecking(
        BSONArray inputArr,
        BSONArray expectedOutputArray,
        bool shouldSpill = false,
        std::unique_ptr<mongo::CollatorInterfaceMock> optionalCollator = nullptr);

private:
    std::unique_ptr<Lock::GlobalLock> _globalLock;
};

void HashAggStageTest::performHashAggWithSpillChecking(
    BSONArray inputArr,
    BSONArray expectedOutputArray,
    bool shouldSpill,
    std::unique_ptr<mongo::CollatorInterfaceMock> optionalCollator) {
    using namespace std::literals;

    auto [inputTag, inputVal] = stage_builder::makeValue(inputArr);
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [expectedTag, expectedVal] = stage_builder::makeValue(expectedOutputArray);
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto collatorSlot = generateSlotId();
    auto shouldUseCollator = optionalCollator != nullptr;

    auto makeStageFn = [this, collatorSlot, shouldUseCollator, shouldSpill](
                           value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        auto countsSlot = generateSlotId();
        auto spillSlot = generateSlotId();

        auto hashAggStage = makeS<HashAggStage>(
            std::move(scanStage),
            makeSV(scanSlot),
            makeAggExprVector(
                countsSlot,
                nullptr,
                stage_builder::makeFunction("sum",
                                            makeE<EConstant>(value::TypeTags::NumberInt64,
                                                             value::bitcastFrom<int64_t>(1)))),
            makeSV(),
            true,
            boost::optional<value::SlotId>{shouldUseCollator, collatorSlot},
            shouldSpill,
            makeSlotExprPairVec(
                spillSlot,
                stage_builder::makeFunction("sum", stage_builder::makeVariable(spillSlot))),
            nullptr /* yieldPolicy */,
            kEmptyPlanNodeId);

        return std::make_pair(countsSlot, std::move(hashAggStage));
    };

    auto ctx = makeCompileCtx();

    value::OwnedValueAccessor collatorAccessor;
    if (shouldUseCollator) {
        ctx->pushCorrelated(collatorSlot, &collatorAccessor);
        collatorAccessor.reset(value::TypeTags::collator,
                               value::bitcastFrom<CollatorInterface*>(optionalCollator.release()));
    }

    // Generate a mock scan from 'input' with a single output slot.
    inputGuard.reset();
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    auto [outputSlot, stage] = makeStageFn(scanSlot, std::move(scanStage));
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), outputSlot);

    // Get all the results produced.
    auto [resultsTag, resultsVal] = getAllResults(stage.get(), resultAccessor);
    value::ValueGuard resultsGuard{resultsTag, resultsVal};

    // Sort results for stable compare, since the counts could come out in any order.
    using ValuePair = std::pair<value::TypeTags, value::Value>;
    std::vector<ValuePair> resultsContents;
    auto resultsView = value::getArrayView(resultsVal);
    for (size_t i = 0; i < resultsView->size(); i++) {
        resultsContents.push_back(resultsView->getAt(i));
    }

    // Sort 'resultContents' in descending order.
    std::sort(resultsContents.begin(),
              resultsContents.end(),
              [](const ValuePair& lhs, const ValuePair& rhs) -> bool {
                  auto [lhsTag, lhsVal] = lhs;
                  auto [rhsTag, rhsVal] = rhs;
                  auto [compareTag, compareVal] =
                      value::compareValue(lhsTag, lhsVal, rhsTag, rhsVal);
                  ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
                  return value::bitcastTo<int32_t>(compareVal) > 0;
              });

    auto [sortedResultsTag, sortedResultsVal] = value::makeNewArray();
    value::ValueGuard sortedResultsGuard{sortedResultsTag, sortedResultsVal};
    auto sortedResultsView = value::getArrayView(sortedResultsVal);
    for (auto [tag, val] : resultsContents) {
        auto [tagCopy, valCopy] = copyValue(tag, val);
        sortedResultsView->push_back(tagCopy, valCopy);
    }

    assertValuesEqual(sortedResultsTag, sortedResultsVal, expectedTag, expectedVal);
};

TEST_F(HashAggStageTest, HashAggMinMaxTest) {
    using namespace std::literals;

    BSONArrayBuilder bab1;
    bab1.append("D").append("a").append("F").append("e").append("B").append("c");
    auto [inputTag, inputVal] = stage_builder::makeValue(bab1.arr());
    value::ValueGuard inputGuard{inputTag, inputVal};

    BSONArrayBuilder bab2;
    bab2.append("B").append("e").append("a").append("F");
    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(bab2.arr()));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);

    auto makeStageFn = [this, &collator](value::SlotId scanSlot,
                                         std::unique_ptr<PlanStage> scanStage) {
        auto collExpr = makeE<EConstant>(
            value::TypeTags::collator, value::bitcastFrom<CollatorInterface*>(collator.release()));

        // Build a HashAggStage that exercises the collMin() and collMax() aggregate functions.
        auto minSlot = generateSlotId();
        auto maxSlot = generateSlotId();
        auto collMinSlot = generateSlotId();
        auto collMaxSlot = generateSlotId();
        auto hashAggStage = makeS<HashAggStage>(
            std::move(scanStage),
            makeSV(),
            makeAggExprVector(minSlot,
                              nullptr,
                              stage_builder::makeFunction("min", makeE<EVariable>(scanSlot)),
                              maxSlot,
                              nullptr,
                              stage_builder::makeFunction("max", makeE<EVariable>(scanSlot)),
                              collMinSlot,
                              nullptr,
                              stage_builder::makeFunction(
                                  "collMin", collExpr->clone(), makeE<EVariable>(scanSlot)),
                              collMaxSlot,
                              nullptr,
                              stage_builder::makeFunction(
                                  "collMax", collExpr->clone(), makeE<EVariable>(scanSlot))),
            makeSV(),
            true,
            boost::none,
            false /* allowDiskUse */,
            makeSlotExprPairVec() /* mergingExprs */,
            nullptr /* yieldPolicy */,
            kEmptyPlanNodeId);

        auto outSlot = generateSlotId();
        auto projectStage =
            makeProjectStage(std::move(hashAggStage),
                             kEmptyPlanNodeId,
                             outSlot,
                             stage_builder::makeFunction("newArray",
                                                         makeE<EVariable>(minSlot),
                                                         makeE<EVariable>(maxSlot),
                                                         makeE<EVariable>(collMinSlot),
                                                         makeE<EVariable>(collMaxSlot)));

        return std::make_pair(outSlot, std::move(projectStage));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(HashAggStageTest, HashAggAddToSetTest) {
    using namespace std::literals;

    BSONArrayBuilder bab;
    bab.append("cc").append("BB").append("Aa").append("Bb").append("dD").append("aA");
    bab.append("CC").append("AA").append("Dd").append("cC").append("bb").append("DD");
    auto [inputTag, inputVal] = stage_builder::makeValue(bab.arr());
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [expectedTag, expectedVal] = value::makeNewArray();
    value::ValueGuard expectedGuard{expectedTag, expectedVal};
    for (auto&& sv : std::array<StringData, 4>{"Aa", "BB", "cc", "dD"}) {
        auto [tag, val] = value::makeNewString(sv);
        value::getArrayView(expectedVal)->push_back(tag, val);
    }

    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);

    auto makeStageFn = [this, &collator](value::SlotId scanSlot,
                                         std::unique_ptr<PlanStage> scanStage) {
        auto collExpr = makeE<EConstant>(
            value::TypeTags::collator, value::bitcastFrom<CollatorInterface*>(collator.release()));

        // Build a HashAggStage that exercises the collAddToSet() aggregate function.
        auto hashAggSlot = generateSlotId();
        auto hashAggStage = makeS<HashAggStage>(
            std::move(scanStage),
            makeSV(),
            makeAggExprVector(hashAggSlot,
                              nullptr,
                              stage_builder::makeFunction(
                                  "collAddToSet", std::move(collExpr), makeE<EVariable>(scanSlot))),
            makeSV(),
            true,
            boost::none,
            false /* allowDiskUse */,
            makeSlotExprPairVec() /* mergingExprs */,
            nullptr /* yieldPolicy */,
            kEmptyPlanNodeId);

        return std::make_pair(hashAggSlot, std::move(hashAggStage));
    };

    // Generate a mock scan from 'input' with a single output slot.
    inputGuard.reset();
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Call the 'makeStage' callback to create the PlanStage that we want to test, passing in
    // the mock scan subtree and its output slot.
    auto [outputSlot, stage] = makeStageFn(scanSlot, std::move(scanStage));

    // Prepare the tree and get the SlotAccessor for the output slot.
    auto ctx = makeCompileCtx();
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), outputSlot);

    // Get all the results produced by the PlanStage we want to test.
    auto [resultsTag, resultsVal] = getAllResults(stage.get(), resultAccessor);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    // Retrieve the first element from the results array.
    value::ArrayEnumerator resultsEnumerator{resultsTag, resultsVal};
    ASSERT_TRUE(!resultsEnumerator.atEnd());
    auto [elemTag, elemVal] = resultsEnumerator.getViewOfValue();

    // Convert the element into an ArraySet 'as' (with no collation).
    auto [asTag, asVal] = value::arrayToSet(elemTag, elemVal);
    value::ValueGuard asGuard{asTag, asVal};
    ASSERT_TRUE(asTag == value::TypeTags::ArraySet);

    // Assert that 'as' and 'expected' are the same size and contain the same values.
    auto as = value::getArraySetView(asVal);
    size_t expectedSize = 0;
    value::ArrayEnumerator expectedEnumerator{expectedTag, expectedVal};

    for (; !expectedEnumerator.atEnd(); expectedEnumerator.advance()) {
        ASSERT_TRUE(as->values().count(expectedEnumerator.getViewOfValue()));
        ++expectedSize;
    }

    ASSERT_TRUE(as->size() == expectedSize);

    // Assert that the results array does not contain more than one element.
    resultsEnumerator.advance();
    ASSERT_TRUE(resultsEnumerator.atEnd());
}

TEST_F(HashAggStageTest, HashAggCollationTest) {
    auto inputArr = BSON_ARRAY("A"
                               << "a"
                               << "b"
                               << "c"
                               << "B"
                               << "a");

    // Collator groups the values as: ["A", "a", "a"], ["B", "b"], ["c"].
    auto collatorExpectedOutputArr = BSON_ARRAY(3 << 2 << 1);
    auto lowerStringCollator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    performHashAggWithSpillChecking(
        inputArr, collatorExpectedOutputArr, false, std::move(lowerStringCollator));

    // No Collator groups the values as: ["a", "a"], ["A"], ["B"], ["b"], ["c"].
    auto nonCollatorExpectedOutputArr = BSON_ARRAY(2 << 1 << 1 << 1 << 1);
    performHashAggWithSpillChecking(inputArr, nonCollatorExpectedOutputArr);
}

TEST_F(HashAggStageTest, HashAggSeekKeysTest) {
    auto ctx = makeCompileCtx();

    // Create a seek slot we will use to peek into the hash table.
    auto seekSlot = generateSlotId();
    value::OwnedValueAccessor seekAccessor;
    ctx->pushCorrelated(seekSlot, &seekAccessor);

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(5 << 6 << 7 << 5 << 6 << 7 << 6 << 7 << 7));
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);


    auto [outputSlot, stage] = [this, seekSlot](value::SlotId scanSlot,
                                                std::unique_ptr<PlanStage> scanStage) {
        // Build a HashAggStage, group by the scanSlot and compute a simple count.
        auto countsSlot = generateSlotId();

        auto hashAggStage = makeS<HashAggStage>(
            std::move(scanStage),
            makeSV(scanSlot),
            makeAggExprVector(
                countsSlot,
                nullptr,
                stage_builder::makeFunction("sum",
                                            makeE<EConstant>(value::TypeTags::NumberInt64,
                                                             value::bitcastFrom<int64_t>(1)))),
            makeSV(seekSlot),
            true,
            boost::none,
            false /* allowDiskUse */,
            makeSlotExprPairVec() /* mergingExprs */,
            nullptr /* yieldPolicy */,
            kEmptyPlanNodeId);

        return std::make_pair(countsSlot, std::move(hashAggStage));
    }(scanSlot, std::move(scanStage));

    // Let's start with '5' as our seek value.
    seekAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int>(5));

    // Prepare the tree and get the 'SlotAccessor' for the output slot.
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), outputSlot);
    ctx->popCorrelated();

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res1Tag, res1Val] = resultAccessor->getViewOfValue();
    // There are '2' occurences of '5' in the input.
    assertValuesEqual(res1Tag, res1Val, value::TypeTags::NumberInt32, value::bitcastFrom<int>(2));
    ASSERT_TRUE(stage->getNext() == PlanState::IS_EOF);

    // Reposition to '6'.
    seekAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int>(6));
    stage->open(true);
    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res2Tag, res2Val] = resultAccessor->getViewOfValue();
    // There are '3' occurences of '6' in the input.
    assertValuesEqual(res2Tag, res2Val, value::TypeTags::NumberInt32, value::bitcastFrom<int>(3));
    ASSERT_TRUE(stage->getNext() == PlanState::IS_EOF);

    // Reposition to '7'.
    seekAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int>(7));
    stage->open(true);
    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res3Tag, res3Val] = resultAccessor->getViewOfValue();
    // There are '4' occurences of '7' in the input.
    assertValuesEqual(res3Tag, res3Val, value::TypeTags::NumberInt32, value::bitcastFrom<int>(4));
    ASSERT_TRUE(stage->getNext() == PlanState::IS_EOF);

    stage->close();
}

TEST_F(HashAggStageTest, HashAggBasicCountNoSpill) {
    // We shouldn't spill to disk if memory is plentiful (which by default it is), even if we are
    // allowed to.

    auto ctx = makeCompileCtx();

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(5 << 6 << 7 << 5 << 6 << 7 << 6 << 7 << 7));
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Build a HashAggStage, group by the scanSlot and compute a simple count.
    auto countsSlot = generateSlotId();
    auto spillSlot = generateSlotId();
    auto stage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(scanSlot),
        makeAggExprVector(
            countsSlot,
            nullptr,
            stage_builder::makeFunction(
                "sum",
                makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1)))),
        makeSV(),  // Seek slot
        true,
        boost::none,
        true /* allowDiskUse */,
        makeSlotExprPairVec(
            spillSlot, stage_builder::makeFunction("sum", stage_builder::makeVariable(spillSlot))),
        nullptr /* yieldPolicy */,
        kEmptyPlanNodeId);

    // Prepare the tree and get the 'SlotAccessor' for the output slot.
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), countsSlot);

    // Read in all of the results.
    std::set<int64_t> results;
    while (stage->getNext() == PlanState::ADVANCED) {
        auto [resTag, resVal] = resultAccessor->getViewOfValue();
        ASSERT_EQ(value::TypeTags::NumberInt64, resTag);
        ASSERT_TRUE(results.insert(value::bitcastFrom<int64_t>(resVal)).second);
    }

    // Check that the results match the expected.
    ASSERT_EQ(3, results.size());
    ASSERT_EQ(1, results.count(2));  // 2 of "5"s
    ASSERT_EQ(1, results.count(3));  // 3 of "6"s
    ASSERT_EQ(1, results.count(4));  // 4 of "7"s

    // Check that the spilling behavior matches the expected.
    auto stats = static_cast<const HashAggStats*>(stage->getSpecificStats());
    ASSERT_FALSE(stats->usedDisk);
    ASSERT_EQ(0, stats->spills);
    ASSERT_EQ(0, stats->spilledRecords);

    stage->close();
}

TEST_F(HashAggStageTest, HashAggBasicCountSpill) {
    // We estimate the size of result row like {int64, int64} at 50B. Set the memory threshold to
    // 64B so that exactly one row fits in memory.
    auto defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill =
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.load();
    internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(64);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(
            defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill);
    });

    auto ctx = makeCompileCtx();

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(5 << 6 << 7 << 5 << 6 << 7 << 6 << 7 << 7));
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Build a HashAggStage, group by the scanSlot and compute a simple count.
    auto countsSlot = generateSlotId();
    auto spillSlot = generateSlotId();
    auto stage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(scanSlot),
        makeAggExprVector(
            countsSlot,
            nullptr,
            stage_builder::makeFunction(
                "sum",
                makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1)))),
        makeSV(),  // Seek slot
        true,
        boost::none,
        true /* allowDiskUse */,
        makeSlotExprPairVec(
            spillSlot, stage_builder::makeFunction("sum", stage_builder::makeVariable(spillSlot))),
        nullptr /* yieldPolicy */,
        kEmptyPlanNodeId);

    // Prepare the tree and get the 'SlotAccessor' for the output slot.
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), countsSlot);

    // Read in all of the results.
    std::set<int64_t> results;
    while (stage->getNext() == PlanState::ADVANCED) {
        auto [resTag, resVal] = resultAccessor->getViewOfValue();
        ASSERT_EQ(value::TypeTags::NumberInt64, resTag);
        ASSERT_TRUE(results.insert(value::bitcastFrom<int64_t>(resVal)).second);
    }

    // Check that the results match the expected.
    ASSERT_EQ(3, results.size());
    ASSERT_EQ(1, results.count(2));  // 2 of "5"s
    ASSERT_EQ(1, results.count(3));  // 3 of "6"s
    ASSERT_EQ(1, results.count(4));  // 4 of "7"s

    // Check that the spilling behavior matches the expected.
    auto stats = static_cast<const HashAggStats*>(stage->getSpecificStats());
    ASSERT_TRUE(stats->usedDisk);
    // Memory usage is estimated only every two rows at the most frequent. Also, we only start
    // spilling after estimating that the memory budget is exceeded. These two factors result in
    // fewer expected spills than there are input records, even though only one record fits in
    // memory at a time.
    ASSERT_EQ(stats->spills, 3);
    // The input has one run of two consecutive values, so we expect to spill as many records as
    // there are input values minus one.
    ASSERT_EQ(stats->spilledRecords, 8);

    stage->close();
}

TEST_F(HashAggStageTest, HashAggBasicCountNoSpillIfNoMemCheck) {
    // We estimate the size of result row like {int64, int64} at 50B. Set the memory threshold to
    // 64B so that exactly one row fits in memory and spill would be required. At the same time, set
    // the memory check bounds to exceed the number of processed records so the checks are never run
    // and the need to spill is never discovered.
    auto defaultMemoryLimit = internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.load();
    internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(64);

    auto defaultAtMost = internalQuerySBEAggMemoryCheckPerAdvanceAtMost.load();
    internalQuerySBEAggMemoryCheckPerAdvanceAtMost.store(100);

    auto defaultAtLeast = internalQuerySBEAggMemoryCheckPerAdvanceAtLeast.load();
    internalQuerySBEAggMemoryCheckPerAdvanceAtLeast.store(100);

    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(defaultMemoryLimit);
        internalQuerySBEAggMemoryCheckPerAdvanceAtMost.store(defaultAtMost);
        internalQuerySBEAggMemoryCheckPerAdvanceAtLeast.store(defaultAtLeast);
    });

    auto ctx = makeCompileCtx();

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(5 << 6 << 7 << 5 << 6 << 7 << 6 << 7 << 7));
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Build a HashAggStage, group by the scanSlot and compute a simple count.
    auto countsSlot = generateSlotId();
    auto spillSlot = generateSlotId();
    auto stage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(scanSlot),
        makeAggExprVector(
            countsSlot,
            nullptr,
            stage_builder::makeFunction(
                "sum",
                makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1)))),
        makeSV(),  // Seek slot
        true,
        boost::none,
        true /* allowDiskUse */,
        makeSlotExprPairVec(
            spillSlot, stage_builder::makeFunction("sum", stage_builder::makeVariable(spillSlot))),
        nullptr /* yieldPolicy */,
        kEmptyPlanNodeId);

    // Prepare the tree and get the 'SlotAccessor' for the output slot.
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), countsSlot);

    // Read in all of the results.
    std::set<int64_t> results;
    while (stage->getNext() == PlanState::ADVANCED) {
        auto [resTag, resVal] = resultAccessor->getViewOfValue();
        ASSERT_EQ(value::TypeTags::NumberInt64, resTag);
        ASSERT_TRUE(results.insert(value::bitcastFrom<int64_t>(resVal)).second);
    }

    // Check that the results match the expected.
    ASSERT_EQ(3, results.size());
    ASSERT_EQ(1, results.count(2));  // 2 of "5"s
    ASSERT_EQ(1, results.count(3));  // 3 of "6"s
    ASSERT_EQ(1, results.count(4));  // 4 of "7"s

    // Check that it did not spill.
    auto stats = static_cast<const HashAggStats*>(stage->getSpecificStats());
    ASSERT_FALSE(stats->usedDisk);
    ASSERT_EQ(0, stats->spills);
    ASSERT_EQ(0, stats->spilledRecords);

    stage->close();
}

TEST_F(HashAggStageTest, HashAggBasicCountSpillDouble) {
    // We estimate the size of result row like {double, int64} at 50B. Set the memory threshold to
    // 64B so that exactly one row fits in memory.
    auto defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill =
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.load();
    internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(64);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(
            defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill);
    });

    auto ctx = makeCompileCtx();

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [inputTag, inputVal] = stage_builder::makeValue(
        BSON_ARRAY(5.0 << 6.0 << 7.0 << 5.0 << 6.0 << 7.0 << 6.0 << 7.0 << 7.0));
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Build a HashAggStage, group by the scanSlot and compute a simple count.
    auto countsSlot = generateSlotId();
    auto spillSlot = generateSlotId();
    auto stage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(scanSlot),
        makeAggExprVector(
            countsSlot,
            nullptr,
            stage_builder::makeFunction(
                "sum",
                makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1)))),
        makeSV(),  // Seek slot
        true,
        boost::none,
        true /* allowDiskUse */,
        makeSlotExprPairVec(
            spillSlot, stage_builder::makeFunction("sum", stage_builder::makeVariable(spillSlot))),
        nullptr /* yieldPolicy */,
        kEmptyPlanNodeId);

    // Prepare the tree and get the 'SlotAccessor' for the output slot.
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), countsSlot);

    // Read in all of the results.
    std::set<int64_t> results;
    while (stage->getNext() == PlanState::ADVANCED) {
        auto [resTag, resVal] = resultAccessor->getViewOfValue();
        ASSERT_EQ(value::TypeTags::NumberInt64, resTag);
        ASSERT_TRUE(results.insert(value::bitcastFrom<int64_t>(resVal)).second);
    }

    // Check that the results match the expected.
    ASSERT_EQ(3, results.size());
    ASSERT_EQ(1, results.count(2));  // 2 of "5.0"s
    ASSERT_EQ(1, results.count(3));  // 3 of "6.0"s
    ASSERT_EQ(1, results.count(4));  // 4 of "7.0"s

    // Check that the spilling behavior matches the expected.
    auto stats = static_cast<const HashAggStats*>(stage->getSpecificStats());
    ASSERT_TRUE(stats->usedDisk);
    // Memory usage is estimated only every two rows at the most frequent. Also, we only start
    // spilling after estimating that the memory budget is exceeded. These two factors result in
    // fewer expected spills than there are input records, even though only one record fits in
    // memory at a time.
    ASSERT_EQ(stats->spills, 3);
    // The input has one run of two consecutive values, so we expect to spill as many records as
    // there are input values minus one.
    ASSERT_EQ(stats->spilledRecords, 8);

    stage->close();
}

TEST_F(HashAggStageTest, HashAggBasicCountNoSpillWithNoGroupByDouble) {
    auto defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill =
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.load();
    internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(1);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(
            defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill);
    });

    auto ctx = makeCompileCtx();

    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(1.0 << 2.0 << 3.0 << 4.0 << 5.0));
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Build a HashAggStage, with an empty group by slot and compute a simple count.
    auto countsSlot = generateSlotId();
    auto spillSlot = generateSlotId();
    auto stage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(),
        makeAggExprVector(
            countsSlot,
            nullptr,
            stage_builder::makeFunction(
                "sum",
                makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1)))),
        makeSV(),  // Seek slot
        true,
        boost::none,
        true /* allowDiskUse */,
        makeSlotExprPairVec(
            spillSlot, stage_builder::makeFunction("sum", stage_builder::makeVariable(spillSlot))),
        nullptr /* yieldPolicy */,
        kEmptyPlanNodeId);

    // Prepare the tree and get the 'SlotAccessor' for the output slot.
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), countsSlot);

    // Read in all of the results.
    std::set<int64_t> results;
    while (stage->getNext() == PlanState::ADVANCED) {
        auto [resTag, resVal] = resultAccessor->getViewOfValue();
        ASSERT_EQ(value::TypeTags::NumberInt64, resTag);
        ASSERT_TRUE(results.insert(value::bitcastFrom<int64_t>(resVal)).second);
    }

    // Check that the results match the expected.
    ASSERT_EQ(1, results.size());
    ASSERT_EQ(1, results.count(5));

    // Check that the spilling behavior matches the expected.
    auto stats = static_cast<const HashAggStats*>(stage->getSpecificStats());
    ASSERT_FALSE(stats->usedDisk);
    ASSERT_EQ(0, stats->spills);
    ASSERT_EQ(0, stats->spilledRecords);

    stage->close();
}

TEST_F(HashAggStageTest, HashAggMultipleAccSpill) {
    // We estimate the size of result row like {double, int64} at 59B. Set the memory threshold to
    // 128B so that two rows fit in memory.
    auto defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill =
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.load();
    internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(128);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(
            defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill);
    });

    auto ctx = makeCompileCtx();

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(5 << 6 << 7 << 5 << 6 << 7 << 6 << 7 << 7));
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Build a HashAggStage, group by the scanSlot and compute a simple count.
    auto countsSlot = generateSlotId();
    auto sumsSlot = generateSlotId();
    auto spillSlot1 = generateSlotId();
    auto spillSlot2 = generateSlotId();
    auto stage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(scanSlot),
        makeAggExprVector(
            countsSlot,
            nullptr,
            stage_builder::makeFunction(
                "sum",
                makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1))),
            sumsSlot,
            nullptr,
            stage_builder::makeFunction("sum", makeE<EVariable>(scanSlot))),
        makeSV(),  // Seek slot
        true,
        boost::none,
        true /* allowDiskUse */,
        makeSlotExprPairVec(
            spillSlot1,
            stage_builder::makeFunction("sum", stage_builder::makeVariable(spillSlot1)),
            spillSlot2,
            stage_builder::makeFunction("sum", stage_builder::makeVariable(spillSlot2))),
        nullptr /* yieldPolicy */,
        kEmptyPlanNodeId);

    // Prepare the tree and get the 'SlotAccessor' for the output slot.
    auto resultAccessors = prepareTree(ctx.get(), stage.get(), makeSV(countsSlot, sumsSlot));

    // Read in all of the results.
    std::set<std::pair<int64_t /*count*/, int32_t /*sum*/>> results;
    while (stage->getNext() == PlanState::ADVANCED) {
        auto [resCountTag, resCountVal] = resultAccessors[0]->getViewOfValue();
        ASSERT_EQ(value::TypeTags::NumberInt64, resCountTag);

        auto [resSumTag, resSumVal] = resultAccessors[1]->getViewOfValue();
        ASSERT_EQ(value::TypeTags::NumberInt32, resSumTag);

        ASSERT_TRUE(results
                        .insert(std::make_pair(value::bitcastFrom<int64_t>(resCountVal),
                                               value::bitcastFrom<int32_t>(resSumVal)))
                        .second);
    }

    // Check that the results match the expected.
    ASSERT_EQ(3, results.size());
    ASSERT_EQ(1, results.count(std::make_pair(2, 2 * 5)));  // 2 of "5"s
    ASSERT_EQ(1, results.count(std::make_pair(3, 3 * 6)));  // 3 of "6"s
    ASSERT_EQ(1, results.count(std::make_pair(4, 4 * 7)));  // 4 of "7"s

    // Check that the spilling behavior matches the expected.
    auto stats = static_cast<const HashAggStats*>(stage->getSpecificStats());
    ASSERT_TRUE(stats->usedDisk);
    ASSERT_EQ(stats->spills, 3);
    // The input has one run of two consecutive values, so we expect to spill as many records as
    // there are input values minus one.
    ASSERT_EQ(stats->spilledRecords, 8);

    stage->close();
}

TEST_F(HashAggStageTest, HashAggMultipleAccSpillAllToDisk) {
    // Set available memory to zero so all aggregated rows have to be spilled.
    auto defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill =
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.load();
    internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(0);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(
            defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill);
    });

    auto ctx = makeCompileCtx();

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(5 << 6 << 7 << 5 << 6 << 7 << 6 << 7 << 7));
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Build a HashAggStage, group by the scanSlot and compute a simple count.
    auto countsSlot = generateSlotId();
    auto sumsSlot = generateSlotId();
    auto spillSlot1 = generateSlotId();
    auto spillSlot2 = generateSlotId();
    auto stage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(scanSlot),
        makeAggExprVector(
            countsSlot,
            nullptr,
            stage_builder::makeFunction(
                "sum",
                makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1))),
            sumsSlot,
            nullptr,
            stage_builder::makeFunction("sum", makeE<EVariable>(scanSlot))),
        makeSV(),  // Seek slot
        true,
        boost::none,
        true,  // allowDiskUse=true
        makeSlotExprPairVec(
            spillSlot1,
            stage_builder::makeFunction("sum", stage_builder::makeVariable(spillSlot1)),
            spillSlot2,
            stage_builder::makeFunction("sum", stage_builder::makeVariable(spillSlot2))),
        nullptr /* yieldPolicy */,
        kEmptyPlanNodeId);

    // Prepare the tree and get the 'SlotAccessor' for the output slot.
    auto resultAccessors = prepareTree(ctx.get(), stage.get(), makeSV(countsSlot, sumsSlot));

    // Read in all of the results.
    std::set<std::pair<int64_t /*count*/, int32_t /*sum*/>> results;
    while (stage->getNext() == PlanState::ADVANCED) {
        auto [resCountTag, resCountVal] = resultAccessors[0]->getViewOfValue();
        ASSERT_EQ(value::TypeTags::NumberInt64, resCountTag);

        auto [resSumTag, resSumVal] = resultAccessors[1]->getViewOfValue();
        ASSERT_EQ(value::TypeTags::NumberInt32, resSumTag);

        ASSERT_TRUE(results
                        .insert(std::make_pair(value::bitcastFrom<int64_t>(resCountVal),
                                               value::bitcastFrom<int32_t>(resSumVal)))
                        .second);
    }

    // Check that the results match the expected.
    ASSERT_EQ(3, results.size());
    ASSERT_EQ(1, results.count(std::make_pair(2, 2 * 5)));  // 2 of "5"s
    ASSERT_EQ(1, results.count(std::make_pair(3, 3 * 6)));  // 3 of "6"s
    ASSERT_EQ(1, results.count(std::make_pair(4, 4 * 7)));  // 4 of "7"s

    // Check that the spilling behavior matches the expected.
    auto stats = static_cast<const HashAggStats*>(stage->getSpecificStats());
    ASSERT_TRUE(stats->usedDisk);
    // We expect each incoming value to result in a spill of a single record.
    ASSERT_EQ(stats->spills, 9);
    ASSERT_EQ(stats->spilledRecords, 9);

    stage->close();
}

TEST_F(HashAggStageTest, HashAggSum10Groups) {
    // Changing the query knobs to always re-estimate the hash table size in HashAgg and spill when
    // estimated size is >= 128. This should spilt the number of ints between the hash table and
    // the record store somewhat evenly.
    const auto memLimit = 128;
    auto defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill =
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.load();
    internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(memLimit);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(
            defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill);
    });

    auto ctx = makeCompileCtx();

    // Build an array with sums over 100 congruence groups.
    BSONArrayBuilder builder;
    stdx::unordered_map<int, int> sums;
    for (int i = 0; i < 10 * memLimit; ++i) {
        auto val = i % 10;
        auto [it, inserted] = sums.try_emplace(val, val);
        if (!inserted) {
            it->second += val;
        }
        builder.append(val);
    }

    auto [inputTag, inputVal] = stage_builder::makeValue(BSONArray(builder.done()));
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Build a HashAggStage, group by the scanSlot and compute a sum for each group.
    auto sumsSlot = generateSlotId();
    auto spillSlot = generateSlotId();
    auto stage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(scanSlot),
        makeAggExprVector(
            sumsSlot, nullptr, stage_builder::makeFunction("sum", makeE<EVariable>(scanSlot))),
        makeSV(),  // Seek slot
        true,
        boost::none,
        true,  // allowDiskUse=true
        makeSlotExprPairVec(
            spillSlot, stage_builder::makeFunction("sum", stage_builder::makeVariable(spillSlot))),
        nullptr /* yieldPolicy */,
        kEmptyPlanNodeId);

    // Prepare the tree and get the 'SlotAccessor' for the output slot.
    auto resultAccessors = prepareTree(ctx.get(), stage.get(), makeSV(scanSlot, sumsSlot));

    while (stage->getNext() == PlanState::ADVANCED) {
        auto [resGroupByTag, resGroupByVal] = resultAccessors[0]->getViewOfValue();
        auto [resSumTag, resSumVal] = resultAccessors[1]->getViewOfValue();
        auto it = sums.find(value::bitcastTo<int>(resGroupByVal));
        ASSERT_TRUE(it != sums.end());
        assertValuesEqual(resSumTag,
                          resSumVal,
                          value::TypeTags::NumberInt32,
                          value::bitcastFrom<int>(it->second));
    }
    stage->close();
}

TEST_F(HashAggStageTest, HashAggBasicCountWithRecordIds) {
    auto ctx = makeCompileCtx();

    // Build a scan of a few record ids.
    std::vector<int64_t> ids{10, 999, 10, 999, 1, 999, 8589869056, 999, 10, 8589869056};
    auto [inputTag, inputVal] = sbe::value::makeNewArray();
    auto testData = sbe::value::getArrayView(inputVal);
    for (auto id : ids) {
        auto [ridTag, ridVal] = sbe::value::makeNewRecordId(id);
        testData->push_back(ridTag, ridVal);
    }
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Build a HashAggStage, group by the scanSlot and compute a simple count.
    auto countsSlot = generateSlotId();
    auto spillSlot = generateSlotId();
    auto stage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(scanSlot),
        makeAggExprVector(
            countsSlot,
            nullptr,
            stage_builder::makeFunction(
                "sum",
                makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1)))),
        makeSV(),  // Seek slot
        true,
        boost::none,
        true,  // allowDiskUse=true
        makeSlotExprPairVec(
            spillSlot, stage_builder::makeFunction("sum", stage_builder::makeVariable(spillSlot))),
        nullptr /* yieldPolicy */,
        kEmptyPlanNodeId);

    // Prepare the tree and get the 'SlotAccessor' for the output slot.
    auto resultAccessors = prepareTree(ctx.get(), stage.get(), makeSV(scanSlot, countsSlot));

    // Read in all of the results.
    std::map<int64_t /*id*/, int64_t /*count*/> results;
    while (stage->getNext() == PlanState::ADVANCED) {
        auto [resScanTag, resScanVal] = resultAccessors[0]->getViewOfValue();
        ASSERT_EQ(value::TypeTags::RecordId, resScanTag);

        auto [resTag, resVal] = resultAccessors[1]->getViewOfValue();
        ASSERT_EQ(value::TypeTags::NumberInt64, resTag);

        auto inserted = results.insert(std::make_pair(
            value::bitcastFrom<int64_t>(sbe::value::getRecordIdView(resScanVal)->getLong()),
            value::bitcastFrom<int64_t>(resVal)));
        ASSERT_TRUE(inserted.second);
    }

    // Assert that the results are as expected.
    ASSERT_EQ(4, results.size());
    ASSERT_EQ(1, results[1]);
    ASSERT_EQ(2, results[8589869056]);
    ASSERT_EQ(3, results[10]);
    ASSERT_EQ(4, results[999]);

    stage->close();
}
}  // namespace mongo::sbe
