// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file contains tests for sbe::HashAggStage.
 */

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/fetch.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_join.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>


namespace mongo::sbe {

using TrialRunTrackerTest = PlanStageTestFixture;

TEST_F(TrialRunTrackerTest, TrackerAttachesToStreamingStage) {
    auto collUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();
    auto scanStage = makeS<sbe::ScanStage>(collUuid,
                                           DatabaseName(),
                                           /*recordSlot=*/generateSlotId(),
                                           /*recordIdSlot=*/generateSlotId(),
                                           /*snapshotIdSlot=*/generateSlotId(),
                                           /*indexIdSlot=*/generateSlotId(),
                                           /*indexKeySlot=*/generateSlotId(),
                                           /*indexKeyPatternSlot=*/generateSlotId(),
                                           /*scanFieldNames=*/std::vector<std::string>{"field"},
                                           /*scanFieldSlots=*/makeSV(generateSlotId()),
                                           /*minRecordIdSlot=*/generateSlotId(),
                                           /*maxRecordIdSlot=*/generateSlotId(),
                                           /*forward=*/true,
                                           /*yieldPolicy=*/nullptr,
                                           /*nodeId=*/kEmptyPlanNodeId,
                                           /*scanOpenCallback=*/nullptr);

    auto tracker =
        std::make_unique<TrialRunTracker>(boost::none, boost::none, boost::none, boost::none);
    ON_BLOCK_EXIT([&]() { scanStage->detachFromTrialRunTracker(); });

    auto attachResult = scanStage->attachToTrialRunTracker(tracker.get());
    ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::TrackReads);
}

TEST_F(TrialRunTrackerTest, TrackerAttachesToFetchStage) {
    auto collUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();
    auto fetchState = std::make_shared<FetchStageState>(generateSlotId(),
                                                        generateSlotId(),
                                                        generateSlotId(),
                                                        generateSlotId(),
                                                        generateSlotId(),
                                                        generateSlotId(),
                                                        generateSlotId(),
                                                        StringListSet({}),
                                                        value::SlotVector(),
                                                        FetchCallbacks());
    auto fetchStage = makeS<sbe::FetchStage>(makeS<CoScanStage>(kEmptyPlanNodeId),
                                             collUuid,
                                             DatabaseName(),
                                             fetchState,
                                             nullptr,
                                             kEmptyPlanNodeId,
                                             true);

    auto tracker =
        std::make_unique<TrialRunTracker>(boost::none, boost::none, boost::none, boost::none);
    ON_BLOCK_EXIT([&]() { fetchStage->detachFromTrialRunTracker(); });

    auto attachResult = fetchStage->attachToTrialRunTracker(tracker.get());
    ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::TrackReads);
}

TEST_F(TrialRunTrackerTest, TrackerDoesNotAttachToBlockingStage) {
    auto sortStage =
        makeS<SortStage>(makeS<LimitSkipStage>(makeS<CoScanStage>(kEmptyPlanNodeId),
                                               makeE<EConstant>(value::TypeTags::NumberInt64, 0),
                                               nullptr,
                                               kEmptyPlanNodeId),
                         makeSV(),
                         std::vector<value::SortDirection>{},
                         makeSV(),
                         /*limit=*/nullptr,
                         204857600,
                         false,
                         /*yieldPolicy=*/nullptr,
                         kEmptyPlanNodeId);

    auto tracker =
        std::make_unique<TrialRunTracker>(boost::none, boost::none, boost::none, boost::none);
    ON_BLOCK_EXIT([&]() { sortStage->detachFromTrialRunTracker(); });

    auto attachResult = sortStage->attachToTrialRunTracker(tracker.get());
    ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::NoTracking);
}

