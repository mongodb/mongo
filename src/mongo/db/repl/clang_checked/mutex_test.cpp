/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/repl/clang_checked/mutex.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
TEST(MutexTest, LockGuardShouldLock) {
    stdx::mutex m;
    clang_checked::lock_guard lk(m);
    // If lock_guard locked m, then try_lock() should return false.
    ASSERT_FALSE(m.try_lock());
}

TEST(MutexTest, UniqueLockShouldLockAndUnlock) {
    stdx::mutex m;
    clang_checked::unique_lock lk(m);
    ASSERT_TRUE(lk.owns_lock());
    lk.unlock();
    ASSERT_FALSE(lk.owns_lock());
}

TEST(MutexTest, UniqueLockShouldTryLockAndOwnsLock) MONGO_LOCKING_NO_THREAD_SAFETY_ANALYSIS {
    stdx::mutex m;
    clang_checked::unique_lock lk(m);
    ASSERT_FALSE(lk.try_lock());
    ASSERT_TRUE(lk.owns_lock());
    lk.unlock();
    ASSERT_FALSE(lk.owns_lock());
    ASSERT_TRUE(lk.try_lock());
    ASSERT_TRUE(lk.owns_lock());
    lk.unlock();
    ASSERT_FALSE(lk.owns_lock());
}

TEST(MutexTest, UniqueLockShouldConvertToBoolean) MONGO_LOCKING_NO_THREAD_SAFETY_ANALYSIS {
    stdx::mutex m;
    clang_checked::unique_lock lk(m);
    ASSERT_FALSE(lk.try_lock());
    ASSERT_TRUE(lk);
    lk.unlock();
    ASSERT_FALSE(lk);
    ASSERT_TRUE(lk.try_lock());
    ASSERT_TRUE(lk);
    lk.unlock();
    ASSERT_FALSE(lk);
}

class MONGO_LOCKING_SCOPED_CAPABILITY MutexWithId {
public:
    explicit MutexWithId(int id) : _id(id) {}

    void lock() MONGO_LOCKING_ACQUIRE() {}

    void unlock() MONGO_LOCKING_RELEASE() {}

    int id() const {
        return _id;
    }

private:
    int _id;
};

TEST(MutexTest, UniqueLockShouldReturnMutex) MONGO_LOCKING_NO_THREAD_SAFETY_ANALYSIS {
    MutexWithId m(3);
    clang_checked::unique_lock lk(m);
    ASSERT_EQ(lk.mutex()->id(), m.id());
    lk.lock();
    ASSERT_EQ(lk.mutex()->id(), m.id());
    lk.unlock();
    ASSERT_EQ(lk.mutex()->id(), m.id());
}

TEST(MutexTest, UniqueLockCanSwapWhenBothLocked) MONGO_LOCKING_NO_THREAD_SAFETY_ANALYSIS {
    MutexWithId m0(0);
    clang_checked::unique_lock lk0(m0);
    MutexWithId m1(1);
    clang_checked::unique_lock lk1(m1);
    ASSERT_EQ(lk0.mutex()->id(), m0.id());
    ASSERT_EQ(lk1.mutex()->id(), m1.id());
    ASSERT_TRUE(lk0.owns_lock());
    ASSERT_TRUE(lk1.owns_lock());
    lk0.swap(lk1);
    ASSERT_EQ(lk1.mutex()->id(), m0.id());
    ASSERT_EQ(lk0.mutex()->id(), m1.id());
    ASSERT_TRUE(lk0.owns_lock());
    ASSERT_TRUE(lk1.owns_lock());
}

TEST(MutexTest, UniqueLockCanSwapWhenNeitherLocked) MONGO_LOCKING_NO_THREAD_SAFETY_ANALYSIS {
    MutexWithId m0(0);
    clang_checked::unique_lock lk0(m0);
    lk0.unlock();
    MutexWithId m1(1);
    clang_checked::unique_lock lk1(m1);
    lk1.unlock();
    ASSERT_EQ(lk0.mutex()->id(), m0.id());
    ASSERT_EQ(lk1.mutex()->id(), m1.id());
    ASSERT_FALSE(lk0.owns_lock());
    ASSERT_FALSE(lk1.owns_lock());
    lk0.swap(lk1);
    ASSERT_EQ(lk1.mutex()->id(), m0.id());
    ASSERT_EQ(lk0.mutex()->id(), m1.id());
    ASSERT_FALSE(lk0.owns_lock());
    ASSERT_FALSE(lk1.owns_lock());
}

TEST(MutexTest, UniqueLockCanSwapWhenFirstLockedSecondUnlocked)
MONGO_LOCKING_NO_THREAD_SAFETY_ANALYSIS {
    MutexWithId m0(0);
    clang_checked::unique_lock lk0(m0);
    MutexWithId m1(1);
    clang_checked::unique_lock lk1(m1);
    lk1.unlock();
    ASSERT_EQ(lk0.mutex()->id(), m0.id());
    ASSERT_EQ(lk1.mutex()->id(), m1.id());
    ASSERT_TRUE(lk0.owns_lock());
    ASSERT_FALSE(lk1.owns_lock());
    lk0.swap(lk1);
    ASSERT_EQ(lk1.mutex()->id(), m0.id());
    ASSERT_EQ(lk0.mutex()->id(), m1.id());
    ASSERT_FALSE(lk0.owns_lock());
    ASSERT_TRUE(lk1.owns_lock());
}

TEST(MutexTest, UniqueLockCanSwapWhenFirstUnlockedSecondLocked)
MONGO_LOCKING_NO_THREAD_SAFETY_ANALYSIS {
    MutexWithId m0(0);
    clang_checked::unique_lock lk0(m0);
    lk0.unlock();
    MutexWithId m1(1);
    clang_checked::unique_lock lk1(m1);
    ASSERT_EQ(lk0.mutex()->id(), m0.id());
    ASSERT_EQ(lk1.mutex()->id(), m1.id());
    ASSERT_FALSE(lk0.owns_lock());
    ASSERT_TRUE(lk1.owns_lock());
    lk0.swap(lk1);
    ASSERT_EQ(lk1.mutex()->id(), m0.id());
    ASSERT_EQ(lk0.mutex()->id(), m1.id());
    ASSERT_TRUE(lk0.owns_lock());
    ASSERT_FALSE(lk1.owns_lock());
}

void takesWithLock(WithLock) {}

TEST(MutexTest, LockGuardShouldConvertToWithLock) {
    stdx::mutex m;
    clang_checked::lock_guard lk(m);
    takesWithLock(lk);
}

TEST(MutexTest, UniqueLockShouldConvertToWithLock) {
    stdx::mutex m;
    clang_checked::unique_lock lk(m);
    takesWithLock(lk);
}
}  // namespace
}  // namespace mongo
