// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/scoped_unlock.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <mutex>
#include <ostream>
#include <string>

namespace mongo {
namespace {
TEST(ScopedUnlockTest, Relocked) {
    std::mutex mutex;
    std::unique_lock lk(mutex);

    {
        ScopedUnlock scopedUnlock(lk);
    }

    ASSERT(lk.owns_lock()) << "ScopedUnlock should relock on destruction";
}

TEST(ScopedUnlockTest, Unlocked) {
    std::mutex mutex;
    std::unique_lock<std::mutex> lk(mutex);

    ScopedUnlock scopedUnlock(lk);

    ASSERT_FALSE(lk.owns_lock()) << "ScopedUnlock should unlock on construction";
}

TEST(ScopedUnlockTest, Dismissed) {
    std::mutex mutex;
    std::unique_lock<std::mutex> lk(mutex);

    {
        ScopedUnlock scopedUnlock(lk);
        scopedUnlock.dismiss();
    }

    ASSERT_FALSE(lk.owns_lock()) << "ScopedUnlock should not relock on destruction if dismissed";
}

DEATH_TEST(ScopedUnlockTestDeathTest,
           InitUnlocked,
           "Locks in ScopedUnlock must be locked on initialization.") {
    std::mutex mutex;
    std::unique_lock<std::mutex> lk(mutex);
    lk.unlock();

    ScopedUnlock scopedUnlock(lk);
}
}  // namespace
}  // namespace mongo
