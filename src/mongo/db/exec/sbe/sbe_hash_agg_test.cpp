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

#include "mongo/platform/basic.h"


#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/util/assert_util.h"

namespace mongo::sbe {

class HashAggStageTest : public PlanStageTestFixture {
public:
    void performHashAggWithSpillChecking(
        BSONArray inputArr,
        BSONArray expectedOutputArray,
        bool shouldSpill = false,
        std::unique_ptr<mongo::CollatorInterfaceMock> optionalCollator = nullptr);
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
    auto shouldUseCollator = optionalCollator.get() != nullptr;

    auto makeStageFn = [this, collatorSlot, shouldUseCollator, shouldSpill](
                           value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        auto countsSlot = generateSlotId();

        auto hashAggStage = makeS<HashAggStage>(
            std::move(scanStage),
            makeSV(scanSlot),
            makeEM(countsSlot,
                   stage_builder::makeFunction("sum",
                                               makeE<EConstant>(value::TypeTags::NumberInt64,
                                                                value::bitcastFrom<int64_t>(1)))),
            makeSV(),
            true,
            boost::optional<value::SlotId>{shouldUseCollator, collatorSlot},
            shouldSpill,
            kEmptyPlanNodeId);

        return std::make_pair(countsSlot, std::move(hashAggStage));
    };

    auto ctx = makeCompileCtx();

    value::OwnedValueAccessor collatorAccessor;
    if (shouldUseCollator) {
        ctx->pushCorrelated(collatorSlot, &collatorAccessor);
        collatorAccessor.reset(value::TypeTags::collator,
                               value::bitcastFrom<CollatorInterface*>(optionalCollator.get()));
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
        auto collExpr = makeE<EConstant>(value::TypeTags::collator,
                                         value::bitcastFrom<CollatorInterface*>(collator.get()));

        // Build a HashAggStage that exercises the collMin() and collMax() aggregate functions.
        auto minSlot = generateSlotId();
        auto maxSlot = generateSlotId();
        auto collMinSlot = generateSlotId();
        auto collMaxSlot = generateSlotId();
        auto hashAggStage = makeS<HashAggStage>(
            std::move(scanStage),
            makeSV(),
            makeEM(minSlot,
                   stage_builder::makeFunction("min", makeE<EVariable>(scanSlot)),
                   maxSlot,
                   stage_builder::makeFunction("max", makeE<EVariable>(scanSlot)),
                   collMinSlot,
                   stage_builder::makeFunction(
                       "collMin", collExpr->clone(), makeE<EVariable>(scanSlot)),
                   collMaxSlot,
                   stage_builder::makeFunction(
                       "collMax", collExpr->clone(), makeE<EVariable>(scanSlot))),
            makeSV(),
            true,
            boost::none,
            false /* allowDiskUse */,
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
        auto collExpr = makeE<EConstant>(value::TypeTags::collator,
                                         value::bitcastFrom<CollatorInterface*>(collator.get()));

        // Build a HashAggStage that exercises the collAddToSet() aggregate function.
        auto hashAggSlot = generateSlotId();
        auto hashAggStage = makeS<HashAggStage>(
            std::move(scanStage),
            makeSV(),
            makeEM(hashAggSlot,
                   stage_builder::makeFunction(
                       "collAddToSet", std::move(collExpr), makeE<EVariable>(scanSlot))),
            makeSV(),
            true,
            boost::none,
            false /* allowDiskUse */,
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
            makeEM(countsSlot,
                   stage_builder::makeFunction("sum",
                                               makeE<EConstant>(value::TypeTags::NumberInt64,
                                                                value::bitcastFrom<int64_t>(1)))),
            makeSV(seekSlot),
            true,
            boost::none,
            false /* allowDiskUse */,
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

TEST_F(HashAggStageTest, HashAggBasicCountSpill) {
    // Changing the query knobs to always re-estimate the hash table size in HashAgg and spill when
    // estimated size is >= 4 * 8.
    auto defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill =
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.load();
    internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(4 * 8);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(
            defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill);
    });
    auto defaultInternalQuerySBEAggMemoryUseSampleRate =
        internalQuerySBEAggMemoryUseSampleRate.load();
    internalQuerySBEAggMemoryUseSampleRate.store(1.0);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggMemoryUseSampleRate.store(defaultInternalQuerySBEAggMemoryUseSampleRate);
    });

