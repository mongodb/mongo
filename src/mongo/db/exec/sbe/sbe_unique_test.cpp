// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>

namespace mongo::sbe {
/**
 * This file contains tests for sbe::UniqueStage and sbe::UniqueRoaringStage.
 */

using UniqueStageTest = PlanStageTestFixture;
using UniqueRoaringStageTest = PlanStageTestFixture;

TEST_F(UniqueStageTest, DeduplicatesAndPreservesOrderSimple) {
    value::TagValueOwned inputOwned =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 1)));

    value::TagValueOwned expectedOwned =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3)));

    auto makeStageFn = [](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        auto unique =
            makeS<UniqueStage>(std::move(scanStage), sbe::makeSV(scanSlot), kEmptyPlanNodeId);

        return std::make_pair(scanSlot, std::move(unique));
    };

    auto [inputTag, inputVal] = inputOwned.releaseToRaw();
    auto [expectedTag, expectedVal] = expectedOwned.releaseToRaw();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(UniqueRoaringStageTest, DeduplicatesAndPreservesOrderSimple) {
    value::TagValueOwned inputOwned =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 1)));

    value::TagValueOwned expectedOwned =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3)));

    auto makeStageFn = [](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        auto unique = makeS<UniqueRoaringStage>(std::move(scanStage), scanSlot, kEmptyPlanNodeId);

        return std::make_pair(scanSlot, std::move(unique));
    };

    auto [inputTag, inputVal] = inputOwned.releaseToRaw();
    auto [expectedTag, expectedVal] = expectedOwned.releaseToRaw();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(UniqueRoaringStageTest, DeduplicatesAndPreservesOrderSimpleRecordId) {
    auto createRecordIdArray =
        [](std::vector<int64_t> rids) -> std::pair<value::TypeTags, value::Value> {
        auto [arrTag, arrVal] = value::makeNewArray();
        auto arr = value::getArrayView(arrVal);
        for (auto rid : rids) {
            auto [ridTag, ridVal] = value::makeNewRecordId(rid);
            arr->push_back_raw(ridTag, ridVal);
        }
        return {arrTag, arrVal};
    };

    value::TagValueOwned inputOwned =
        value::TagValueOwned::fromRaw(createRecordIdArray({1, 2, 3, 1, 4}));

    value::TagValueOwned expectedOwned =
        value::TagValueOwned::fromRaw(createRecordIdArray({1, 2, 3, 4}));

    auto makeStageFn = [](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        auto unique = makeS<UniqueRoaringStage>(std::move(scanStage), scanSlot, kEmptyPlanNodeId);

        return std::make_pair(scanSlot, std::move(unique));
    };

    auto [inputTag, inputVal] = inputOwned.releaseToRaw();
    auto [expectedTag, expectedVal] = expectedOwned.releaseToRaw();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(UniqueStageTest, DeduplicatesMultipleSlotsInKey) {
    auto [tag, val] = stage_builder::makeValue(BSON_ARRAY(
        BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2) << BSON_ARRAY(1 << 1) << BSON_ARRAY(3 << 3)));
    auto [scanSlots, scan] = generateVirtualScanMulti(2,  // numSlots
                                                      tag,
                                                      val);

    value::TagValueOwned expectedOwned = value::TagValueOwned::fromRaw(stage_builder::makeValue(
        BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2) << BSON_ARRAY(3 << 3))));

    auto unique = makeS<UniqueStage>(std::move(scan), scanSlots, kEmptyPlanNodeId);

    auto ctx = makeCompileCtx();
    auto resultAccessors = prepareTree(ctx.get(), unique.get(), scanSlots);

    value::TagValueOwned resultsOwned =
        value::TagValueOwned::fromRaw(getAllResultsMulti(unique.get(), resultAccessors));

    ASSERT_TRUE(valueEquals(
        resultsOwned.tag(), resultsOwned.value(), expectedOwned.tag(), expectedOwned.value()));
}

