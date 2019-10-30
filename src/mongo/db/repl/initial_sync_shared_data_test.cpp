/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/initial_sync_shared_data.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace repl {

TEST(InitialSyncSharedDataTest, SingleFailedOperation) {
    InitialSyncSharedData data(
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44, 1 /* rollBackId */);
    ClockSourceMock clock;

    stdx::lock_guard<InitialSyncSharedData> lk(data);
    // No current outage.
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds::zero(), data.getTotalTimeUnreachable(lk, &clock));

    // Outage just started.  Still no outage time recorded, but current outage is zero rather than
    // min().
    data.incrementRetryingOperations(lk, &clock);
    ASSERT_EQ(Milliseconds::zero(), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds::zero(), data.getTotalTimeUnreachable(lk, &clock));

    // Our outage goes on for a second.
    clock.advance(Seconds(1));
    ASSERT_EQ(Milliseconds(1000), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk, &clock));

    // Now it's over.
    data.decrementRetryingOperations(lk, &clock);
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk, &clock));

    // Nothing changes when there's no outage.
    clock.advance(Seconds(1));
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk, &clock));
}

TEST(InitialSyncSharedDataTest, SequentialFailedOperations) {
    InitialSyncSharedData data(
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44, 1 /* rollBackId */);
    ClockSourceMock clock;

    stdx::lock_guard<InitialSyncSharedData> lk(data);
    // No current outage.
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds::zero(), data.getTotalTimeUnreachable(lk, &clock));

    // Outage just started.  Still no outage time recorded, but current outage is zero rather than
    // min().
    data.incrementRetryingOperations(lk, &clock);
    ASSERT_EQ(Milliseconds::zero(), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds::zero(), data.getTotalTimeUnreachable(lk, &clock));

    // Our outage goes on for a second.
    clock.advance(Seconds(1));
    ASSERT_EQ(Milliseconds(1000), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk, &clock));

    // Now it's over.
    data.decrementRetryingOperations(lk, &clock);
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk, &clock));

    // Nothing changes when there's no outage.
    clock.advance(Seconds(1));
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk, &clock));

    // There's another outage.
    data.incrementRetryingOperations(lk, &clock);
    ASSERT_EQ(Milliseconds::zero(), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk, &clock));

    // It's on for three seconds.
    clock.advance(Seconds(3));
    ASSERT_EQ(Milliseconds(3000), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(4000), data.getTotalTimeUnreachable(lk, &clock));

    // It's on for another five seconds.
    clock.advance(Seconds(5));
    ASSERT_EQ(Milliseconds(8000), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(9000), data.getTotalTimeUnreachable(lk, &clock));

    // Now it's over.
    data.decrementRetryingOperations(lk, &clock);
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(9000), data.getTotalTimeUnreachable(lk, &clock));

    clock.advance(Seconds(1));
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(9000), data.getTotalTimeUnreachable(lk, &clock));
}

TEST(InitialSyncSharedDataTest, OverlappingFailedOperations) {
    InitialSyncSharedData data(
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44, 1 /* rollBackId */);
    ClockSourceMock clock;

    stdx::lock_guard<InitialSyncSharedData> lk(data);
    // No current outage.
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds::zero(), data.getTotalTimeUnreachable(lk, &clock));

    // Outage just started.  Still no outage time recorded, but current outage is zero rather than
    // min().
    data.incrementRetryingOperations(lk, &clock);
    ASSERT_EQ(Milliseconds::zero(), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds::zero(), data.getTotalTimeUnreachable(lk, &clock));

    // Our outage goes on for a second.
    clock.advance(Seconds(1));
    ASSERT_EQ(Milliseconds(1000), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk, &clock));

    // Now another operation fails.
    data.incrementRetryingOperations(lk, &clock);
    ASSERT_EQ(Milliseconds(1000), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk, &clock));

    // Both stay failed for 2 seconds.
    clock.advance(Seconds(2));
    ASSERT_EQ(Milliseconds(3000), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(3000), data.getTotalTimeUnreachable(lk, &clock));

    // Now an operation succeeds.
    data.decrementRetryingOperations(lk, &clock);
    ASSERT_EQ(Milliseconds(3000), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(3000), data.getTotalTimeUnreachable(lk, &clock));

    // The next one doesn't work for another three seconds.
    clock.advance(Seconds(3));
    ASSERT_EQ(Milliseconds(6000), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(6000), data.getTotalTimeUnreachable(lk, &clock));

    // Now the other operation succeeds.
    data.decrementRetryingOperations(lk, &clock);
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(6000), data.getTotalTimeUnreachable(lk, &clock));

    // Nothing changes when there's no outage.
    clock.advance(Seconds(1));
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk, &clock));
    ASSERT_EQ(Milliseconds(6000), data.getTotalTimeUnreachable(lk, &clock));
}

}  // namespace repl
}  // namespace mongo
