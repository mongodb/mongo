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

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

namespace mongo::sbe {

using TrialRunTrackerTest = PlanStageTestFixture;

TEST_F(TrialRunTrackerTest, TrackerAttachesToStreamingStage) {
    auto collUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();
    auto scanStage = makeS<sbe::ScanStage>(collUuid,
                                           DatabaseName(),
                                           generateSlotId() /* recordSlot */,
                                           generateSlotId() /* recordIdSlot */,
                                           generateSlotId() /* snapshotIdSlot */,
                                           generateSlotId() /* indexIdSlot */,
                                           generateSlotId() /* indexKeySlot */,
                                           generateSlotId() /* indexKeyPatternSlot */,
                                           boost::none /* oplogTsSlot */,
                                           std::vector<std::string>{"field"} /* scanFieldNames */,
                                           makeSV(generateSlotId()) /* scanFieldSlots */,
                                           generateSlotId() /* seekRecordIdSlot */,
                                           generateSlotId() /* minRecordIdSlot */,
                                           generateSlotId() /* maxRecordIdSlot */,
                                           true /* forward */,
                                           nullptr /* yieldPolicy */,
                                           kEmptyPlanNodeId /* nodeId */,
                                           ScanCallbacks());

    auto tracker = std::make_unique<TrialRunTracker>(boost::none, boost::none, boost::none);
    ON_BLOCK_EXIT([&]() { scanStage->detachFromTrialRunTracker(); });

    auto attachResult = scanStage->attachToTrialRunTracker(tracker.get());
    ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::TrackReads);
}

TEST_F(TrialRunTrackerTest, TrackerAttachesToBlockingStage) {
    auto sortStage =
        makeS<SortStage>(makeS<LimitSkipStage>(makeS<CoScanStage>(kEmptyPlanNodeId),
                                               makeE<EConstant>(value::TypeTags::NumberInt64, 0),
                                               nullptr,
                                               kEmptyPlanNodeId),
                         makeSV(),
                         std::vector<value::SortDirection>{},
                         makeSV(),
                         nullptr /*limit*/,
                         204857600,
                         false,
                         nullptr /* yieldPolicy */,
                         kEmptyPlanNodeId);

    auto tracker = std::make_unique<TrialRunTracker>(boost::none, boost::none, boost::none);
    ON_BLOCK_EXIT([&]() { sortStage->detachFromTrialRunTracker(); });

    auto attachResult = sortStage->attachToTrialRunTracker(tracker.get());
    ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::TrackResults);
}

TEST_F(TrialRunTrackerTest, TrackerAttachesToBothBlockingAndStreamingStages) {
    auto collUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();
    auto scanStage = makeS<sbe::ScanStage>(collUuid,
                                           DatabaseName(),
                                           generateSlotId() /* recordSlot */,
                                           generateSlotId() /* recordIdSlot */,
                                           generateSlotId() /* snapshotIdSlot */,
                                           generateSlotId() /* indexIdSlot */,
                                           generateSlotId() /* indexKeySlot */,
                                           generateSlotId() /* indexKeyPatternSlot */,
                                           boost::none /* oplogTsSlot */,
                                           std::vector<std::string>{"field"} /* scanFieldNames */,
                                           makeSV(generateSlotId()) /* scanFieldSlots */,
                                           generateSlotId() /* seekRecordIdSlot */,
                                           generateSlotId() /* minRecordIdSlot */,
                                           generateSlotId() /* maxRecordIdSlot */,
                                           true /* forward */,
                                           nullptr /* yieldPolicy */,
                                           kEmptyPlanNodeId /* nodeId */,
                                           ScanCallbacks());

    auto rootSortStage = makeS<SortStage>(std::move(scanStage),
                                          makeSV(),
                                          std::vector<value::SortDirection>{},
                                          makeSV(),
                                          nullptr /*limit*/,
                                          204857600,
                                          false,
                                          nullptr /* yieldPolicy */,
                                          kEmptyPlanNodeId);

    auto tracker = std::make_unique<TrialRunTracker>(boost::none, boost::none, boost::none);
    ON_BLOCK_EXIT([&]() { rootSortStage->detachFromTrialRunTracker(); });

    auto attachResult = rootSortStage->attachToTrialRunTracker(tracker.get());
    ASSERT_EQ(attachResult,
              PlanStage::TrialRunTrackingType::TrackReads |
                  PlanStage::TrialRunTrackingType::TrackResults);
}

