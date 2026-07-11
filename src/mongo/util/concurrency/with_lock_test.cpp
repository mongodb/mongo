// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/concurrency/with_lock.h"

#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <mutex>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {

namespace {

struct Beerp {
    explicit Beerp(int i) {
        _blerp(WithLock::withoutLock(), i);
    }
    Beerp(std::lock_guard<std::mutex> const& lk, int i) {
        _blerp(lk, i);
    }
    int bleep(char n) {
        std::lock_guard lk(_m);
        return _bloop(lk, n - '0');
    }
    int bleep(int i) {
        std::unique_lock lk(_m);
        return _bloop(lk, i);
    }

private:
    int _bloop(WithLock lk, int i) {
        return _blerp(lk, i);
    }
    int _blerp(WithLock, int i) {
        LOGV2(23122, "{i} bleep(s)\n", "i"_attr = i);
        return i;
    }
    std::mutex _m;
};

TEST(WithLockTest, OverloadSet) {
    Beerp b(0);
    ASSERT_EQ(1, b.bleep('1'));
    ASSERT_EQ(2, b.bleep(2));

    std::mutex m;
    std::lock_guard lk(m);
    Beerp(lk, 3);
}

int withLock(WithLock, int i) {
    return i;
}

TEST(WithLockTest, WriteRarelyRWMutex) {
    WriteRarelyRWMutex m;
    ASSERT_EQ(withLock(m.writeLock(), 1), 1);
    ASSERT_EQ(withLock(m.readLock(), 2), 2);
}

int recursiveMoveWithLock(WithLock lk, int i) {
    return withLock(std::move(lk), i);
}

int recursiveWithLock(WithLock lk, int i) {
    return recursiveMoveWithLock(lk, i);
}

TEST(WithLockTest, RecursiveWithLock) {
    std::mutex m;
    ASSERT_EQ(recursiveWithLock(std::lock_guard(m), 1), 1);
}

constexpr std::string_view kDeathTestExpectedMessage = "lock.owns_lock()";

DEATH_TEST(WithLockDeathTest, UnlockedUniqueLock, kDeathTestExpectedMessage) {
    std::mutex m;
    std::unique_lock lk(m);
    lk.unlock();
    [[maybe_unused]] WithLock withLock{lk};  // should fail with invariant
}

DEATH_TEST(WithLockDeathTest, MovedFromWriteRarelyRWMutex, kDeathTestExpectedMessage) {
    WriteRarelyRWMutex m;
    auto lk = m.writeLock();
    // Steal the lock from lk to check the invariant.
    auto lk2 = std::move(lk);
    [[maybe_unused]] WithLock withLock{lk};  // should fail with invariant
}

}  // namespace
}  // namespace mongo