TEST_F(UniqueStageTest, ResetsStateAfterClose) {
    auto [tag, val] = stage_builder::makeValue(BSON_ARRAY(
        BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2) << BSON_ARRAY(1 << 1) << BSON_ARRAY(3 << 3)));
    auto [scanSlot, scanStage] =
        generateVirtualScan(value::TagValueMaybeOwned::fromRaw(true, tag, val));

    value::TagValueOwned expectedOwned = value::TagValueOwned::fromRaw(stage_builder::makeValue(
        BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2) << BSON_ARRAY(3 << 3))));

    auto unique = makeS<UniqueStage>(std::move(scanStage), sbe::makeSV(scanSlot), kEmptyPlanNodeId);
    auto ctx = makeCompileCtx();
    auto resultAccessor = prepareTree(ctx.get(), unique.get(), scanSlot);

    value::TagValueOwned resultsOwned =
        value::TagValueOwned::fromRaw(getAllResults(unique.get(), resultAccessor));

    ASSERT_TRUE(valueEquals(
        resultsOwned.tag(), resultsOwned.value(), expectedOwned.tag(), expectedOwned.value()));

    // Closing and opening the plan should have the effect of clearing the values that 'unique'
    // has seen.
    unique->close();
    unique->open(false);

    value::TagValueOwned resetResultsOwned =
        value::TagValueOwned::fromRaw(getAllResults(unique.get(), resultAccessor));

    // The same result is seen again after closing and opening the plan tree. This proves the
    // seen set has been cleared, otherwise we would not get any result from the second run.
    ASSERT_TRUE(valueEquals(resetResultsOwned.tag(),
                            resetResultsOwned.value(),
                            expectedOwned.tag(),
                            expectedOwned.value()));
}

TEST_F(UniqueRoaringStageTest, ResetsStateAfterClose) {
    auto [tag, val] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 1 << 3));
    auto [scanSlot, scanStage] =
        generateVirtualScan(value::TagValueMaybeOwned::fromRaw(true, tag, val));

    value::TagValueOwned expectedOwned =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3)));

    auto unique = makeS<UniqueRoaringStage>(std::move(scanStage), scanSlot, kEmptyPlanNodeId);
    auto ctx = makeCompileCtx();
    auto resultAccessor = prepareTree(ctx.get(), unique.get(), scanSlot);

    value::TagValueOwned resultsOwned =
        value::TagValueOwned::fromRaw(getAllResults(unique.get(), resultAccessor));

    ASSERT_TRUE(valueEquals(
        resultsOwned.tag(), resultsOwned.value(), expectedOwned.tag(), expectedOwned.value()));

    // Closing and opening the plan should have the effect of clearing the values that 'unique'
    // has seen.
    unique->close();
    unique->open(false);

    value::TagValueOwned resetResultsOwned =
        value::TagValueOwned::fromRaw(getAllResults(unique.get(), resultAccessor));

    // The same result is seen again after closing and opening the plan tree. This proves the
    // seen set has been cleared, otherwise we would not get any result from the second run.
    ASSERT_TRUE(valueEquals(resetResultsOwned.tag(),
                            resetResultsOwned.value(),
                            expectedOwned.tag(),
                            expectedOwned.value()));
}

TEST_F(UniqueStageTest, ResetsStateAfterReopen) {
    auto [tag, val] = stage_builder::makeValue(BSON_ARRAY(
        BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2) << BSON_ARRAY(1 << 1) << BSON_ARRAY(3 << 3)));
    auto [scanSlot, scanStage] =
        generateVirtualScan(value::TagValueMaybeOwned::fromRaw(true, tag, val));

    value::TagValueOwned expectedOwned = value::TagValueOwned::fromRaw(stage_builder::makeValue(
        BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2) << BSON_ARRAY(3 << 3))));

    auto unique = makeS<UniqueStage>(std::move(scanStage), sbe::makeSV(scanSlot), kEmptyPlanNodeId);
    auto ctx = makeCompileCtx();
    auto resultAccessor = prepareTree(ctx.get(), unique.get(), scanSlot);

    value::TagValueOwned resultsOwned =
        value::TagValueOwned::fromRaw(getAllResults(unique.get(), resultAccessor));

    ASSERT_TRUE(valueEquals(
        resultsOwned.tag(), resultsOwned.value(), expectedOwned.tag(), expectedOwned.value()));

    // Calling open with reOpen set to 'true' should have the effect of clearing the values that
    // 'unique' has seen.
    unique->open(/* reOpen */ true);
    value::TagValueOwned resetResultsOwned =
        value::TagValueOwned::fromRaw(getAllResults(unique.get(), resultAccessor));

    // The same result is seen after re-opening the plan tree. This proves the seen set has been
    // cleared, otherwise we would not get any result from the second run.
    ASSERT_TRUE(valueEquals(resetResultsOwned.tag(),
                            resetResultsOwned.value(),
                            expectedOwned.tag(),
                            expectedOwned.value()));
}

