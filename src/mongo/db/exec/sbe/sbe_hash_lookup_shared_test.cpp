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

namespace mongo::sbe {
void HashLookupSharedTest::prepareAndEvalStageWithReopen(
    CompileCtx* ctx,
    std::ostream& stream,
    const StageResultsPrinters::SlotNames& slotNames,
    PlanStage* stage) {
    prepareTree(ctx, stage);

    // Execute the stage normally.
    std::stringstream firstStream;
    StageResultsPrinters::make(firstStream, printOptions).printStageResults(ctx, slotNames, stage);
    stream << "--- First Stats\n";
    printHashLookupStats(stream, stage);

    // Execute the stage after reopen and verify that output is the same.
    stage->open(true);
    std::stringstream secondStream;
    StageResultsPrinters::make(secondStream, printOptions).printStageResults(ctx, slotNames, stage);
    ASSERT_EQ(firstStream.view(), secondStream.view());
    stream << "--- Second Stats\n";
    printHashLookupStats(stream, stage);

    // Execute the stage after close and open and verify that output is the same.
    stage->close();
    stage->open(false);
    std::stringstream thirdStream;
    StageResultsPrinters::make(thirdStream, printOptions).printStageResults(ctx, slotNames, stage);
    ASSERT_EQ(firstStream.view(), thirdStream.view());
    stream << "--- Third Stats\n";
    printHashLookupStats(stream, stage);

    stage->close();

    // Execute the stage with spilling to disk.
    auto defaultInternalQuerySBELookupApproxMemoryUseInBytesBeforeSpill =
        internalQuerySBELookupApproxMemoryUseInBytesBeforeSpill.load();
    internalQuerySBELookupApproxMemoryUseInBytesBeforeSpill.store(10);
    ON_BLOCK_EXIT([&] {
        internalQuerySBELookupApproxMemoryUseInBytesBeforeSpill.store(
            defaultInternalQuerySBELookupApproxMemoryUseInBytesBeforeSpill);
    });

    // Run the stage after the knob is set and spill to disk. We need to hold a global IS lock
    // to read from WT.
    Lock::GlobalLock lk(operationContext(), MODE_IS);
    stage->open(true);
    std::stringstream fourthStream;
    StageResultsPrinters::make(fourthStream, printOptions).printStageResults(ctx, slotNames, stage);
    ASSERT_EQ(firstStream.view(), fourthStream.view());
    stream << "--- Fourth Stats\n";
    printHashLookupStats(stream, stage);
    stream << '\n';

    // Execute the stage after close and open and verify that output is the same.
    stage->close();
    stage->open(false);
    std::stringstream fifthStream;
    StageResultsPrinters::make(fifthStream, printOptions).printStageResults(ctx, slotNames, stage);
    ASSERT_EQ(firstStream.view(), fifthStream.view());
    stream << "--- Fifth Stats\n";
    printHashLookupStats(stream, stage);
    stream << '\n';

    // Execute the stage after reopen and we have spilled to disk and verify that output is the
    // same.
    stage->open(true);
    std::stringstream sixthStream;
    StageResultsPrinters::make(sixthStream, printOptions).printStageResults(ctx, slotNames, stage);
    ASSERT_EQ(firstStream.view(), sixthStream.view());
    stream << "--- Sixth Stats\n";
    printHashLookupStats(stream, stage);
    stream << '\n';

    stage->close();

    stream << "-- OUTPUT " << firstStream.view();
}  // prepareAndEvalStageWithReopen
}  // namespace mongo::sbe