TEST_F(TrialRunTrackerTest, TrialEndsDuringOpenPhaseOfBlockingStage) {
    auto ctx = makeCompileCtx();

    // Build a mock scan that will provide 9 values to its parent SortStage. The test
    // TrialRunTracker will have a 'numResults' limit of 8, so it will reach its limit when the
    // SortStage it attaches to reaches the last value.
    const size_t numResultsLimit = 8;
    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 4 << 5 << 6 << 7 << 8 << 9));
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Build a HashAggStage, group by the scanSlot and compute a simple count.
    auto countsSlot = generateSlotId();
    auto hashAggStage = makeS<HashAggStage>(
        std::move(scanStage),
        makeSV(scanSlot),
        makeAggExprVector(
            countsSlot,
            nullptr,
            stage_builder::makeFunction(
                "sum",
                makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1)))),
        makeSV(), /* Seek slot */
        true,
        boost::none,
        false /* allowDiskUse */,
        makeSlotExprPairVec(), /* mergingExprs */
        nullptr /* yieldPolicy */,
        kEmptyPlanNodeId);

    auto tracker = std::make_unique<TrialRunTracker>(numResultsLimit, size_t{0}, size_t{0});
    auto attachResult = hashAggStage->attachToTrialRunTracker(tracker.get());

    // Note: A scan is a streaming stage, but the "virtual scan" used here does not attach to the
    // tracker.
    ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::TrackResults);

    // The 'prepareTree()' function opens the HashAggStage, causing it to read documents from its
    // child. Because the child provides more documents than the 'numResults' limit, we expect the
    // open operation to be interrupted by a 'QueryTrialRunCompleted' exception.
    ASSERT_THROWS_CODE(prepareTree(ctx.get(), hashAggStage.get(), countsSlot),
                       DBException,
                       ErrorCodes::QueryTrialRunCompleted);
}

