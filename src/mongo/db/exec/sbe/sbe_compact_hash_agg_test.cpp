// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file contains tests for sbe::CompactHashAggStage.
 */

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/compact_hash_agg.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {
namespace {

class CompactHashAggStageTest : public PlanStageTestFixture {
public:
    void setUp() override {
        PlanStageTestFixture::setUp();
        _globalLock = std::make_unique<Lock::GlobalLock>(operationContext(), MODE_IS);
    }

    void tearDown() override {
        _globalLock.reset();
        PlanStageTestFixture::tearDown();
    }

    // Groups the input by its single field and asserts that the resulting group keys equal
    // 'expectedKeys'. With a collator, each group keeps the first key inserted.
    void performCompactHashAgg(
        BSONArray inputArr,
        std::set<std::string> expectedKeys,
        std::unique_ptr<mongo::CollatorInterfaceMock> optionalCollator = nullptr);

    void checkMemoryStats(mongo::sbe::PlanStage* stage) {
        ASSERT_GT(stage->getMemoryTracker()->peakTrackedMemoryBytes(), 0);
        ASSERT_GT(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
        auto stats = static_cast<const HashAggStats*>(stage->getSpecificStats());
        ASSERT_GT(stats->peakTrackedMemBytes, 0);
    }

    // Drives getNext() to completion and returns the distinct group-by keys.
    std::set<int32_t> collectIntKeys(mongo::sbe::PlanStage* stage, value::SlotAccessor* accessor) {
        std::set<int32_t> results;
        while (stage->getNext() == PlanState::ADVANCED) {
            auto [tag, val] = accessor->getViewOfValue();
            ASSERT_EQ(value::TypeTags::NumberInt32, tag);
            results.insert(value::bitcastTo<int32_t>(val));
        }
        return results;
    }

private:
    std::unique_ptr<Lock::GlobalLock> _globalLock;
};

void CompactHashAggStageTest::performCompactHashAgg(
    BSONArray inputArr,
    std::set<std::string> expectedKeys,
    std::unique_ptr<mongo::CollatorInterfaceMock> optionalCollator) {
    auto [inputTag, inputVal] = stage_builder::makeValue(inputArr);
    value::TagValueOwned inputOwned = value::TagValueOwned::fromRaw(inputTag, inputVal);

    auto collatorSlot = generateSlotId();
    auto shouldUseCollator = optionalCollator != nullptr;

    auto ctx = makeCompileCtx();

    value::OwnedValueAccessor collatorAccessor;
    if (shouldUseCollator) {
        ctx->pushCorrelated(collatorSlot, &collatorAccessor);
        collatorAccessor.reset(value::TypeTags::collator,
                               value::bitcastFrom<CollatorInterface*>(optionalCollator.release()));
    }

    // Generate a mock scan from 'input' with a single output slot.
    auto [scanSlot, scanStage] = generateVirtualScan(std::move(inputOwned));

    auto stage =
        makeS<CompactHashAggStage>(std::move(scanStage),
                                   makeSV(scanSlot),
                                   true,
                                   boost::optional<value::SlotId>{shouldUseCollator, collatorSlot},
                                   nullptr /* yieldPolicy */,
                                   kEmptyPlanNodeId);
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), scanSlot);

    std::set<std::string> results;
    while (stage->getNext() == PlanState::ADVANCED) {
        auto [tag, val] = resultAccessor->getViewOfValue();
        ASSERT_EQ(value::TypeTags::StringSmall, tag);
        results.insert(std::string(value::getStringView(tag, val)));
    }

    ASSERT_EQ(expectedKeys, results);
    ASSERT_GT(stage->getMemoryTracker()->peakTrackedMemoryBytes(), 0);
    ASSERT_GT(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
}

TEST_F(CompactHashAggStageTest, CompactHashAggCollationTest) {
    auto inputArr = BSON_ARRAY("A" << "a"
                                   << "b"
                                   << "c"
                                   << "B");

    // With a collator, "A" groups with "a" and "b" groups with "B".
    auto lowerStringCollator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    performCompactHashAgg(inputArr, {"A", "b", "c"}, std::move(lowerStringCollator));

    // Without a collator, every input value is its own group.
    performCompactHashAgg(inputArr, {"A", "a", "b", "B", "c"});
}

TEST_F(CompactHashAggStageTest, CompactHashAggBasicNoSpill) {
    // The compact stage never spills; with plentiful memory the query completes in memory.
    auto ctx = makeCompileCtx();

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(5 << 6 << 7 << 5 << 6 << 7 << 6 << 7 << 7));
    auto [scanSlot, scanStage] =
        generateVirtualScan(value::TagValueMaybeOwned::fromRaw(true, inputTag, inputVal));

    auto stage = makeS<CompactHashAggStage>(std::move(scanStage),
                                            makeSV(scanSlot),
                                            true,
                                            boost::none,
                                            nullptr /* yieldPolicy */,
                                            kEmptyPlanNodeId);
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), scanSlot);

    // Check that the results match the expected: one group per distinct key.
    auto results = collectIntKeys(stage.get(), resultAccessor);
    ASSERT_EQ(3, results.size());
    ASSERT_EQ(1, results.count(5));
    ASSERT_EQ(1, results.count(6));
    ASSERT_EQ(1, results.count(7));

    // Check that the spilling behavior matches the expected.
    auto stats = static_cast<const HashAggStats*>(stage->getSpecificStats());
    ASSERT_FALSE(stats->usedDisk);
    ASSERT_EQ(0, stats->spillingStats.getSpills());
    ASSERT_EQ(0, stats->spillingStats.getSpilledRecords());

    checkMemoryStats(stage.get());
    stage->close();
}

