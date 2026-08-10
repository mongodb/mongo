// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/lock_guard_with_deferred_task.h"

#include "mongo/unittest/unittest.h"

#include <mutex>
#include <string>
#include <vector>

namespace mongo {
namespace repl {
namespace {

using testing::ElementsAre;

using Guard = LockGuardWithDeferredTask<std::mutex>;

TEST(LockGuardWithDeferredTaskTest, TaskRunsAfterTheLockIsReleased) {
    std::mutex mutex;
    bool ran = false;
    bool heldWhileRunning = true;
    {
        Guard guard(mutex);
        guard.scheduleDeferredTask([&] {
            ran = true;
            // The guard has released the mutex, so taking it here must succeed.
            if (mutex.try_lock()) {
                heldWhileRunning = false;
                mutex.unlock();
            }
        });
        ASSERT_FALSE(ran);  // Nothing runs until the guard goes out of scope.
    }
    ASSERT_TRUE(ran);
    ASSERT_FALSE(heldWhileRunning);
}

TEST(LockGuardWithDeferredTaskTest, TasksRunInTheOrderScheduled) {
    std::mutex mutex;
    std::vector<std::string> order;
    {
        Guard guard(mutex);
        guard.scheduleDeferredTask([&] { order.push_back("first"); });
        guard.scheduleDeferredTask([&] { order.push_back("second"); });
        guard.scheduleDeferredTask([&] { order.push_back("third"); });
    }
    ASSERT_THAT(order, ElementsAre("first", "second", "third"));
}

TEST(LockGuardWithDeferredTaskTest, SchedulingAnEmptyTaskThrows) {
    std::mutex mutex;
    Guard guard(mutex);
    ASSERT_THROWS_CODE(guard.scheduleDeferredTask({}), DBException, ErrorCodes::IllegalOperation);
}

TEST(LockGuardWithDeferredTaskTest, DestructorRunsTheTaskEvenWhenTheGuardUnlockedEarly) {
    std::mutex mutex;
    bool ran = false;
    {
        Guard guard(mutex);
        guard.scheduleDeferredTask([&] { ran = true; });
        guard.unlock();
        ASSERT_FALSE(guard.owns_lock());
        ASSERT_FALSE(ran);
    }
    ASSERT_TRUE(ran);
}

TEST(LockGuardWithDeferredTaskTest, TaskMayTakeTheMutexThroughAnotherGuard) {
    std::mutex mutex;
    bool innerRan = false;
    bool outerRan = false;
    {
        Guard guard(mutex);
        guard.scheduleDeferredTask([&] {
            outerRan = true;
            // The outer guard is no longer live, so the task may take the mutex again -- and the
            // task it schedules must not be picked up by the guard that is draining.
            Guard inner(mutex);
            inner.scheduleDeferredTask([&] { innerRan = true; });
        });
    }
    ASSERT_TRUE(outerRan);
    ASSERT_TRUE(innerRan);
}

TEST(LockGuardWithDeferredTaskTest, GuardIsUsableAsWithLock) {
    std::mutex mutex;
    Guard guard(mutex);
    auto takesWithLock = [](WithLock) {
        return true;
    };
    ASSERT_TRUE(takesWithLock(guard));
}

}  // namespace
}  // namespace repl
}  // namespace mongo
