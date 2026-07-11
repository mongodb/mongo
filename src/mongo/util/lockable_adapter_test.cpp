// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// IWYU pragma: no_include "cxxabi.h"
#include "mongo/util/lockable_adapter.h"

#include "mongo/stdx/condition_variable.h"
#include "mongo/unittest/unittest.h"

#include <future>
#include <memory>
#include <mutex>
#include <system_error>

namespace mongo {

namespace {

template <typename Pred>
void waitForCondition(stdx::condition_variable_any& cv, BasicLockableAdapter lock, Pred pred) {
    cv.wait(lock, pred);
}

void callUnderLock(BasicLockableAdapter adaptedLock) {
    std::lock_guard lock(adaptedLock);
    ASSERT_TRUE(true);  // got here
}

class TestLockable {
public:
    TestLockable() {}

    void lock() {
        _mutex.lock();
        ++lockCalls;
    }

    void unlock() {
        ++unlockCalls;
        _mutex.unlock();
    }

    int lockCalls{0};
    int unlockCalls{0};

private:
    std::mutex _mutex;
};

}  // namespace

TEST(BasicLockableAdapter, TestWithConditionVariable) {
    bool ready = false;
    stdx::condition_variable_any cv;
    std::mutex mut;

    auto result = std::async(std::launch::async, [&ready, &mut, &cv] {
        std::lock_guard lock(mut);
        ASSERT_FALSE(ready);
        ready = true;
        cv.notify_all();
    });

    std::unique_lock lock(mut);
    waitForCondition(cv, lock, [&ready] { return ready; });
    ASSERT_TRUE(ready);
}

TEST(BasicLockableAdapter, TestWithMutexTypes) {

    {
        std::mutex mut;
        callUnderLock(mut);
    }

    {
        std::timed_mutex mut;  // NOLINT
        callUnderLock(mut);
    }

    {
        TestLockable mut;
        callUnderLock(mut);
        ASSERT_EQ(mut.lockCalls, 1);
        ASSERT_EQ(mut.unlockCalls, 1);
    }
}

TEST(BasicLockableAdapter, TestWithCustomLockableType) {
    bool ready = false;
    stdx::condition_variable_any cv;
    TestLockable mut;

    auto result = std::async(std::launch::async, [&ready, &mut, &cv] {
        std::lock_guard lock(mut);
        ASSERT_FALSE(ready);
        ready = true;
        cv.notify_all();
    });

    {
        std::unique_lock lock(mut);
        waitForCondition(cv, lock, [&ready] { return ready; });
    }

    ASSERT_TRUE(ready);
    ASSERT_GT(mut.lockCalls, 0);
    ASSERT_EQ(mut.lockCalls, mut.unlockCalls);
}

}  // namespace mongo