TEST_F(TrialRunTrackerTest, TrackerAttachesToBothBlockingAndStreamingStages) {
    auto collUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();
    auto scanStage = makeS<sbe::ScanStage>(collUuid,
                                           DatabaseName(),
                                           /*recordSlot=*/generateSlotId(),
                                           /*recordIdSlot=*/generateSlotId(),
                                           /*snapshotIdSlot=*/generateSlotId(),
                                           /*indexIdSlot=*/generateSlotId(),
                                           /*indexKeySlot=*/generateSlotId(),
                                           /*indexKeyPatternSlot=*/generateSlotId(),
                                           /*scanFieldNames=*/std::vector<std::string>{"field"},
                                           /*scanFieldSlots=*/makeSV(generateSlotId()),
                                           /*minRecordIdSlot=*/generateSlotId(),
                                           /*maxRecordIdSlot=*/generateSlotId(),
                                           /*forward=*/true,
                                           /*yieldPolicy=*/nullptr,
                                           /*nodeId=*/kEmptyPlanNodeId,
                                           /*scanOpenCallback=*/nullptr);

    auto rootSortStage = makeS<SortStage>(std::move(scanStage),
                                          makeSV(),
                                          std::vector<value::SortDirection>{},
                                          makeSV(),
                                          /*limit=*/nullptr,
                                          204857600,
                                          false,
                                          /*yieldPolicy=*/nullptr,
                                          kEmptyPlanNodeId);

    auto tracker =
        std::make_unique<TrialRunTracker>(boost::none, boost::none, boost::none, boost::none);
    ON_BLOCK_EXIT([&]() { rootSortStage->detachFromTrialRunTracker(); });

    auto attachResult = rootSortStage->attachToTrialRunTracker(tracker.get());
    ASSERT_EQ(attachResult,
              PlanStage::TrialRunTrackingType::TrackReads |
                  PlanStage::TrialRunTrackingType::NoTracking);
}

TEST_F(TrialRunTrackerTest, TrialRunTrackingCanBeDisabled) {
    auto scanStage =
        makeS<sbe::ScanStage>(UUID::parse("00000000-0000-0000-0000-000000000000").getValue(),
                              DatabaseName(),
                              /*recordSlot=*/generateSlotId(),
                              /*recordIdSlot=*/generateSlotId(),
                              /*snapshotIdSlot=*/generateSlotId(),
                              /*indexIdSlot=*/generateSlotId(),
                              /*indexKeySlot=*/generateSlotId(),
                              /*indexKeyPatternSlot=*/generateSlotId(),
                              /*scanFieldNames=*/std::vector<std::string>{"field"},
                              /*scanFieldSlots=*/makeSV(generateSlotId()),
                              /*minRecordIdSlot=*/generateSlotId(),
                              /*maxRecordIdSlot=*/generateSlotId(),
                              /*forward=*/true,
                              /*yieldPolicy=*/nullptr,
                              /*nodeId=*/kEmptyPlanNodeId,
                              /*scanOpenCallback=*/nullptr);

    scanStage->disableTrialRunTracking();
    auto tracker =
        std::make_unique<TrialRunTracker>(boost::none, boost::none, boost::none, boost::none);
    auto attachResult = scanStage->attachToTrialRunTracker(tracker.get());
    ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::NoTracking);
}

TEST_F(TrialRunTrackerTest, DisablingTrackingForChildDoesNotInhibitTrackingForParent) {
    auto scanStage =
        makeS<sbe::ScanStage>(UUID::parse("00000000-0000-0000-0000-000000000000").getValue(),
                              DatabaseName(),
                              /*recordSlot=*/generateSlotId(),
                              /*recordIdSlot=*/generateSlotId(),
                              /*snapshotIdSlot=*/generateSlotId(),
                              /*indexIdSlot=*/generateSlotId(),
                              /*indexKeySlot=*/generateSlotId(),
                              /*indexKeyPatternSlot=*/generateSlotId(),
                              /*scanFieldNames=*/std::vector<std::string>{"field"},
                              /*scanFieldSlots=*/makeSV(generateSlotId()),
                              /*minRecordIdSlot=*/generateSlotId(),
                              /*maxRecordIdSlot=*/generateSlotId(),
                              /*forward=*/true,
                              /*yieldPolicy=*/nullptr,
                              /*nodeId=*/kEmptyPlanNodeId,
                              /*scanOpenCallback=*/nullptr);

    // Disable tracking for 'scanStage'. We should still attach the tracker for 'rootSortStage'.
    scanStage->disableTrialRunTracking();

    auto rootSortStage = makeS<SortStage>(std::move(scanStage),
                                          makeSV(),
                                          std::vector<value::SortDirection>{},
                                          makeSV(),
                                          /*limit=*/nullptr,
                                          204857600,
                                          false,
                                          /*yieldPolicy=*/nullptr,
                                          kEmptyPlanNodeId);

    auto tracker =
        std::make_unique<TrialRunTracker>(boost::none, boost::none, boost::none, boost::none);
    ON_BLOCK_EXIT([&]() { rootSortStage->detachFromTrialRunTracker(); });

    auto attachResult = rootSortStage->attachToTrialRunTracker(tracker.get(), PlanNodeId{1});
    ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::TrackResults);
}