TEST_F(CompactHashAggStageTest, CompactHashAggNoGroupByProducesSingleGroup) {
    auto ctx = makeCompileCtx();

    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(1.0 << 2.0 << 3.0 << 4.0 << 5.0));
    auto [scanSlot, scanStage] =
        generateVirtualScan(value::TagValueMaybeOwned::fromRaw(true, inputTag, inputVal));

    // Build a CompactHashAggStage with an empty group-by slot list.
    auto stage = makeS<CompactHashAggStage>(std::move(scanStage),
                                            makeSV(),
                                            true,
                                            boost::none,
                                            nullptr /* yieldPolicy */,
                                            kEmptyPlanNodeId);

    // There are no output slots, so prepare the tree without requesting any and count the
    // results by driving getNext() directly.
    prepareTree(ctx.get(), stage.get());
    size_t numResults = 0;
    while (stage->getNext() == PlanState::ADVANCED) {
        ++numResults;
    }

    // All input rows aggregate into a single group.
    ASSERT_EQ(1, numResults);

    // Check that it did not touch disk.
    auto stats = static_cast<const HashAggStats*>(stage->getSpecificStats());
    ASSERT_FALSE(stats->usedDisk);
    ASSERT_EQ(0, stats->spillingStats.getSpills());
    ASSERT_EQ(0, stats->spillingStats.getSpilledRecords());

    // The group-by key is empty, so we never compute an estimate for the amount of memory.
    ASSERT_EQ(stage->getMemoryTracker()->peakTrackedMemoryBytes(), 0);
    ASSERT_EQ(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);

    stage->close();
}

TEST_F(CompactHashAggStageTest, CompactHashAggCloneTest) {
    auto ctx = makeCompileCtx();

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(5 << 6 << 7 << 5 << 6 << 7 << 6 << 7 << 7));
    auto [scanSlot, scanStage] =
        generateVirtualScan(value::TagValueMaybeOwned::fromRaw(true, inputTag, inputVal));

    auto stage = makeS<CompactHashAggStage>(std::move(scanStage),
                                            makeSV(scanSlot),
                                            true,
                                            boost::none,
                                            nullptr /* yieldPolicy */,
                                            kEmptyPlanNodeId);
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), scanSlot);
    auto results = collectIntKeys(stage.get(), resultAccessor);
    stage->close();

    // The clone is independently prepared and produces the same groups.
    auto clonedCtx = makeCompileCtx();
    auto clone = stage->clone();
    auto cloneAccessor = prepareTree(clonedCtx.get(), clone.get(), scanSlot);
    auto clonedResults = collectIntKeys(clone.get(), cloneAccessor);
    clone->close();

    ASSERT_EQ(results, clonedResults);
    ASSERT_EQ(3, results.size());
    ASSERT_EQ(1, results.count(5));
    ASSERT_EQ(1, results.count(6));
    ASSERT_EQ(1, results.count(7));
}