TEST_F(TrialRunTrackerTest, OnlyDeepestNestedBlockingStageHasTrialRunTracker) {
    auto ctx = makeCompileCtx();

    // The contrived PlanStage tree constructed here allows us to observe what happens when a
    // HashAgg stage has a SortStage in its subtree. A UnionStage injects extra documents into the
    // HashAgg stage, allowing for a test scenario where the parent HashAgg stage sees many more
    // documents than the SortStage.

    // This "upperScan" gets unioned with the output of the SortStage and provides 10 values.
    auto [upperScanSlot, upperScanStage] = [this]() {
        auto [inputTag, inputVal] =
            stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 4 << 5 << 6 << 7 << 8 << 9 << 10));
        return generateVirtualScan(inputTag, inputVal);
    }();

    auto [sortSlot, sortStage] = [this]() {
        auto [inputTag, inputVal] =
            stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 4 << 5 << 6 << 7 << 8 << 9));
        auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

        auto sortStage =
            makeS<SortStage>(std::move(scanStage),
                             makeSV(scanSlot),
                             std::vector<value::SortDirection>{value::SortDirection::Ascending},
                             makeSV(),
                             nullptr /*limit*/,
                             204857600,
                             false,
                             nullptr /* yieldPolicy */,
                             kEmptyPlanNodeId);

        return std::make_pair(scanSlot, std::move(sortStage));
    }();

    auto unionSlot = generateSlotId();
    auto unionStage =
        makeS<UnionStage>(makeSs(std::move(upperScanStage), std::move(sortStage)),
                          std::vector<value::SlotVector>{makeSV(upperScanSlot), makeSV(sortSlot)},
                          makeSV(unionSlot),
                          kEmptyPlanNodeId);

    auto countsSlot = generateSlotId();
    auto hashAggStage = makeS<HashAggStage>(
        std::move(unionStage),
        makeSV(unionSlot),
        makeAggExprVector(
            countsSlot,
            nullptr,
            stage_builder::makeFunction(
                "sum",
                makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1)))),
        makeSV(), /* Seek slot */
        true,
        boost::none,
        false /* allowDiskUse */,
        makeSlotExprPairVec(), /* mergingExprs */
        nullptr /* yieldPolicy */,
        kEmptyPlanNodeId);

    hashAggStage->prepare(*ctx);
    hashAggStage->attachToOperationContext(operationContext());

    {
        // We expect the TrialRunTracker to attach to the SortStage but not the root HashAggStage.
        auto tracker = std::make_unique<TrialRunTracker>(size_t{9}, size_t{0}, size_t{0});
        ON_BLOCK_EXIT([&]() { hashAggStage->detachFromTrialRunTracker(); });
        auto attachResult = hashAggStage->attachToTrialRunTracker(tracker.get());

        // Note: A scan is a streaming stage, but the "virtual scan" used here does not attach to
        // the tracker.
        ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::TrackResults);

        // In this scenario, the HashAggStage will see 10+ documents (the 10 documents from the
        // "upperScan" plus the documents from the SortStage), which exceeds the 'numResults'
        // requirement of the TrialRunTracker. The 'open()' call will _succeed_, however, because
        // the TrialRunTracker is not attached to the HashAggStage.
        hashAggStage->open(false);

        hashAggStage->close();
    }

    {
        // We expect the TrialRunTracker to attach to the SortStage but not the root HashAggStage.
        auto tracker = std::make_unique<TrialRunTracker>(size_t{2}, size_t{0}, size_t{0});
        ON_BLOCK_EXIT([&]() { hashAggStage->detachFromTrialRunTracker(); });
        auto attachResult = hashAggStage->attachToTrialRunTracker(tracker.get());

        ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::TrackResults);

        // In this scenario, the SortStage will see more documents than the 2 permitted by the
        // 'numResults' requirement of the TrialRunTracker. The 'open()' call will _fail_, because
        // the TrialRunTracker is attached to the SortStage.
        ASSERT_THROWS_CODE(
            hashAggStage->open(false), DBException, ErrorCodes::QueryTrialRunCompleted);

        hashAggStage->close();
    }
}

TEST_F(TrialRunTrackerTest, SiblingBlockingStagesBothGetTrialRunTracker) {
    auto ctx = makeCompileCtx();

    // This PlanStage tree allows us to observe what happens when we attach a TrialRunTracker to two
    // sibling HashAgg stages.

    auto buildHashAgg = [&]() {
        auto [inputTag, inputVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 4 << 5));
        auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

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
            makeSV(), /* Seek slot */
            true,
            boost::none,
            false /* allowDiskUse */,
            makeSlotExprPairVec(), /* mergingExprs */
            nullptr /* yieldPolicy */,
            kEmptyPlanNodeId);

        return std::make_pair(countsSlot, std::move(hashAggStage));
    };

    auto [leftCountsSlot, leftHashAggStage] = buildHashAgg();
    auto [rightCountsSlot, rightHashAggStage] = buildHashAgg();

    // The UnionStage allows us to establish the sibling relationship.
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
                         nullptr /*limit*/,
                         204857600,
                         false,
                         nullptr /* yieldPolicy */,
                         kEmptyPlanNodeId);

    // We expect the TrialRunTracker to attach to _both_ HashAgg stages but not to the SortStage.
    auto tracker = std::make_unique<TrialRunTracker>(size_t{9}, size_t{0}, size_t{0});
    auto attachResult = sortStage->attachToTrialRunTracker(tracker.get());

    // Note: A scan is a streaming stage, but the "virtual scan" used here does not attach to the
    // tracker.
    ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::TrackResults);

    // The 'prepareTree()' function opens the SortStage, causing it to read documents from its
    // child. If only one of the HashAgg stages were attached to the TrialRunTracker, it would not
    // increment the 'numResults' metric enough to end the trial, but with both stages together,
    // `numResults` gets incremented to 10 (with a limit of 9), resulting in a
    // QueryTrialRunCompleted exception.
    ASSERT_THROWS_CODE(prepareTree(ctx.get(), sortStage.get(), resultSlot),
                       DBException,
                       ErrorCodes::QueryTrialRunCompleted);
}