TEST_F(TrialRunTrackerTest, DisablingTrackingForAChildStagePreventsEarlyExit) {
    auto ctx = makeCompileCtx();

    // This PlanStage tree allows us to observe what happens when we attach a TrialRunTracker to
    // one of two sibling HashAgg stages.
    auto buildHashAgg = [&]() {
        auto [inputTag, inputVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 4 << 5));
        auto [scanSlot, scanStage] =
            generateVirtualScan(value::TagValueMaybeOwned::fromRaw(true, inputTag, inputVal));

        // Build a HashAggStage, group by the scanSlot and compute a simple count.
        auto countsSlot = generateSlotId();
        auto hashAggStage = makeS<HashAggStage>(
            std::move(scanStage),
            makeSV(scanSlot),
            makeHashAggAccumulatorList(std::make_unique<CompiledHashAggAccumulator>(
                countsSlot,
                generateSlotId(),
                makeFunction(
                    EFn::kSum,
                    makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1))),
                makeFunction(EFn::kFail,
                             makeInt32Constant(ErrorCodes::NotImplemented),
                             makeStringConstant("Unexpected merge with allowDiskUse=false")))),
            true,
            boost::none,
            /*allowDiskUse=*/false,
            /*yieldPolicy=*/nullptr,
            kEmptyPlanNodeId);

        return std::make_pair(countsSlot, std::move(hashAggStage));
    };

    auto [leftCountsSlot, leftHashAggStage] = buildHashAgg();
    auto [rightCountsSlot, rightHashAggStage] = buildHashAgg();

    // Disable trial run tracking for the right HashAgg stage.
    rightHashAggStage->disableTrialRunTracking();
    auto resultSlot = generateSlotId();
    auto unionStage = makeS<UnionStage>(
        makeSs(std::move(leftHashAggStage), std::move(rightHashAggStage)),
        std::vector<value::SlotVector>{makeSV(leftCountsSlot), makeSV(rightCountsSlot)},
        makeSV(resultSlot),
        kEmptyPlanNodeId);

    // The blocking SortStage at the root ensures that both of the child HashAgg stages will be
    // opened during the open phase of the root stage.
    auto sortStage =
        makeS<SortStage>(std::move(unionStage),
                         makeSV(resultSlot),
                         std::vector<value::SortDirection>{value::SortDirection::Ascending},
                         makeSV(),
                         /*limit=*/nullptr,
                         204857600,
                         false,
                         /*yieldPolicy=*/nullptr,
                         kEmptyPlanNodeId);

    // We expect the TrialRunTracker to attach to _only_ the left child.
    auto tracker =
        std::make_unique<TrialRunTracker>(size_t{9}, boost::none, boost::none, boost::none);
    auto attachResult = sortStage->attachToTrialRunTracker(tracker.get());

    // Note: A scan is a streaming stage, but the "virtual scan" used here does not attach to the
    // tracker.
    ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::NoTracking);

    // The 'prepareTree()' function opens the SortStage, causing it to read documents from its
    // child. Because only one of the HashAgg stages is attached to the TrialRunTracker, the
    // 'numResults' metric will not be incremented enough to end the trial. As such, this call to
    // 'prepareTree()' will not end the trial.
    prepareTree(ctx.get(), sortStage.get(), resultSlot);
}