    auto ctx = makeCompileCtx();

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(5 << 6 << 7 << 5 << 6 << 7 << 6 << 7 << 7));
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Build a HashAggStage, group by the scanSlot and compute a simple count.
    auto countsSlot = generateSlotId();
    auto stage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(scanSlot),
        makeEM(countsSlot,
               stage_builder::makeFunction(
                   "sum",
                   makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1)))),
        makeSV(),  // Seek slot
        true,
        boost::none,
        true /* allowDiskUse */,
        kEmptyPlanNodeId);

    // Prepare the tree and get the 'SlotAccessor' for the output slot.
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), countsSlot);

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res1Tag, res1Val] = resultAccessor->getViewOfValue();
    // There are '2' occurences of '5' in the input.
    assertValuesEqual(res1Tag, res1Val, value::TypeTags::NumberInt32, value::bitcastFrom<int>(2));

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res2Tag, res2Val] = resultAccessor->getViewOfValue();
    // There are '3' occurences of '6' in the input.
    assertValuesEqual(res2Tag, res2Val, value::TypeTags::NumberInt32, value::bitcastFrom<int>(3));

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res3Tag, res3Val] = resultAccessor->getViewOfValue();
    // There are '4' occurences of '7' in the input.
    assertValuesEqual(res3Tag, res3Val, value::TypeTags::NumberInt32, value::bitcastFrom<int>(4));
    ASSERT_TRUE(stage->getNext() == PlanState::IS_EOF);

    stage->close();
}

TEST_F(HashAggStageTest, HashAggBasicCountSpillDouble) {
    // Changing the query knobs to always re-estimate the hash table size in HashAgg and spill when
    // estimated size is >= 4 * 8.
    auto defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill =
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.load();
    internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(4 * 8);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(
            defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill);
    });
    auto defaultInternalQuerySBEAggMemoryUseSampleRate =
        internalQuerySBEAggMemoryUseSampleRate.load();
    internalQuerySBEAggMemoryUseSampleRate.store(1.0);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggMemoryUseSampleRate.store(defaultInternalQuerySBEAggMemoryUseSampleRate);
    });

    auto ctx = makeCompileCtx();

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [inputTag, inputVal] = stage_builder::makeValue(
        BSON_ARRAY(5.0 << 6.0 << 7.0 << 5.0 << 6.0 << 7.0 << 6.0 << 7.0 << 7.0));
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Build a HashAggStage, group by the scanSlot and compute a simple count.
    auto countsSlot = generateSlotId();
    auto stage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(scanSlot),
        makeEM(countsSlot,
               stage_builder::makeFunction(
                   "sum",
                   makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1)))),
        makeSV(),  // Seek slot
        true,
        boost::none,
        true /* allowDiskUse */,
        kEmptyPlanNodeId);

    // Prepare the tree and get the 'SlotAccessor' for the output slot.
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), countsSlot);

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res1Tag, res1Val] = resultAccessor->getViewOfValue();
    // There are '2' occurences of '5' in the input.
    assertValuesEqual(res1Tag, res1Val, value::TypeTags::NumberInt32, value::bitcastFrom<int>(2));

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res2Tag, res2Val] = resultAccessor->getViewOfValue();
    // There are '3' occurences of '6' in the input.
    assertValuesEqual(res2Tag, res2Val, value::TypeTags::NumberInt32, value::bitcastFrom<int>(3));

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res3Tag, res3Val] = resultAccessor->getViewOfValue();
    // There are '4' occurences of '7' in the input.
    assertValuesEqual(res3Tag, res3Val, value::TypeTags::NumberInt32, value::bitcastFrom<int>(4));
    ASSERT_TRUE(stage->getNext() == PlanState::IS_EOF);

    stage->close();
}


