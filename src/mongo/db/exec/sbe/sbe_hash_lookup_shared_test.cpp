// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file contains shared code for child classes HashLookupStageTest, HashLookupUnwindStageTest.
 */

#include "mongo/db/exec/sbe/sbe_hash_lookup_shared_test.h"

#include "mongo/unittest/server_parameter_guard.h"

namespace mongo::sbe {
void HashLookupSharedTest::prepareAndEvalStageWithReopen(
    CompileCtx* ctx,
    std::ostream& stream,
    const StageResultsPrinters::SlotNames& slotNames,
    PlanStage* stage,
    bool expectMemUse,
    bool getStats,
    bool executeWithSpill) {
    prepareTree(ctx, stage);

    // Execute the stage normally.
    std::stringstream firstStream;
    StageResultsPrinters::make(firstStream, printOptions).printStageResults(ctx, slotNames, stage);
    if (getStats) {
        stream << "--- First Stats\n";
        printHashLookupStats(stream, stage);
    }
    auto* stats = static_cast<const HashLookupStats*>(stage->getSpecificStats());
    auto firstPeakTrackedMemBytes = stats->peakTrackedMemBytes;
    ASSERT(stage->getMemoryTracker());

    if (expectMemUse) {
        ASSERT_GT(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
        ASSERT_GT(firstPeakTrackedMemBytes, 0);
    } else {
        ASSERT_EQ(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
        ASSERT_EQ(firstPeakTrackedMemBytes, 0);
    }

    // Execute the stage after reopen and verify that output is the same.
    stage->open(true);
    std::stringstream secondStream;
    StageResultsPrinters::make(secondStream, printOptions).printStageResults(ctx, slotNames, stage);
    ASSERT_EQ(firstStream.view(), secondStream.view());
    if (getStats) {
        stream << "--- Second Stats\n";
        printHashLookupStats(stream, stage);
    }
    stats = static_cast<const HashLookupStats*>(stage->getSpecificStats());
    auto secondPeakTrackedMemBytes = stats->peakTrackedMemBytes;
    ASSERT_EQ(firstPeakTrackedMemBytes, secondPeakTrackedMemBytes);
    if (expectMemUse) {
        ASSERT_GT(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
    } else {
        ASSERT_EQ(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
    }

    // Execute the stage after close and open and verify that output is the same.
    stage->close();
    stage->open(false);
    std::stringstream thirdStream;
    StageResultsPrinters::make(thirdStream, printOptions).printStageResults(ctx, slotNames, stage);
    ASSERT_EQ(firstStream.view(), thirdStream.view());
    if (getStats) {
        stream << "--- Third Stats\n";
        printHashLookupStats(stream, stage);
    }
    stats = static_cast<const HashLookupStats*>(stage->getSpecificStats());
    auto thirdPeakTrackedMemBytes = stats->peakTrackedMemBytes;
    ASSERT_EQ(firstPeakTrackedMemBytes, thirdPeakTrackedMemBytes);
    if (expectMemUse) {
        ASSERT_GT(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
    } else {
        ASSERT_EQ(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
    }
    stage->close();

    if (!executeWithSpill) {
        stream << "-- OUTPUT " << firstStream.view();
        return;
    }

    // Execute the stage with spilling to disk.
    unittest::ServerParameterGuard maxMemoryLimit(
        "internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill", 10);

    // Run the stage after the knob is set and spill to disk. We need to hold a global IS lock
    // to read from WT.
    Lock::GlobalLock lk(operationContext(), MODE_IS);
    stage->open(true);
    std::stringstream fourthStream;
    StageResultsPrinters::make(fourthStream, printOptions).printStageResults(ctx, slotNames, stage);
    ASSERT_EQ(firstStream.view(), fourthStream.view());
    if (getStats) {
        stream << "--- Fourth Stats\n";
        printHashLookupStats(stream, stage);
        stream << '\n';
    }
    stats = static_cast<const HashLookupStats*>(stage->getSpecificStats());
    auto fourthPeakTrackedMemBytes = stats->peakTrackedMemBytes;
    ASSERT_EQ(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(firstPeakTrackedMemBytes, fourthPeakTrackedMemBytes);

    // Execute the stage after close and open and verify that output is the same.
    stage->close();
    stage->open(false);
    std::stringstream fifthStream;
    StageResultsPrinters::make(fifthStream, printOptions).printStageResults(ctx, slotNames, stage);
    ASSERT_EQ(firstStream.view(), fifthStream.view());
    if (getStats) {
        stream << "--- Fifth Stats\n";
        printHashLookupStats(stream, stage);
        stream << '\n';
    }
    stats = static_cast<const HashLookupStats*>(stage->getSpecificStats());
    auto fifthPeakTrackedMemBytes = stats->peakTrackedMemBytes;
    ASSERT_EQ(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(firstPeakTrackedMemBytes, fifthPeakTrackedMemBytes);

    // Execute the stage after reopen and we have spilled to disk and verify that output is the
    // same.
    stage->open(true);
    std::stringstream sixthStream;
    StageResultsPrinters::make(sixthStream, printOptions).printStageResults(ctx, slotNames, stage);
    ASSERT_EQ(firstStream.view(), sixthStream.view());
    if (getStats) {
        stream << "--- Sixth Stats\n";
        printHashLookupStats(stream, stage);
        stream << '\n';
    }
    stats = static_cast<const HashLookupStats*>(stage->getSpecificStats());
    auto sixthPeakTrackedMemBytes = stats->peakTrackedMemBytes;
    ASSERT_EQ(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(firstPeakTrackedMemBytes, sixthPeakTrackedMemBytes);

    stage->close();

    stream << "-- OUTPUT " << firstStream.view();
}  // prepareAndEvalStageWithReopen
}  // namespace mongo::sbe