TEST_F(TrialRunTrackerTest, TrackerAttachesToPlanningRootStageAndTracksTheDocumentsReturnedByIt) {
    auto ctx = makeCompileCtx();

    auto [inputTag, inputVal] = stage_builder::makeValue(
        BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(3 << 4) << BSON_ARRAY(5 << 6)));
    auto [scanSlot, scanStage] = generateVirtualScan(
        value::TagValueMaybeOwned::fromRaw(true, inputTag, inputVal), PlanNodeId{1});
    auto unwindSlot = generateSlotId();
    auto unwindStage = makeS<UnwindStage>(
        std::move(scanStage), scanSlot, generateSlotId(), unwindSlot, true, PlanNodeId{2});
    auto sortStage =
        makeS<SortStage>(std::move(unwindStage),
                         makeSV(unwindSlot),
                         std::vector<value::SortDirection>{value::SortDirection::Ascending},
                         makeSV(),
                         /*limit=*/nullptr,
                         1024 * 1024,
                         false,
                         /*yieldPolicy=*/nullptr,
                         PlanNodeId{3});

    auto tracker =
        std::make_unique<TrialRunTracker>(size_t{3}, boost::none, boost::none, boost::none);
    auto attachResult = sortStage->attachToTrialRunTracker(tracker.get());
    ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::NoTracking);

    // The 'prepareTree()' function opens the SortStage, causing it to read documents from its
    // child. Only 3 documents should be returned by scan stage, which is marked as planning root,
    // but sort should have 6 results as input because of unwind.
    prepareTree(ctx.get(), sortStage.get(), unwindSlot);
    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumResults>(), 0);
}

TEST_F(TrialRunTrackerTest, TrackerCanTrackMetricWithMaxMetricSetToZero) {
    auto ctx = makeCompileCtx();

    auto [inputTag, inputVal] = stage_builder::makeValue(
        BSON_ARRAY(BSON("a" << 1) << BSON("a" << 2) << BSON("a" << 3) << BSON("a" << 4)));
    auto [scanSlot, scanStage] = generateVirtualScan(
        value::TagValueMaybeOwned::fromRaw(true, inputTag, inputVal), PlanNodeId{1});
    auto sortStage =
        makeS<SortStage>(std::move(scanStage),
                         makeSV(scanSlot),
                         std::vector<value::SortDirection>{value::SortDirection::Ascending},
                         makeSV(),
                         /*limit=*/nullptr,
                         1024 * 1024,
                         false,
                         /*yieldPolicy=*/nullptr,
                         PlanNodeId{3});

    auto onMetricReached = [&](TrialRunTracker::TrialRunMetric metric) {
        // This callback should get called for the kNumResults metric.
        ASSERT_EQ(metric, TrialRunTracker::kNumResults);
        // Detach the TrialRunTracker and allow the plan to continue running.
        sortStage->detachFromTrialRunTracker();
        return false;
    };

    auto tracker = std::make_unique<TrialRunTracker>(
        std::move(onMetricReached), size_t{0}, boost::none, boost::none, boost::none);
    auto attachResult = sortStage->attachToTrialRunTracker(tracker.get(), PlanNodeId{1});
    ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::TrackResults);

    // The 'prepareTree()' function opens the SortStage, causing it to read documents from its
    // child. The scan stage (which is marked as the planning root) should return 4 documents.
    // The tracker should only observe the first document, because the callback we supplied should
    // detach the tracker after the first document is observed (because that is precisely when
    // 'tracker->getMetric<TrialRunTracker::kNumResults>()' becomes greater than the maximum of 0.)
    prepareTree(ctx.get(), sortStage.get(), scanSlot);
    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumResults>(), 1);
}

