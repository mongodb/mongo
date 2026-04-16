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

#include "mongo/db/repl/optime_observer_dispatcher.h"

#include "mongo/bson/timestamp.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"

#include <memory>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace repl {
namespace {

/**
 * Thread-safe observer that records every timestamp it receives and allows tests to block
 * until a given number of notifications have arrived.
 */
class MockObserver : public OpTimeObserver {
public:
    void onOpTime(const Timestamp& ts) override {
        stdx::lock_guard lk(_mutex);
        _received.push_back(ts);
        _cv.notify_all();
    }

    /**
     * Blocks until at least `count` notifications have been received or `timeout` elapses.
     * Returns true if the count was reached within the timeout.
     */
    bool waitForCount(size_t count, Milliseconds timeout = Milliseconds(5000)) {
        stdx::unique_lock lk(_mutex);
        return _cv.wait_for(
            lk, timeout.toSystemDuration(), [&] { return _received.size() >= count; });
    }

    std::vector<Timestamp> timestamps() {
        stdx::lock_guard lk(_mutex);
        return _received;
    }

    size_t count() {
        stdx::lock_guard lk(_mutex);
        return _received.size();
    }

private:
    stdx::mutex _mutex;
    stdx::condition_variable _cv;
    std::vector<Timestamp> _received;
};


class DispatcherTest : public unittest::Test {
protected:
    OpTimeObserverDispatcher dispatcher;
};

// ---------------------------------------------------------------------------
// 1. No thread is started and shutdown is a no-op when no observers are added.
// ---------------------------------------------------------------------------
TEST_F(DispatcherTest, NoObserversShutdownIsNoop) {
    // Destructor calls shutdown(); this test just verifies no crash or hang.
    dispatcher.shutdown();
}

// ---------------------------------------------------------------------------
// 2. A single observer receives the exact timestamp passed to notify().
// ---------------------------------------------------------------------------
TEST_F(DispatcherTest, SingleObserverReceivesNotify) {
    auto obs = std::make_unique<MockObserver>();
    auto* raw = obs.get();
    dispatcher.addObserver(std::move(obs));

    dispatcher.notify(WithLock::withoutLock(), Timestamp(5, 1));

    ASSERT_TRUE(raw->waitForCount(1));
    ASSERT_EQ(Timestamp(5, 1), raw->timestamps()[0]);
}

// ---------------------------------------------------------------------------
// 3. An observer added after the thread has started receives subsequent notifies.
//    Also verifies that the dispatcher snapshots the observer list under lock,
//    so a newly added observer is included in the next dispatch cycle.
// ---------------------------------------------------------------------------
TEST_F(DispatcherTest, ObserverAddedAfterThreadStartReceivesSubsequentNotify) {
    auto obs1 = std::make_unique<MockObserver>();
    auto* raw1 = obs1.get();
    dispatcher.addObserver(std::move(obs1));  // starts thread

    dispatcher.notify(WithLock::withoutLock(), Timestamp(1, 1));
    ASSERT_TRUE(raw1->waitForCount(1));

    // Add obs2 after the first dispatch cycle completes.
    auto obs2 = std::make_unique<MockObserver>();
    auto* raw2 = obs2.get();
    dispatcher.addObserver(std::move(obs2));

    dispatcher.notify(WithLock::withoutLock(), Timestamp(2, 1));

    ASSERT_TRUE(raw1->waitForCount(2));
    ASSERT_TRUE(raw2->waitForCount(1));

    ASSERT_EQ(2U, raw1->count());
    ASSERT_EQ(1U, raw2->count());

    ASSERT_EQ(Timestamp(1, 1), raw1->timestamps()[0]);
    ASSERT_EQ(Timestamp(2, 1), raw1->timestamps()[1]);
    ASSERT_EQ(Timestamp(2, 1), raw2->timestamps()[0]);
}

// ---------------------------------------------------------------------------
// 4. All observers registered before the first notify receive that notification.
// ---------------------------------------------------------------------------
TEST_F(DispatcherTest, MultipleObserversAllReceiveNotify) {
    auto obs1 = std::make_unique<MockObserver>();
    auto obs2 = std::make_unique<MockObserver>();
    auto* raw1 = obs1.get();
    auto* raw2 = obs2.get();
    dispatcher.addObserver(std::move(obs1));
    dispatcher.addObserver(std::move(obs2));

    dispatcher.notify(WithLock::withoutLock(), Timestamp(7, 1));

    ASSERT_TRUE(raw1->waitForCount(1));
    ASSERT_TRUE(raw2->waitForCount(1));

    ASSERT_EQ(Timestamp(7, 1), raw1->timestamps()[0]);
    ASSERT_EQ(Timestamp(7, 1), raw2->timestamps()[0]);
}

// ---------------------------------------------------------------------------
// 5. The observer is called for each successive notify(), in order.
// ---------------------------------------------------------------------------
TEST_F(DispatcherTest, ObserverReceivesEachNotifyInOrder) {
    auto obs = std::make_unique<MockObserver>();
    auto* raw = obs.get();
    dispatcher.addObserver(std::move(obs));

    const std::vector<Timestamp> expected = {
        Timestamp(10, 1),
        Timestamp(20, 1),
        Timestamp(30, 1),
        Timestamp(40, 1),
        Timestamp(50, 1),
    };

    for (size_t i = 0; i < expected.size(); ++i) {
        dispatcher.notify(WithLock::withoutLock(), expected[i]);
        // Wait for each notification before sending the next to prevent coalescing.
        ASSERT_TRUE(raw->waitForCount(i + 1));
    }

    const auto ts = raw->timestamps();
    ASSERT_EQ(expected.size(), ts.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQ(expected[i], ts[i]);
    }
}

}  // namespace
}  // namespace repl
}  // namespace mongo