TEST_F(TrialRunTrackerTest, TrialRunTrackingCanBeDisabled) {
    auto scanStage =
        makeS<sbe::ScanStage>(UUID::parse("00000000-0000-0000-0000-000000000000").getValue(),
                              DatabaseName(),
                              generateSlotId() /* recordSlot */,
                              generateSlotId() /* recordIdSlot */,
                              generateSlotId() /* snapshotIdSlot */,
                              generateSlotId() /* indexIdSlot */,
                              generateSlotId() /* indexKeySlot */,
                              generateSlotId() /* indexKeyPatternSlot */,
                              boost::none /* oplogTsSlot */,
                              std::vector<std::string>{"field"} /* scanFieldNames */,
                              makeSV(generateSlotId()) /* scanFieldSlots */,
                              generateSlotId() /* seekRecordIdSlot */,
                              generateSlotId() /* minRecordIdSlot */,
                              generateSlotId() /* maxRecordIdSlot */,
                              true /* forward */,
                              nullptr /* yieldPolicy */,
                              kEmptyPlanNodeId /* nodeId */,
                              ScanCallbacks());

    scanStage->disableTrialRunTracking();
    auto tracker = std::make_unique<TrialRunTracker>(boost::none, boost::none, boost::none);
    auto attachResult = scanStage->attachToTrialRunTracker(tracker.get());
    ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::NoTracking);
}

TEST_F(TrialRunTrackerTest, DisablingTrackingForChildDoesNotInhibitTrackingForParent) {
    auto scanStage =
        makeS<sbe::ScanStage>(UUID::parse("00000000-0000-0000-0000-000000000000").getValue(),
                              DatabaseName(),
                              generateSlotId() /* recordSlot */,
                              generateSlotId() /* recordIdSlot */,
                              generateSlotId() /* snapshotIdSlot */,
                              generateSlotId() /* indexIdSlot */,
                              generateSlotId() /* indexKeySlot */,
                              generateSlotId() /* indexKeyPatternSlot */,
                              boost::none /* oplogTsSlot */,
                              std::vector<std::string>{"field"} /* scanFieldNames */,
                              makeSV(generateSlotId()) /* scanFieldSlots */,
                              generateSlotId() /* seekRecordIdSlot */,
                              generateSlotId() /* minRecordIdSlot */,
                              generateSlotId() /* maxRecordIdSlot */,
                              true /* forward */,
                              nullptr /* yieldPolicy */,
                              kEmptyPlanNodeId /* nodeId */,
                              ScanCallbacks());

    // Disable tracking for 'scanStage'. We should still attach the tracker for 'rootSortStage'.
    scanStage->disableTrialRunTracking();

    auto rootSortStage = makeS<SortStage>(std::move(scanStage),
                                          makeSV(),
                                          std::vector<value::SortDirection>{},
                                          makeSV(),
                                          nullptr /*limit*/,
                                          204857600,
                                          false,
                                          nullptr /* yieldPolicy */,
                                          kEmptyPlanNodeId);


    auto tracker = std::make_unique<TrialRunTracker>(boost::none, boost::none, boost::none);
    ON_BLOCK_EXIT([&]() { rootSortStage->detachFromTrialRunTracker(); });

    auto attachResult = rootSortStage->attachToTrialRunTracker(tracker.get());
    ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::TrackResults);
}