TEST_F(TrialRunTrackerTest, IntermediateRowsBreachTheirOwnLimit) {
    // maxNumIntermediateRows = 10; the trial ends once the metric exceeds it.
    auto tracker =
        std::make_unique<TrialRunTracker>(boost::none, boost::none, size_t{10}, boost::none);

    ASSERT_FALSE(tracker->trackProgress<TrialRunTracker::kNumIntermediateRows>(10));
    ASSERT_TRUE(tracker->trackProgress<TrialRunTracker::kNumIntermediateRows>(1));

    // Once the trial has ended, further progress is reported but not recorded.
    ASSERT_TRUE(tracker->trackProgress<TrialRunTracker::kNumIntermediateRows>(1));
    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumIntermediateRows>(), 11);
}

TEST_F(TrialRunTrackerTest, SpillsAreTrackedWhenUncapped) {
    // SIZE_MAX is effectively no limit: spills are recorded but never end the trial.
    auto tracker = std::make_unique<TrialRunTracker>(
        boost::none, boost::none, boost::none, std::numeric_limits<size_t>::max());

    ASSERT_FALSE(tracker->trackProgress<TrialRunTracker::kNumSpills>(1));
    ASSERT_FALSE(tracker->trackProgress<TrialRunTracker::kNumSpills>(1));
    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumSpills>(), 2);
}

TEST_F(TrialRunTrackerTest, SpillsBreachWhenMaxIsZero) {
    // maxNumSpills = 0: any spill ends the trial.
    auto tracker =
        std::make_unique<TrialRunTracker>(boost::none, boost::none, boost::none, size_t{0});

    ASSERT_TRUE(tracker->trackProgress<TrialRunTracker::kNumSpills>(1));
}

namespace {

/**
 * Builds a HashJoinStage over two virtual scans (each with a key and a project slot), plus the
 * stage's output slots.
 */
std::pair<value::SlotVector, std::unique_ptr<PlanStage>> makeHashJoinStageWithTracking(
    PlanStageTestFixture* fixture,
    const BSONArray& outerDocs,
    const BSONArray& innerDocs,
    IntermediateTrackingBehavior intermediateTracking,
    bool allowDiskUse = true) {
    auto [outerTag, outerVal] = stage_builder::makeValue(outerDocs);
    auto [outerSlots, outerStage] = fixture->generateVirtualScanMulti(2, outerTag, outerVal);
    auto [innerTag, innerVal] = stage_builder::makeValue(innerDocs);
    auto [innerSlots, innerStage] = fixture->generateVirtualScanMulti(2, innerTag, innerVal);

    value::SlotVector outputSlots{innerSlots[0], outerSlots[0], innerSlots[1], outerSlots[1]};

    auto stage = makeS<HashJoinStage>(std::move(outerStage),
                                      std::move(innerStage),
                                      makeSV(outerSlots[0]),
                                      makeSV(outerSlots[1]),
                                      makeSV(innerSlots[0]),
                                      makeSV(innerSlots[1]),
                                      boost::none /* collatorSlot */,
                                      allowDiskUse,
                                      nullptr /* yieldPolicy */,
                                      kEmptyPlanNodeId,
                                      boost::none /* estimatedBuildCardinality */,
                                      true /* participateInTrialRunTracking */,
                                      intermediateTracking);
    return {std::move(outputSlots), std::move(stage)};
}

/** A hash join that spills, plus the guard that installs the tiny memory limit. */
struct SpillingHashJoin {
    unittest::ServerParameterGuard maxMemoryLimit;
    value::SlotVector outputSlots;
    std::unique_ptr<PlanStage> stage;
};

SpillingHashJoin makeSpillingHashJoinStage(PlanStageTestFixture* fixture,
                                           IntermediateTrackingBehavior intermediateTracking) {
    // Install the tiny limit before the stage is prepared.
    unittest::ServerParameterGuard maxMemoryLimit(
        "internalQuerySlotBasedExecutionHashJoinApproxMemoryUseInBytesBeforeSpill",
        static_cast<long long>(16));

    BSONArrayBuilder innerBuilder;
    for (int i = 0; i < 50; ++i) {
        innerBuilder.append(BSON_ARRAY(i << ("big_string_" + std::to_string(i))));
    }

    auto [outputSlots, stage] = makeHashJoinStageWithTracking(fixture,
                                                              BSON_ARRAY(BSON_ARRAY(1 << "x")),
                                                              innerBuilder.arr(),
                                                              intermediateTracking,
                                                              true /* allowDiskUse */);
    return {std::move(maxMemoryLimit), std::move(outputSlots), std::move(stage)};
}

/** Builds a LoopJoinStage over a 3x3 cross product of two virtual scans, plus its outer slot. */
std::pair<value::SlotId, std::unique_ptr<PlanStage>> makeLoopJoinStageWithTracking(
    PlanStageTestFixture* fixture, IntermediateTrackingBehavior intermediateTracking) {
    auto [outerTag, outerVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3));
    auto [outerSlot, outerStage] = fixture->generateVirtualScan(
        value::TagValueMaybeOwned::fromRaw(true, outerTag, outerVal), PlanNodeId{1});
    auto [innerTag, innerVal] = stage_builder::makeValue(BSON_ARRAY(4 << 5 << 6));
    auto [innerSlot, innerStage] = fixture->generateVirtualScan(
        value::TagValueMaybeOwned::fromRaw(true, innerTag, innerVal), PlanNodeId{2});

