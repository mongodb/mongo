/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
 * This file contains shared code for child classes HashLookupStageTest, HashLookupUnwindStageTest.
 */

#include "mongo/db/exec/sbe/sbe_hash_lookup_shared_test.h"

#include "mongo/idl/server_parameter_test_controller.h"

namespace mongo::sbe {
void HashLookupSharedTest::prepareAndEvalStageWithReopen(
    CompileCtx* ctx,
    std::ostream& stream,
    const StageResultsPrinters::SlotNames& slotNames,
    PlanStage* stage,
    bool expectMemUse) {
    prepareTree(ctx, stage);

    // Execute the stage normally.
    std::stringstream firstStream;
    StageResultsPrinters::make(firstStream, printOptions).printStageResults(ctx, slotNames, stage);
    std::string firstStr = firstStream.str();
    stream << "--- First Stats" << std::endl;
    printHashLookupStats(stream, stage);
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
    std::string secondStr = secondStream.str();
    ASSERT_EQ(firstStr, secondStr);
    stream << "--- Second Stats" << std::endl;
    printHashLookupStats(stream, stage);
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
    std::string thirdStr = thirdStream.str();
    ASSERT_EQ(firstStr, thirdStr);
    stream << "--- Third Stats" << std::endl;
    printHashLookupStats(stream, stage);
    stats = static_cast<const HashLookupStats*>(stage->getSpecificStats());
    auto thirdPeakTrackedMemBytes = stats->peakTrackedMemBytes;
    ASSERT_EQ(firstPeakTrackedMemBytes, thirdPeakTrackedMemBytes);
    if (expectMemUse) {
        ASSERT_GT(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
    } else {
        ASSERT_EQ(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
    }
    stage->close();

    // Execute the stage with spilling to disk.
    RAIIServerParameterControllerForTest maxMemoryLimit(
        "internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill", 10);

    // Run the stage after the knob is set and spill to disk. We need to hold a global IS lock
    // to read from WT.
    Lock::GlobalLock lk(operationContext(), MODE_IS);
    stage->open(true);
    std::stringstream fourthStream;
    StageResultsPrinters::make(fourthStream, printOptions).printStageResults(ctx, slotNames, stage);
    std::string fourthStr = fourthStream.str();
    ASSERT_EQ(firstStr, fourthStr);
    stream << "--- Fourth Stats" << std::endl;
    printHashLookupStats(stream, stage);
    stream << std::endl;
    stats = static_cast<const HashLookupStats*>(stage->getSpecificStats());
    auto fourthPeakTrackedMemBytes = stats->peakTrackedMemBytes;
    ASSERT_EQ(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(firstPeakTrackedMemBytes, fourthPeakTrackedMemBytes);

    // Execute the stage after close and open and verify that output is the same.
    stage->close();
    stage->open(false);
    std::stringstream fifthStream;
    StageResultsPrinters::make(fifthStream, printOptions).printStageResults(ctx, slotNames, stage);
    std::string fifthStr = fifthStream.str();
    ASSERT_EQ(firstStr, fifthStr);
    stream << "--- Fifth Stats" << std::endl;
    printHashLookupStats(stream, stage);
    stream << std::endl;
    stats = static_cast<const HashLookupStats*>(stage->getSpecificStats());
    auto fifthPeakTrackedMemBytes = stats->peakTrackedMemBytes;
    ASSERT_EQ(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(firstPeakTrackedMemBytes, fifthPeakTrackedMemBytes);

    // Execute the stage after reopen and we have spilled to disk and verify that output is the
    // same.
    stage->open(true);
    std::stringstream sixthStream;
    StageResultsPrinters::make(sixthStream, printOptions).printStageResults(ctx, slotNames, stage);
    std::string sixthStr = sixthStream.str();
    ASSERT_EQ(firstStr, sixthStr);
    stream << "--- Sixth Stats" << std::endl;
    printHashLookupStats(stream, stage);
    stream << std::endl;
    stats = static_cast<const HashLookupStats*>(stage->getSpecificStats());
    auto sixthPeakTrackedMemBytes = stats->peakTrackedMemBytes;
    ASSERT_EQ(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(firstPeakTrackedMemBytes, sixthPeakTrackedMemBytes);

    stage->close();

    stream << "-- OUTPUT ";
    stream << firstStr;
}  // prepareAndEvalStageWithReopen
}  // namespace mongo::sbe