TEST_F(UniqueStageTest, UniqueStageTracksMemory) {
    // Mix short and long strings to exercise the how we estimate memory used.
    auto [tag, val] = stage_builder::makeValue(
        BSON_ARRAY("a" << "b-enormous" << "c" << "d-enormous" << "e" << "f-enormous"));
    auto [scanSlot, scanStage] =
        generateVirtualScan(value::TagValueMaybeOwned::fromRaw(true, tag, val));

    auto unique = makeS<UniqueStage>(std::move(scanStage), sbe::makeSV(scanSlot), kEmptyPlanNodeId);
    auto ctx = makeCompileCtx();
    prepareTree(ctx.get(), unique.get());

    const SimpleMemoryUsageTracker& tracker = *unique->getMemoryTracker();

    size_t i = 0;
    int64_t prevTrackedMem = tracker.inUseTrackedMemoryBytes();
    for (auto st = unique->getNext(); st == PlanState::ADVANCED; st = unique->getNext(), ++i) {
        // Since every output value represent a new value inserted into the _seen map, memory will
        // always be increasing.
        int64_t newTrackedMem = tracker.inUseTrackedMemoryBytes();
        ASSERT_GT(newTrackedMem, prevTrackedMem);
        prevTrackedMem = newTrackedMem;
    }
}

TEST_F(UniqueRoaringStageTest, ResetsStateAfterReopen) {
    auto [tag, val] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 1 << 3));
    auto [scanSlot, scanStage] =
        generateVirtualScan(value::TagValueMaybeOwned::fromRaw(true, tag, val));

    value::TagValueOwned expectedOwned =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3)));

    auto unique = makeS<UniqueRoaringStage>(std::move(scanStage), scanSlot, kEmptyPlanNodeId);
    auto ctx = makeCompileCtx();
    auto resultAccessor = prepareTree(ctx.get(), unique.get(), scanSlot);

    value::TagValueOwned resultsOwned =
        value::TagValueOwned::fromRaw(getAllResults(unique.get(), resultAccessor));

    ASSERT_TRUE(valueEquals(
        resultsOwned.tag(), resultsOwned.value(), expectedOwned.tag(), expectedOwned.value()));

    // Calling open with reOpen set to 'true' should have the effect of clearing the values that
    // 'unique' has seen.
    unique->open(/* reOpen */ true);
    value::TagValueOwned resetResultsOwned =
        value::TagValueOwned::fromRaw(getAllResults(unique.get(), resultAccessor));

    // The same result is seen after re-opening the plan tree. This proves the seen set has been
    // cleared, otherwise we would not get any result from the second run.
    ASSERT_TRUE(valueEquals(resetResultsOwned.tag(),
                            resetResultsOwned.value(),
                            expectedOwned.tag(),
                            expectedOwned.value()));
}

TEST_F(UniqueRoaringStageTest, UniqueRoaringStageTracksMemory) {
    // Mix short and long strings to exercise the how we estimate memory used.
    auto [tag, val] =
        stage_builder::makeValue(BSON_ARRAY(1 << 100 << 2 << 1 << 3 << 2 << 4 << 100));
    auto [scanSlot, scanStage] =
        generateVirtualScan(value::TagValueMaybeOwned::fromRaw(true, tag, val));

    auto unique = makeS<UniqueRoaringStage>(std::move(scanStage), scanSlot, kEmptyPlanNodeId);
    auto ctx = makeCompileCtx();
    prepareTree(ctx.get(), unique.get());

    const SimpleMemoryUsageTracker& tracker = *unique->getMemoryTracker();

    size_t i = 0;
    int64_t prevTrackedMem = tracker.inUseTrackedMemoryBytes();
    for (auto st = unique->getNext(); st == PlanState::ADVANCED; st = unique->getNext(), ++i) {
        // Since every output value represent a new value inserted into the _seen HashRoaringSet,
        // memory will always be increasing.
        int64_t newTrackedMem = tracker.inUseTrackedMemoryBytes();
        ASSERT_GT(newTrackedMem, prevTrackedMem);
        prevTrackedMem = newTrackedMem;
    }
}
}  // namespace mongo::sbe