    auto stage = makeS<LoopJoinStage>(std::move(outerStage),
                                      std::move(innerStage),
                                      makeSV(outerSlot) /* outerProjects */,
                                      makeSV() /* outerCorrelated */,
                                      makeSV() /* innerProjects */,
                                      nullptr /* predicate */,
                                      JoinType::Inner,
                                      PlanNodeId{3},
                                      true /* participateInTrialRunTracking */,
                                      intermediateTracking);
    return {outerSlot, std::move(stage)};
}

}  // namespace

TEST_F(TrialRunTrackerTest, TrackerTracksIntermediateRowsFromMarkedLoopJoinStage) {
    auto ctx = makeCompileCtx();

    // maxNumIntermediateRows = SIZE_MAX: record intermediate rows without a limit.
    auto tracker = std::make_unique<TrialRunTracker>(
        boost::none, boost::none, std::numeric_limits<size_t>::max(), boost::none);

    auto [outerSlot, loopJoinStage] =
        makeLoopJoinStageWithTracking(this, IntermediateTrackingBehavior::kTrackIntermediateRows);

    ASSERT_EQ(loopJoinStage->attachToTrialRunTracker(tracker.get()),
              PlanStage::TrialRunTrackingType::TrackIntermediateRows);

    // A 3x3 cross product produces 9 intermediate rows, and the virtual scans perform no reads.
    auto resultAccessor = prepareTree(ctx.get(), loopJoinStage.get(), outerSlot);
    exhaustStage(loopJoinStage.get(), resultAccessor);

    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumIntermediateRows>(), 9);
    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumReads>(), 0);
}

TEST_F(TrialRunTrackerTest, TrackerDoesNotTrackIntermediateRowsFromUnmarkedLoopJoinStage) {
    auto ctx = makeCompileCtx();

    auto tracker =
        std::make_unique<TrialRunTracker>(boost::none, boost::none, boost::none, boost::none);

    auto [outerSlot, loopJoinStage] =
        makeLoopJoinStageWithTracking(this, IntermediateTrackingBehavior::kNone);

    ASSERT_EQ(loopJoinStage->attachToTrialRunTracker(tracker.get()),
              PlanStage::TrialRunTrackingType::NoTracking);

    auto resultAccessor = prepareTree(ctx.get(), loopJoinStage.get(), outerSlot);
    exhaustStage(loopJoinStage.get(), resultAccessor);

    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumIntermediateRows>(), 0);
}

