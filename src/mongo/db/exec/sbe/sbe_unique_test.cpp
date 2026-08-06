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
 * This file contains tests for sbe::UniqueStage and sbe::UniqueRoaringStage (including its
 * optional fused-filter mode).
 */

using UniqueStageTest = PlanStageTestFixture;
using UniqueRoaringStageTest = PlanStageTestFixture;
using UniqueRoaringStageWithFilterTest = PlanStageTestFixture;

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

namespace {
// Two-slot virtual scan (recordId, keyValue) wrapped in a UniqueRoaringStage (with a filter) that
// dedups on recordId and filters on "keyValue == 'b'".
std::pair<value::SlotVector, std::unique_ptr<PlanStage>> makeDedupFilterEqBStage(
    PlanStageTestFixture& fixture, const BSONArray& rows) {
    auto [scanSlots, scanStage] = fixture.generateVirtualScanMulti(2, rows);
    auto filterExpr = sbe::makeE<EPrimBinary>(
        EPrimBinary::eq, makeVariable(scanSlots[1]), makeStringConstant("b"));
    auto stage = makeS<UniqueRoaringStage>(
        std::move(scanStage), scanSlots[0], std::move(filterExpr), kEmptyPlanNodeId);
    return {scanSlots, std::move(stage)};
}
}  // namespace

TEST_F(UniqueRoaringStageWithFilterTest, MatchOnLaterKeyStillReturnsRecordExactlyOnce) {
    // Record 1's first key ("a") fails the filter; only its second key ("b") passes. A naive
    // dedup-before-filter ordering would drop the matching "b" key.
    auto rows = BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(1 << "b") << BSON_ARRAY(2 << "x"));

    auto [scanSlots, stage] = makeDedupFilterEqBStage(*this, rows);
    auto ctx = makeCompileCtx();
    auto accessors = prepareTree(ctx.get(), stage.get(), sbe::makeSV(scanSlots[0]));

    value::TagValueOwned resultsOwned =
        value::TagValueOwned::fromRaw(getAllResultsMulti(stage.get(), accessors));

    value::TagValueOwned expectedOwned =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(BSON_ARRAY(1))));

    ASSERT_TRUE(valueEquals(
        resultsOwned.tag(), resultsOwned.value(), expectedOwned.tag(), expectedOwned.value()));
}

TEST_F(UniqueRoaringStageWithFilterTest, DuplicateMatchingKeysReturnRecordOnce) {
    // Two identical matching keys should still return the document only once.
    auto rows = BSON_ARRAY(BSON_ARRAY(1 << "b") << BSON_ARRAY(1 << "b"));

    auto [scanSlots, stage] = makeDedupFilterEqBStage(*this, rows);
    auto ctx = makeCompileCtx();
    auto accessors = prepareTree(ctx.get(), stage.get(), sbe::makeSV(scanSlots[0]));

    value::TagValueOwned resultsOwned =
        value::TagValueOwned::fromRaw(getAllResultsMulti(stage.get(), accessors));

    value::TagValueOwned expectedOwned =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(BSON_ARRAY(1))));

    ASSERT_TRUE(valueEquals(
        resultsOwned.tag(), resultsOwned.value(), expectedOwned.tag(), expectedOwned.value()));
}

TEST_F(UniqueRoaringStageWithFilterTest, RecordExcludedWhenNoKeyMatchesFilter) {
    // Neither key matches the filter, so the document should not be returned.
    auto rows = BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(1 << "c"));

    auto [scanSlots, stage] = makeDedupFilterEqBStage(*this, rows);
    auto ctx = makeCompileCtx();
    auto accessors = prepareTree(ctx.get(), stage.get(), sbe::makeSV(scanSlots[0]));

    value::TagValueOwned resultsOwned =
        value::TagValueOwned::fromRaw(getAllResultsMulti(stage.get(), accessors));

    value::TagValueOwned emptyOwned = value::TagValueOwned::fromRaw(value::makeNewArray());

    ASSERT_TRUE(valueEquals(
        resultsOwned.tag(), resultsOwned.value(), emptyOwned.tag(), emptyOwned.value()));

    // Neither key ever passes the filter, so dupsDropped should stay 0.
    auto stats = stage->getStats(/* includeDebugInfo */ true);
    ASSERT_FALSE(stats->debugInfo.isEmpty());
    ASSERT_EQ(stats->debugInfo.getField("dupsTested").numberLong(), 2);
    ASSERT_EQ(stats->debugInfo.getField("filterTested").numberLong(), 2);
    ASSERT_EQ(stats->debugInfo.getField("dupsDropped").numberLong(), 0);
}

