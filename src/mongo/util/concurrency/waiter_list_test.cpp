/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

    AtomicWord<int> i = 0;

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