TEST_F(TrialRunTrackerTest, NestedLoopJoinBreachesIntermediateRowLimit) {
    auto ctx = makeCompileCtx();

    // maxNumIntermediateRows = 2: the 3x3 cross product breaches it.
    auto tracker =
        std::make_unique<TrialRunTracker>(boost::none, boost::none, size_t{2}, boost::none);

    auto [outerSlot, loopJoinStage] =
        makeLoopJoinStageWithTracking(this, IntermediateTrackingBehavior::kTrackIntermediateRows);

    ASSERT_EQ(loopJoinStage->attachToTrialRunTracker(tracker.get()),
              PlanStage::TrialRunTrackingType::TrackIntermediateRows);

    ASSERT_THROWS_CODE(
        [&] {
            auto resultAccessor = prepareTree(ctx.get(), loopJoinStage.get(), outerSlot);
            exhaustStage(loopJoinStage.get(), resultAccessor);
        }(),
        DBException,
        ErrorCodes::QueryTrialRunCompleted);
}

TEST_F(TrialRunTrackerTest, TrackerTracksIntermediateRowsFromMarkedHashJoinStage) {
    auto ctx = makeCompileCtx();

    // maxNumIntermediateRows = SIZE_MAX: record intermediate rows without a limit.
    auto tracker = std::make_unique<TrialRunTracker>(
        boost::none, boost::none, std::numeric_limits<size_t>::max(), boost::none);

    auto [outputSlots, hashJoinStage] = makeHashJoinStageWithTracking(
        this,
        BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(2 << "b") << BSON_ARRAY(3 << "c")),
        BSON_ARRAY(BSON_ARRAY(2 << "x") << BSON_ARRAY(3 << "y") << BSON_ARRAY(4 << "z")),
        IntermediateTrackingBehavior::kTrackIntermediateRows);

    ASSERT_EQ(hashJoinStage->attachToTrialRunTracker(tracker.get()),
              PlanStage::TrialRunTrackingType::TrackIntermediateRows);

    // Keys 2 and 3 match, producing 2 intermediate rows.
    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);
    value::TagValueOwned::fromRaw(getAllResultsMulti(hashJoinStage.get(), resultAccessors));

    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumIntermediateRows>(), 2);
}

TEST_F(TrialRunTrackerTest, ClonedJoinStagePreservesIntermediateTracking) {
    auto tracker = std::make_unique<TrialRunTracker>(
        boost::none, boost::none, std::numeric_limits<size_t>::max(), boost::none);

    auto hashJoin =
        makeHashJoinStageWithTracking(this,
                                      BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(2 << "b")),
                                      BSON_ARRAY(BSON_ARRAY(2 << "x") << BSON_ARRAY(3 << "y")),
                                      IntermediateTrackingBehavior::kTrackIntermediateRows);
    auto clonedStage = hashJoin.second->clone();

    // The clone must stay marked, otherwise it silently stops reporting intermediate rows.
    ASSERT_EQ(clonedStage->attachToTrialRunTracker(tracker.get()),
              PlanStage::TrialRunTrackingType::TrackIntermediateRows);
}

TEST_F(TrialRunTrackerTest, ClonedLoopJoinStagePreservesIntermediateTracking) {
    auto tracker = std::make_unique<TrialRunTracker>(
        boost::none, boost::none, std::numeric_limits<size_t>::max(), boost::none);

    auto loopJoin =
        makeLoopJoinStageWithTracking(this, IntermediateTrackingBehavior::kTrackIntermediateRows);
    auto clonedStage = loopJoin.second->clone();

    // The clone must stay marked, otherwise it silently stops reporting intermediate rows.
    ASSERT_EQ(clonedStage->attachToTrialRunTracker(tracker.get()),
              PlanStage::TrialRunTrackingType::TrackIntermediateRows);
}