TEST_F(UniqueRoaringStageWithFilterTest, FilterIsNotReRunOnKeysOfAlreadyMatchedRecord) {
    // Only the first key ("b") should hit the filter; later keys for the same record should be
    // skipped via the cheap dedup-set lookup instead.
    auto rows = BSON_ARRAY(BSON_ARRAY(1 << "b") << BSON_ARRAY(1 << "z") << BSON_ARRAY(1 << "y"));

    auto [scanSlots, stage] = makeDedupFilterEqBStage(*this, rows);
    auto ctx = makeCompileCtx();
    auto accessors = prepareTree(ctx.get(), stage.get(), sbe::makeSV(scanSlots[0]));

    exhaustStage(stage.get(), accessors[0]);

    auto stats = stage->getStats(/* includeDebugInfo */ true);
    ASSERT_FALSE(stats->debugInfo.isEmpty());
    ASSERT_EQ(stats->debugInfo.getField("dupsTested").numberLong(), 3);
    ASSERT_EQ(stats->debugInfo.getField("filterTested").numberLong(), 1);
    // The other two keys are skipped via the "already seen" path instead of re-running the filter.
    ASSERT_EQ(stats->debugInfo.getField("dupsDropped").numberLong(), 2);
}

TEST_F(UniqueRoaringStageWithFilterTest, ResetsStateAfterClose) {
    auto rows = BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(1 << "b") << BSON_ARRAY(2 << "x"));

    auto [scanSlots, stage] = makeDedupFilterEqBStage(*this, rows);
    auto ctx = makeCompileCtx();
    auto accessors = prepareTree(ctx.get(), stage.get(), sbe::makeSV(scanSlots[0]));

    value::TagValueOwned resultsOwned =
        value::TagValueOwned::fromRaw(getAllResultsMulti(stage.get(), accessors));
    value::TagValueOwned expectedOwned =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(BSON_ARRAY(1))));
    ASSERT_TRUE(valueEquals(
        resultsOwned.tag(), resultsOwned.value(), expectedOwned.tag(), expectedOwned.value()));

    stage->close();
    stage->open(false);

    value::TagValueOwned resetResultsOwned =
        value::TagValueOwned::fromRaw(getAllResultsMulti(stage.get(), accessors));
    ASSERT_TRUE(valueEquals(resetResultsOwned.tag(),
                            resetResultsOwned.value(),
                            expectedOwned.tag(),
                            expectedOwned.value()));
}

TEST_F(UniqueRoaringStageWithFilterTest, ResetsStateAfterReopen) {
    auto rows = BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(1 << "b") << BSON_ARRAY(2 << "x"));

    auto [scanSlots, stage] = makeDedupFilterEqBStage(*this, rows);
    auto ctx = makeCompileCtx();
    auto accessors = prepareTree(ctx.get(), stage.get(), sbe::makeSV(scanSlots[0]));

    value::TagValueOwned resultsOwned =
        value::TagValueOwned::fromRaw(getAllResultsMulti(stage.get(), accessors));
    value::TagValueOwned expectedOwned =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(BSON_ARRAY(1))));
    ASSERT_TRUE(valueEquals(
        resultsOwned.tag(), resultsOwned.value(), expectedOwned.tag(), expectedOwned.value()));

    stage->open(/* reOpen */ true);

    value::TagValueOwned resetResultsOwned =
        value::TagValueOwned::fromRaw(getAllResultsMulti(stage.get(), accessors));
    ASSERT_TRUE(valueEquals(resetResultsOwned.tag(),
                            resetResultsOwned.value(),
                            expectedOwned.tag(),
                            expectedOwned.value()));
}
}  // namespace mongo::sbe