TEST_F(HashAggStageTest, HashAggMultipleAccSpill) {
    // Changing the query knobs to always re-estimate the hash table size in HashAgg and spill when
    // estimated size is >= 2 * 8.
    auto defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill =
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.load();
    internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(2 * 8);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(
            defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill);
    });
    auto defaultInternalQuerySBEAggMemoryUseSampleRate =
        internalQuerySBEAggMemoryUseSampleRate.load();
    internalQuerySBEAggMemoryUseSampleRate.store(1.0);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggMemoryUseSampleRate.store(defaultInternalQuerySBEAggMemoryUseSampleRate);
    });

    auto ctx = makeCompileCtx();

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(5 << 6 << 7 << 5 << 6 << 7 << 6 << 7 << 7));
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Build a HashAggStage, group by the scanSlot and compute a simple count.
    auto countsSlot = generateSlotId();
    auto sumsSlot = generateSlotId();
    auto stage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(scanSlot),
        makeEM(countsSlot,
               stage_builder::makeFunction(
                   "sum",
                   makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1))),
               sumsSlot,
               stage_builder::makeFunction("sum", makeE<EVariable>(scanSlot))),
        makeSV(),  // Seek slot
        true,
        boost::none,
        true /* allowDiskUse */,
        kEmptyPlanNodeId);

    // Prepare the tree and get the 'SlotAccessor' for the output slot.
    auto resultAccessors = prepareTree(ctx.get(), stage.get(), makeSV(countsSlot, sumsSlot));

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res1Tag, res1Val] = resultAccessors[0]->getViewOfValue();
    auto [res1TagSum, res1ValSum] = resultAccessors[1]->getViewOfValue();

    // There are '2' occurences of '5' in the input.
    assertValuesEqual(res1Tag, res1Val, value::TypeTags::NumberInt32, value::bitcastFrom<int>(2));
    assertValuesEqual(
        res1TagSum, res1ValSum, value::TypeTags::NumberInt32, value::bitcastFrom<int>(10));

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res2Tag, res2Val] = resultAccessors[0]->getViewOfValue();
    auto [res2TagSum, res2ValSum] = resultAccessors[1]->getViewOfValue();
    // There are '3' occurences of '6' in the input.
    assertValuesEqual(res2Tag, res2Val, value::TypeTags::NumberInt32, value::bitcastFrom<int>(3));
    assertValuesEqual(
        res2TagSum, res2ValSum, value::TypeTags::NumberInt32, value::bitcastFrom<int>(18));

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res3Tag, res3Val] = resultAccessors[0]->getViewOfValue();
    auto [res3TagSum, res3ValSum] = resultAccessors[1]->getViewOfValue();
    // There are '4' occurences of '7' in the input.
    assertValuesEqual(res3Tag, res3Val, value::TypeTags::NumberInt32, value::bitcastFrom<int>(4));
    assertValuesEqual(
        res3TagSum, res3ValSum, value::TypeTags::NumberInt32, value::bitcastFrom<int>(28));
    ASSERT_TRUE(stage->getNext() == PlanState::IS_EOF);

    stage->close();
}

TEST_F(HashAggStageTest, HashAggMultipleAccSpillAllToDisk) {
    // Changing the query knobs to always re-estimate the hash table size in HashAgg and spill when
    // estimated size is >= 0. This sill spill everything to the RecordStore.
    auto defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill =
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.load();
    internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(0);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(
            defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill);
    });
    auto defaultInternalQuerySBEAggMemoryUseSampleRate =
        internalQuerySBEAggMemoryUseSampleRate.load();
    internalQuerySBEAggMemoryUseSampleRate.store(1.0);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggMemoryUseSampleRate.store(defaultInternalQuerySBEAggMemoryUseSampleRate);
    });

    auto ctx = makeCompileCtx();

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(5 << 6 << 7 << 5 << 6 << 7 << 6 << 7 << 7));
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Build a HashAggStage, group by the scanSlot and compute a simple count.
    auto countsSlot = generateSlotId();
    auto sumsSlot = generateSlotId();
    auto stage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(scanSlot),
        makeEM(countsSlot,
               stage_builder::makeFunction(
                   "sum",
                   makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1))),
               sumsSlot,
               stage_builder::makeFunction("sum", makeE<EVariable>(scanSlot))),
        makeSV(),  // Seek slot
        true,
        boost::none,
        true,  // allowDiskUse=true
        kEmptyPlanNodeId);

    // Prepare the tree and get the 'SlotAccessor' for the output slot.
    auto resultAccessors = prepareTree(ctx.get(), stage.get(), makeSV(countsSlot, sumsSlot));

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res1Tag, res1Val] = resultAccessors[0]->getViewOfValue();
    auto [res1TagSum, res1ValSum] = resultAccessors[1]->getViewOfValue();

    // There are '2' occurences of '5' in the input.
    assertValuesEqual(res1Tag, res1Val, value::TypeTags::NumberInt32, value::bitcastFrom<int>(2));
    assertValuesEqual(
        res1TagSum, res1ValSum, value::TypeTags::NumberInt32, value::bitcastFrom<int>(10));

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res2Tag, res2Val] = resultAccessors[0]->getViewOfValue();
    auto [res2TagSum, res2ValSum] = resultAccessors[1]->getViewOfValue();
    // There are '3' occurences of '6' in the input.
    assertValuesEqual(res2Tag, res2Val, value::TypeTags::NumberInt32, value::bitcastFrom<int>(3));
    assertValuesEqual(
        res2TagSum, res2ValSum, value::TypeTags::NumberInt32, value::bitcastFrom<int>(18));

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res3Tag, res3Val] = resultAccessors[0]->getViewOfValue();
    auto [res3TagSum, res3ValSum] = resultAccessors[1]->getViewOfValue();
    // There are '4' occurences of '7' in the input.
    assertValuesEqual(res3Tag, res3Val, value::TypeTags::NumberInt32, value::bitcastFrom<int>(4));
    assertValuesEqual(
        res3TagSum, res3ValSum, value::TypeTags::NumberInt32, value::bitcastFrom<int>(28));
    ASSERT_TRUE(stage->getNext() == PlanState::IS_EOF);

    stage->close();
}

