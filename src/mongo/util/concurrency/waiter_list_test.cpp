// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/concurrency/waiter_list.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/executor_test_util.h"

namespace mongo {

TEST(WaiterList, BasicWaiterWorks) {
    auto waiters = WaiterList<int>();
    auto fut = waiters.waitFor(1);
    ASSERT_FALSE(fut.isReady());
    waiters.notifyWaiters(1);
    ASSERT_TRUE(fut.isReady());
    fut.get();
}

TEST(WaiterList, MultipleWaitersWorks) {
    auto waiters = WaiterList<int>();

    auto fut1 = waiters.waitFor(1);
    auto fut2 = waiters.waitFor(1);
    auto fut3 = waiters.waitFor(1);
    ASSERT_FALSE(fut1.isReady());
    ASSERT_FALSE(fut2.isReady());
    ASSERT_FALSE(fut3.isReady());

    waiters.notifyWaiters(1);

    ASSERT_TRUE(fut1.isReady());
    ASSERT_TRUE(fut2.isReady());
    ASSERT_TRUE(fut3.isReady());
    fut1.get();
    fut2.get();
    fut3.get();
}

TEST(WaiterList, MultipleWaitersWithDifferentContinuations) {
    auto waiters = WaiterList<int>();

    auto fut1 = waiters.waitFor(1);
    auto fut2 = waiters.waitFor(1);
    auto fut3 = waiters.waitFor(1);
    ASSERT_FALSE(fut1.isReady());
    ASSERT_FALSE(fut2.isReady());
    ASSERT_FALSE(fut3.isReady());

    auto executor = InlineQueuedCountingExecutor::make();

    Atomic<int> i = 0;

    // We now attach different continuation chains to the returned futures. The expectation is that
    // each chain will be executed separately without overwriting each other on the returned future
    // since futures only have a single callback attached.
    auto finalFut1 = fut1.thenRunOn(executor).then([&]() { return i.addAndFetch(1); });
    auto finalFut2 = fut2.thenRunOn(executor).then([&]() { return i.addAndFetch(1); });
    auto finalFut3 = fut3.thenRunOn(executor).then([&]() { return i.addAndFetch(1); });

    waiters.notifyWaiters(1);

    ASSERT_TRUE(fut1.isReady());
    ASSERT_TRUE(fut2.isReady());
    ASSERT_TRUE(fut3.isReady());

    finalFut1.get();
    finalFut2.get();
    finalFut3.get();

    ASSERT_EQ(i.load(), 3);
}

TEST(WaiterList, WaitersAreSelectivelyWoken) {
    auto waiters = WaiterList<int>();

    auto fut1 = waiters.waitFor(1);
    auto fut2 = waiters.waitFor(2);
    ASSERT_FALSE(fut1.isReady());
    ASSERT_FALSE(fut2.isReady());

    waiters.notifyWaiters(1);

    ASSERT_TRUE(fut1.isReady());
    fut1.get();
    ASSERT_FALSE(fut2.isReady());
}

TEST(WaiterList, WaitersAreWokenBasedOnAPredicate) {
    auto waiters = WaiterList<int>();

    auto fut1 = waiters.waitFor(1);
    auto fut2 = waiters.waitFor(2);
    auto fut3 = waiters.waitFor(3);
    auto fut4 = waiters.waitFor(4);
    ASSERT_FALSE(fut1.isReady());
    ASSERT_FALSE(fut2.isReady());
    ASSERT_FALSE(fut3.isReady());
    ASSERT_FALSE(fut4.isReady());

    waiters.notifyWaitersBasedOnPredicate([](const auto& key) { return key % 2 == 0; });

    ASSERT_FALSE(fut1.isReady());
    ASSERT_TRUE(fut2.isReady());
    fut2.get();
    ASSERT_FALSE(fut3.isReady());
    ASSERT_TRUE(fut4.isReady());
    fut4.get();
}

}  // namespace mongo
