// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/initial_sync/initial_sync_shared_data.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

#include <memory>

#include <boost/none.hpp>

namespace mongo {
namespace repl {

TEST(InitialSyncSharedDataTest, SingleFailedOperation) {
    Days timeout(1);
    ClockSourceMock clock;
    InitialSyncSharedData data(1 /* rollBackId */, timeout, &clock);

    std::unique_lock<InitialSyncSharedData> lk(data);
    // No current outage.
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds::zero(), data.getTotalTimeUnreachable(lk));

    // Outage just started.  Still no outage time recorded, but current outage is zero rather than
    // min().
    InitialSyncSharedData::RetryableOperation op1;
    ASSERT_TRUE(data.shouldRetryOperation(lk, &op1));
    ASSERT_EQ(Milliseconds::zero(), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds::zero(), data.getTotalTimeUnreachable(lk));

    // Our outage goes on for a second.
    clock.advance(Seconds(1));
    ASSERT_EQ(Milliseconds(1000), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk));

    // Now it's over.
    lk.unlock();
    op1 = boost::none;
    lk.lock();
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk));

    // Nothing changes when there's no outage.
    clock.advance(Seconds(1));
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk));
}

TEST(InitialSyncSharedDataTest, SequentialFailedOperations) {
    Days timeout(1);
    ClockSourceMock clock;
    InitialSyncSharedData data(1 /* rollBackId */, timeout, &clock);

    std::unique_lock<InitialSyncSharedData> lk(data);
    // No current outage.
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds::zero(), data.getTotalTimeUnreachable(lk));

    // Outage just started.  Still no outage time recorded, but current outage is zero rather than
    // min().
    InitialSyncSharedData::RetryableOperation op1;
    ASSERT_TRUE(data.shouldRetryOperation(lk, &op1));
    ASSERT_EQ(Milliseconds::zero(), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds::zero(), data.getTotalTimeUnreachable(lk));

    // Our outage goes on for a second.
    clock.advance(Seconds(1));
    ASSERT_EQ(Milliseconds(1000), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk));

    // Now it's over.
    lk.unlock();
    op1 = boost::none;
    lk.lock();
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk));

    // Nothing changes when there's no outage.
    clock.advance(Seconds(1));
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk));

    // There's another outage.
    InitialSyncSharedData::RetryableOperation op2;
    ASSERT_TRUE(data.shouldRetryOperation(lk, &op2));
    ASSERT_EQ(Milliseconds::zero(), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk));

    // It's on for three seconds.
    clock.advance(Seconds(3));
    ASSERT_EQ(Milliseconds(3000), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(4000), data.getTotalTimeUnreachable(lk));

    // It's on for another five seconds.
    clock.advance(Seconds(5));
    ASSERT_EQ(Milliseconds(8000), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(9000), data.getTotalTimeUnreachable(lk));

    // Now it's over.
    lk.unlock();
    op2 = boost::none;
    lk.lock();
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(9000), data.getTotalTimeUnreachable(lk));

    clock.advance(Seconds(1));
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(9000), data.getTotalTimeUnreachable(lk));
}

TEST(InitialSyncSharedDataTest, OverlappingFailedOperations) {
    Days timeout(1);
    ClockSourceMock clock;
    InitialSyncSharedData data(1 /* rollBackId */, timeout, &clock);

    std::unique_lock<InitialSyncSharedData> lk(data);
    // No current outage.
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds::zero(), data.getTotalTimeUnreachable(lk));

    // Outage just started.  Still no outage time recorded, but current outage is zero rather than
    // min().
    InitialSyncSharedData::RetryableOperation op1;
    ASSERT_TRUE(data.shouldRetryOperation(lk, &op1));
    ASSERT_EQ(Milliseconds::zero(), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds::zero(), data.getTotalTimeUnreachable(lk));

    // Our outage goes on for a second.
    clock.advance(Seconds(1));
    ASSERT_EQ(Milliseconds(1000), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk));

    // Now another operation fails.
    InitialSyncSharedData::RetryableOperation op2;
    ASSERT_TRUE(data.shouldRetryOperation(lk, &op2));
    ASSERT_EQ(Milliseconds(1000), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(1000), data.getTotalTimeUnreachable(lk));

    // Both stay failed for 2 seconds.
    clock.advance(Seconds(2));
    ASSERT_EQ(Milliseconds(3000), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(3000), data.getTotalTimeUnreachable(lk));

    // Now an operation succeeds.
    lk.unlock();
    op1 = boost::none;
    lk.lock();
    ASSERT_EQ(Milliseconds(3000), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(3000), data.getTotalTimeUnreachable(lk));

    // The next one doesn't work for another three seconds.
    clock.advance(Seconds(3));
    ASSERT_EQ(Milliseconds(6000), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(6000), data.getTotalTimeUnreachable(lk));

    // Now the other operation succeeds.
    lk.unlock();
    op2 = boost::none;
    lk.lock();
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(6000), data.getTotalTimeUnreachable(lk));

    // Nothing changes when there's no outage.
    clock.advance(Seconds(1));
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(Milliseconds(6000), data.getTotalTimeUnreachable(lk));
}

TEST(InitialSyncSharedDataTest, OperationTimesOut) {
    Seconds timeout(5);
    ClockSourceMock clock;
    InitialSyncSharedData data(1 /* rollBackId */, timeout, &clock);

    InitialSyncSharedData::RetryableOperation op1;
    InitialSyncSharedData::RetryableOperation op2;
    std::unique_lock<InitialSyncSharedData> lk(data);

    // First operation fails
    ASSERT_TRUE(data.shouldRetryOperation(lk, &op1));
    ASSERT_EQ(Milliseconds::zero(), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(1, data.getRetryingOperationsCount(lk));

    clock.advance(Seconds(1));

    // Make another operation fail.
    ASSERT_TRUE(data.shouldRetryOperation(lk, &op2));
    ASSERT_EQ(Seconds(1), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(2, data.getRetryingOperationsCount(lk));

    clock.advance(Seconds(5));

    // op1 now times out, and should be cleared.
    ASSERT_FALSE(data.shouldRetryOperation(lk, &op1));
    ASSERT_FALSE(op1);

    // op2 is still runnning.
    ASSERT_EQ(Seconds(6), data.getCurrentOutageDuration(lk));
    ASSERT_EQ(1, data.getRetryingOperationsCount(lk));

    // op2 now times out.
    ASSERT_FALSE(data.shouldRetryOperation(lk, &op2));
    ASSERT_FALSE(op2);

    // No outage in progress (because all operations have failed).
    ASSERT_EQ(0, data.getRetryingOperationsCount(lk));
    ASSERT_EQ(Milliseconds::min(), data.getCurrentOutageDuration(lk));
}

}  // namespace repl
}  // namespace mongo
