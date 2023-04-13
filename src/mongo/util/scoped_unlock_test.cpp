/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/util/scoped_unlock.h"

#include "mongo/platform/mutex.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
TEST(ScopedUnlockTest, Relocked) {
    Mutex mutex;
    stdx::unique_lock lk(mutex);

    { ScopedUnlock scopedUnlock(lk); }

    ASSERT(lk.owns_lock()) << "ScopedUnlock should relock on destruction";
}

TEST(ScopedUnlockTest, Unlocked) {
    Mutex mutex;
    stdx::unique_lock<Mutex> lk(mutex);

    ScopedUnlock scopedUnlock(lk);

    ASSERT_FALSE(lk.owns_lock()) << "ScopedUnlock should unlock on construction";
}

TEST(ScopedUnlockTest, Dismissed) {
    Mutex mutex;
    stdx::unique_lock<Mutex> lk(mutex);

    {
        ScopedUnlock scopedUnlock(lk);
        scopedUnlock.dismiss();
    }

    ASSERT_FALSE(lk.owns_lock()) << "ScopedUnlock should not relock on destruction if dismissed";
}

DEATH_TEST(ScopedUnlockTest,
           InitUnlocked,
           "Locks in ScopedUnlock must be locked on initialization.") {
    Mutex mutex;
    stdx::unique_lock<Mutex> lk(mutex);
    lk.unlock();

    ScopedUnlock scopedUnlock(lk);
}
}  // namespace
}  // namespace mongo