TEST_F(TrialRunTrackerTest, DisablingTrackingForAChildStagePreventsEarlyExit) {
    auto ctx = makeCompileCtx();

    // This PlanStage tree allows us to observe what happens when we attach a TrialRunTracker to
    // one of two sibling HashAgg stages.
    auto buildHashAgg = [&]() {
        auto [inputTag, inputVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 4 << 5));
        auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

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
            makeSV(), /* Seek slot */
            true,
            boost::none,
            false /* allowDiskUse */,
            makeSlotExprPairVec(), /* mergingExprs */
            nullptr /* yieldPolicy */,
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
                         nullptr /*limit*/,
                         204857600,
                         false,
                         nullptr /* yieldPolicy */,
                         kEmptyPlanNodeId);

    // We expect the TrialRunTracker to attach to _only_ the left child.
    auto tracker = std::make_unique<TrialRunTracker>(size_t{9}, boost::none, boost::none);
    auto attachResult = sortStage->attachToTrialRunTracker(tracker.get());

    // Note: A scan is a streaming stage, but the "virtual scan" used here does not attach to the
    // tracker.
    ASSERT_EQ(attachResult, PlanStage::TrialRunTrackingType::TrackResults);

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
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal, PlanNodeId{1});
    auto unwindSlot = generateSlotId();
    auto unwindStage = makeS<UnwindStage>(
        std::move(scanStage), scanSlot, generateSlotId(), unwindSlot, true, PlanNodeId{2});
    auto sortStage =
        makeS<SortStage>(std::move(unwindStage),
                         makeSV(unwindSlot),
                         std::vector<value::SortDirection>{value::SortDirection::Ascending},
                         makeSV(),
                         nullptr /*limit*/,
                         1024 * 1024,
                         false,
                         nullptr /* yieldPolicy */,
                         PlanNodeId{3});

    auto tracker = std::make_unique<TrialRunTracker>(size_t{6}, boost::none, size_t{3});
    auto attachResult = sortStage->attachToTrialRunTracker(tracker.get(), PlanNodeId{1});
    ASSERT_EQ(attachResult,
              PlanStage::TrialRunTrackingType::TrackPlanningResults |
                  PlanStage::TrialRunTrackingType::TrackResults);

    // The 'prepareTree()' function opens the SortStage, causing it to read documents from its
    // child. Only 3 documents should be returned by scan stage, which is marked as planning root,
    // but sort should have 6 results as input because of unwind.
    prepareTree(ctx.get(), sortStage.get(), unwindSlot);
    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumResults>(), 6);
    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumPlanningResults>(), 3);
}

TEST_F(TrialRunTrackerTest, TrackerCanTrackMetricWithMaxMetricSetToZero) {
    auto ctx = makeCompileCtx();

    auto [inputTag, inputVal] = stage_builder::makeValue(
        BSON_ARRAY(BSON("a" << 1) << BSON("a" << 2) << BSON("a" << 3) << BSON("a" << 4)));
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal, PlanNodeId{1});
    auto sortStage =
        makeS<SortStage>(std::move(scanStage),
                         makeSV(scanSlot),
                         std::vector<value::SortDirection>{value::SortDirection::Ascending},
                         makeSV(),
                         nullptr /*limit*/,
                         1024 * 1024,
                         false,
                         nullptr /* yieldPolicy */,
                         PlanNodeId{3});

    auto onMetricReached = [&](TrialRunTracker::TrialRunMetric metric) {
        // This callback should get called for the kNumResults metric.
        ASSERT_EQ(metric, TrialRunTracker::kNumResults);
        // Detach the TrialRunTracker and allow the plan to continue running.
        sortStage->detachFromTrialRunTracker();
        return false;
    };

    auto tracker = std::make_unique<TrialRunTracker>(
        std::move(onMetricReached), size_t{0}, boost::none, boost::none);
    auto attachResult = sortStage->attachToTrialRunTracker(tracker.get(), PlanNodeId{1});
    ASSERT_EQ(attachResult,
              PlanStage::TrialRunTrackingType::TrackPlanningResults |
                  PlanStage::TrialRunTrackingType::TrackResults);

    // The 'prepareTree()' function opens the SortStage, causing it to read documents from its
    // child. The scan stage (which is marked as the planning root) should return 4 documents.
    // The tracker should only observe the first document, because the callback we supplied should
    // detach the tracker after the first document is observed (because that is precisely when
    // 'tracker->getMetric<TrialRunTracker::kNumResults>()' becomes greater than the maximum of 0.)
    prepareTree(ctx.get(), sortStage.get(), scanSlot);
    ASSERT_EQ(tracker->getMetric<TrialRunTracker::kNumResults>(), 1);
}
}  // namespace mongo::sbe
