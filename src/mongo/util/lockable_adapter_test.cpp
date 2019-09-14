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

#include "mongo/unittest/unittest.h"

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/lockable_adapter.h"

namespace mongo {

namespace {

template <typename Pred>
void waitForCondition(stdx::condition_variable_any& cv, BasicLockableAdapter lock, Pred pred) {
    cv.wait(lock, pred);
}

void callUnderLock(BasicLockableAdapter adaptedLock) {
    stdx::lock_guard lock(adaptedLock);
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
    stdx::mutex _mutex;
};

}  // namespace

TEST(BasicLockableAdapter, TestWithConditionVariable) {
    bool ready = false;
    stdx::condition_variable_any cv;
    stdx::mutex mut;

    auto result = stdx::async(stdx::launch::async, [&ready, &mut, &cv] {
        stdx::lock_guard lock(mut);
        ASSERT_FALSE(ready);
        ready = true;
        cv.notify_all();
    });

    stdx::unique_lock lock(mut);
    waitForCondition(cv, lock, [&ready] { return ready; });
    ASSERT_TRUE(ready);
}

TEST(BasicLockableAdapter, TestWithMutexTypes) {

    {
        stdx::mutex mut;
        callUnderLock(mut);
    }

    {
        stdx::timed_mutex mut;
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

    auto result = stdx::async(stdx::launch::async, [&ready, &mut, &cv] {
        stdx::lock_guard lock(mut);
        ASSERT_FALSE(ready);
        ready = true;
        cv.notify_all();
    });

    {
        stdx::unique_lock lock(mut);
        waitForCondition(cv, lock, [&ready] { return ready; });
    }

    ASSERT_TRUE(ready);
    ASSERT_GT(mut.lockCalls, 0);
    ASSERT_EQ(mut.lockCalls, mut.unlockCalls);
}

}  // namespace mongo
