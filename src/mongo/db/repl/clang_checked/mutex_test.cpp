// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/clang_checked/mutex.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
TEST(MutexTest, LockGuardShouldLock) {
    std::mutex m;
    clang_checked::lock_guard lk(m);
    // If lock_guard locked m, then try_lock() should return false.
    ASSERT_FALSE(m.try_lock());
}

TEST(MutexTest, UniqueLockShouldLockAndUnlock) {
    std::mutex m;
    clang_checked::unique_lock lk(m);
    ASSERT_TRUE(lk.owns_lock());
    lk.unlock();
    ASSERT_FALSE(lk.owns_lock());
}

TEST(MutexTest, UniqueLockShouldTryLockAndOwnsLock) MONGO_LOCKING_NO_THREAD_SAFETY_ANALYSIS {
    std::mutex m;
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
    std::mutex m;
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
    std::mutex m;
    clang_checked::lock_guard lk(m);
    takesWithLock(lk);
}

TEST(MutexTest, UniqueLockShouldConvertToWithLock) {
    std::mutex m;
    clang_checked::unique_lock lk(m);
    takesWithLock(lk);
}
}  // namespace
}  // namespace mongo