TEST_F(TrialRunTrackerTest, ClonedUnmarkedJoinStageStaysUnmarked) {
    auto ctx = makeCompileCtx();

    auto tracker = std::make_unique<TrialRunTracker>(
        boost::none, boost::none, std::numeric_limits<size_t>::max(), boost::none);

    auto [outputSlots, hashJoinStage] =
        makeHashJoinStageWithTracking(this,
                                      BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(2 << "b")),
                                      BSON_ARRAY(BSON_ARRAY(2 << "x") << BSON_ARRAY(3 << "y")),
                                      IntermediateTrackingBehavior::kNone);
    auto clonedStage = hashJoinStage->clone();

    // An unmarked stage's clone must stay unmarked too.
    ASSERT_EQ(clonedStage->attachToTrialRunTracker(tracker.get()),
              PlanStage::TrialRunTrackingType::NoTracking);

    auto resultAccessors = prepareTree(ctx.get(), clonedStage.get(), outputSlots);
    value::TagValueOwned::fromRaw(getAllResultsMulti(clonedStage.get(), resultAccessors));

    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumIntermediateRows>(), 0);
}

TEST_F(TrialRunTrackerTest, TrackerReportsSpillFromMarkedHashJoinStage) {
    auto ctx = makeCompileCtx();
    // maxNumSpills = SIZE_MAX: record spills without a limit.
    auto tracker = std::make_unique<TrialRunTracker>(
        boost::none, boost::none, boost::none, std::numeric_limits<size_t>::max());
    auto spilling =
        makeSpillingHashJoinStage(this, IntermediateTrackingBehavior::kTrackIntermediateRows);

    spilling.stage->attachToTrialRunTracker(tracker.get());

    auto resultAccessors = prepareTree(ctx.get(), spilling.stage.get(), spilling.outputSlots);
    value::TagValueOwned::fromRaw(getAllResultsMulti(spilling.stage.get(), resultAccessors));

    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumSpills>(), 1);
}

TEST_F(TrialRunTrackerTest, TrackerDoesNotReportSpillFromUnmarkedHashJoinStage) {
    auto ctx = makeCompileCtx();
    auto tracker = std::make_unique<TrialRunTracker>(
        boost::none, boost::none, boost::none, std::numeric_limits<size_t>::max());
    auto spilling = makeSpillingHashJoinStage(this, IntermediateTrackingBehavior::kNone);

    spilling.stage->attachToTrialRunTracker(tracker.get());

    auto resultAccessors = prepareTree(ctx.get(), spilling.stage.get(), spilling.outputSlots);
    value::TagValueOwned::fromRaw(getAllResultsMulti(spilling.stage.get(), resultAccessors));

    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumSpills>(), 0);
}

TEST_F(TrialRunTrackerTest, DisallowedSpillFromMarkedHashJoinTerminatesTrial) {
    auto ctx = makeCompileCtx();
    // maxNumSpills = 0: any spill ends the trial.
    auto tracker =
        std::make_unique<TrialRunTracker>(boost::none, boost::none, boost::none, size_t{0});
    auto spilling =
        makeSpillingHashJoinStage(this, IntermediateTrackingBehavior::kTrackIntermediateRows);

    spilling.stage->attachToTrialRunTracker(tracker.get());

    ASSERT_THROWS_CODE(
        [&] {
            auto resultAccessors =
                prepareTree(ctx.get(), spilling.stage.get(), spilling.outputSlots);
            getAllResultsMulti(spilling.stage.get(), resultAccessors);
        }(),
        DBException,
        ErrorCodes::QueryTrialRunCompleted);
}

TEST(TrialRunTrackerUncappedTest, RecordsEveryMetricWithoutEndingTheTrial) {
    auto tracker = TrialRunTracker::makeUncapped();
    ASSERT_FALSE(tracker->trackProgress<TrialRunTracker::kNumResults>(1000000));
    ASSERT_FALSE(tracker->trackProgress<TrialRunTracker::kNumReads>(1000000));
    ASSERT_FALSE(tracker->trackProgress<TrialRunTracker::kNumIntermediateRows>(1000000));
    ASSERT_FALSE(tracker->trackProgress<TrialRunTracker::kNumSpills>(1000000));
    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumResults>(), 1000000);
    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumReads>(), 1000000);
    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumIntermediateRows>(), 1000000);
    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumSpills>(), 1000000);
}
}  // namespace mongo::sbe