TEST_F(HashAggStageTest, HashAggMemUsageTest) {
    // Changing the query knobs to always re-estimate the hash table size in HashAgg and spill when
    // estimated size is >= 128 * 5.
    auto defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill =
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.load();
    internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(128 * 5);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(
            defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill);
    });
    auto defaultInternalQuerySBEAggMemoryUseSampleRate =
        internalQuerySBEAggMemoryUseSampleRate.load();
    internalQuerySBEAggMemoryUseSampleRate.store(1.0);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggMemoryUseSampleRate.store(defaultInternalQuerySBEAggMemoryUseSampleRate);
    });

    auto createInputArray = [](int numberOfBytesPerEntry) {
        auto arr = BSON_ARRAY(
            std::string(numberOfBytesPerEntry, 'A')
            << std::string(numberOfBytesPerEntry, 'a') << std::string(numberOfBytesPerEntry, 'b')
            << std::string(numberOfBytesPerEntry, 'c') << std::string(numberOfBytesPerEntry, 'B')
            << std::string(numberOfBytesPerEntry, 'a'));
        return arr;
    };

    auto nonSpillInputArr = createInputArray(64);
    auto spillInputArr = createInputArray(256);

    // Groups the values as: ["a", "a"], ["A"], ["B"], ["b"], ["c"].
    auto expectedOutputArr = BSON_ARRAY(2 << 1 << 1 << 1 << 1);
    // Should NOT spill to disk because internalQuerySlotBasedExecutionHashAggMemoryUsageThreshold
    // is set to 128 * 5. (64 + padding) * 5 < 128 * 5
    performHashAggWithSpillChecking(nonSpillInputArr, expectedOutputArr);
    // Should spill to disk because internalQuerySlotBasedExecutionHashAggMemoryUsageThreshold is
    // set to 128 * 5. (256 + padding) * 5 > 128 * 5
    performHashAggWithSpillChecking(spillInputArr, expectedOutputArr, true);
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
    auto defaultInternalQuerySBEAggMemoryUseSampleRate =
        internalQuerySBEAggMemoryUseSampleRate.load();
    internalQuerySBEAggMemoryUseSampleRate.store(1.0);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggMemoryUseSampleRate.store(defaultInternalQuerySBEAggMemoryUseSampleRate);
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
    auto stage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(scanSlot),
        makeEM(sumsSlot, stage_builder::makeFunction("sum", makeE<EVariable>(scanSlot))),
        makeSV(),  // Seek slot
        true,
        boost::none,
        true,  // allowDiskUse=true
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
    // Changing the query knobs to always re-estimate the hash table size in HashAgg and spill when
    // estimated size is >= 4 * 8.
    auto defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill =
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.load();
    internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(4 * 8);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggApproxMemoryUseInBytesBeforeSpill.store(
            defaultInternalQuerySBEAggApproxMemoryUseInBytesBeforeSpill);
    });
    auto defaultInternalQuerySBEAggMemoryUseSampleRate =
        internalQuerySBEAggMemoryUseSampleRate.load();
    internalQuerySBEAggMemoryUseSampleRate.store(1.0);
    ON_BLOCK_EXIT([&] {
        internalQuerySBEAggMemoryUseSampleRate.store(defaultInternalQuerySBEAggMemoryUseSampleRate);
    });

    auto ctx = makeCompileCtx();

    // Build a scan of record ids [1,10,999,10,1,999,8589869056,999,10,8589869056] input array.
    auto [inputTag, inputVal] = sbe::value::makeNewArray();
    auto testData = sbe::value::getArrayView(inputVal);

    testData->push_back(value::TypeTags::RecordId, value::bitcastFrom<int64_t>(1));
    testData->push_back(value::TypeTags::RecordId, value::bitcastFrom<int64_t>(10));
    testData->push_back(value::TypeTags::RecordId, value::bitcastFrom<int64_t>(999));
    testData->push_back(value::TypeTags::RecordId, value::bitcastFrom<int64_t>(10));
    testData->push_back(value::TypeTags::RecordId, value::bitcastFrom<int64_t>(999));
    testData->push_back(value::TypeTags::RecordId, value::bitcastFrom<int64_t>(1));
    testData->push_back(value::TypeTags::RecordId, value::bitcastFrom<int64_t>(999));
    testData->push_back(value::TypeTags::RecordId, value::bitcastFrom<int64_t>(8589869056));
    testData->push_back(value::TypeTags::RecordId, value::bitcastFrom<int64_t>(999));
    testData->push_back(value::TypeTags::RecordId, value::bitcastFrom<int64_t>(10));
    testData->push_back(value::TypeTags::RecordId, value::bitcastFrom<int64_t>(8589869056));

    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Build a HashAggStage, group by the scanSlot and compute a simple count.
    auto countsSlot = generateSlotId();
    auto stage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(scanSlot),
        makeEM(countsSlot,
               stage_builder::makeFunction(
                   "sum",
                   makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1)))),
        makeSV(),  // Seek slot
        true,
        boost::none,
        true,  // allowDiskUse=true
        kEmptyPlanNodeId);

    // Prepare the tree and get the 'SlotAccessor' for the output slot.
    auto resultAccessors = prepareTree(ctx.get(), stage.get(), makeSV(scanSlot, countsSlot));

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res1ScanTag, res1ScanVal] = resultAccessors[0]->getViewOfValue();
    auto [res1Tag, res1Val] = resultAccessors[1]->getViewOfValue();
    // There are '2' occurences of '1' in the input.
    assertValuesEqual(
        res1ScanTag, res1ScanVal, value::TypeTags::RecordId, value::bitcastFrom<int64_t>(1));
    assertValuesEqual(
        res1Tag, res1Val, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res2ScanTag, res2ScanVal] = resultAccessors[0]->getViewOfValue();
    auto [res2Tag, res2Val] = resultAccessors[1]->getViewOfValue();
    // There are '2' occurences of '8589869056' in the input.
    assertValuesEqual(res2ScanTag,
                      res2ScanVal,
                      value::TypeTags::RecordId,
                      value::bitcastFrom<int64_t>(8589869056));
    assertValuesEqual(
        res2Tag, res2Val, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res3ScanTag, res3ScanVal] = resultAccessors[0]->getViewOfValue();
    auto [res3Tag, res3Val] = resultAccessors[1]->getViewOfValue();
    // There are '3' occurences of '10' in the input.
    assertValuesEqual(
        res3ScanTag, res3ScanVal, value::TypeTags::RecordId, value::bitcastFrom<int64_t>(10));
    assertValuesEqual(
        res3Tag, res3Val, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3));

    ASSERT_TRUE(stage->getNext() == PlanState::ADVANCED);
    auto [res4ScanTag, res4ScanVal] = resultAccessors[0]->getViewOfValue();
    auto [res4Tag, res4Val] = resultAccessors[1]->getViewOfValue();
    // There are '4' occurences of '999' in the input.
    assertValuesEqual(
        res4ScanTag, res4ScanVal, value::TypeTags::RecordId, value::bitcastFrom<int64_t>(999));
    assertValuesEqual(
        res4Tag, res4Val, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(4));
    ASSERT_TRUE(stage->getNext() == PlanState::IS_EOF);

    stage->close();
}
}  // namespace mongo::sbe