TEST_F(CompactHashAggStageTest, CompactHashAggWithRecordIds) {
    auto ctx = makeCompileCtx();

    // Build a scan of a few record ids.
    std::vector<int64_t> ids{10, 999, 10, 999, 1, 999, 8589869056, 999, 10, 8589869056};
    auto [inputTag, inputVal] = sbe::value::makeNewArray();
    auto testData = sbe::value::getArrayView(inputVal);
    for (auto id : ids) {
        auto [ridTag, ridVal] = sbe::value::makeNewRecordId(id);
        testData->push_back_raw(ridTag, ridVal);
    }
    auto [scanSlot, scanStage] =
        generateVirtualScan(value::TagValueMaybeOwned::fromRaw(true, inputTag, inputVal));

    auto stage = makeS<CompactHashAggStage>(std::move(scanStage),
                                            makeSV(scanSlot),
                                            true,
                                            boost::none,
                                            nullptr /* yieldPolicy */,
                                            kEmptyPlanNodeId);
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), scanSlot);

    // Read in all of the results.
    std::set<int64_t> results;
    while (stage->getNext() == PlanState::ADVANCED) {
        auto [resScanTag, resScanVal] = resultAccessor->getViewOfValue();
        ASSERT_EQ(value::TypeTags::RecordId, resScanTag);
        ASSERT_TRUE(results
                        .insert(value::bitcastFrom<int64_t>(
                            sbe::value::getRecordIdView(resScanVal)->getLong()))
                        .second);
    }

    // Assert that the results are as expected: one group per distinct record id.
    ASSERT_EQ(4, results.size());
    ASSERT_EQ(1, results.count(1));
    ASSERT_EQ(1, results.count(10));
    ASSERT_EQ(1, results.count(999));
    ASSERT_EQ(1, results.count(8589869056));

    checkMemoryStats(stage.get());
    stage->close();
}

TEST_F(CompactHashAggStageTest, CompactHashAggMemoryLimitNoDiskUse) {
    // Set the memory threshold low enough that only a couple of groups fit in memory. The
    // compact stage does not allow disk use, so exceeding the budget must fail the query with
    // 'QueryExceededMemoryLimitNoDiskUseAllowed' instead of spilling.
    unittest::ServerParameterGuard maxMemoryLimit(
        "internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill", 64);

    auto ctx = makeCompileCtx();

    // Build a scan of the [5,6,7,5,6,7,6,7,7] input array.
    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(5 << 6 << 7 << 5 << 6 << 7 << 6 << 7 << 7));
    auto [scanSlot, scanStage] =
        generateVirtualScan(value::TagValueMaybeOwned::fromRaw(true, inputTag, inputVal));

    auto stage = makeS<CompactHashAggStage>(std::move(scanStage),
                                            makeSV(scanSlot),
                                            true,
                                            boost::none,
                                            nullptr /* yieldPolicy */,
                                            kEmptyPlanNodeId);

    // The stage must throw QueryExceededMemoryLimitNoDiskUseAllowed while building the hash
    // table, instead of spilling.
    ASSERT_THROWS_CODE(
        [&] {
            auto resultAccessor = prepareTree(ctx.get(), stage.get(), scanSlot);
            getAllResults(stage.get(), resultAccessor);
        }(),
        DBException,
        ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed);
}

}  // namespace
}  // namespace mongo::sbe
