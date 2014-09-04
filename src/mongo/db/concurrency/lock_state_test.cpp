/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/unittest/unittest.h"
#include "mongo/db/concurrency/lock_mgr.h"
#include "mongo/db/concurrency/lock_mgr_test_help.h"
#include "mongo/db/concurrency/lock_state.h"


namespace mongo {
namespace newlm {
    
    TEST(Locker, LockNoConflict) {
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        Locker locker(1);

        ASSERT(LOCK_OK == locker.lock(resId, MODE_X));

        ASSERT(locker.isLockHeldForMode(resId, MODE_X));
        ASSERT(locker.isLockHeldForMode(resId, MODE_S));

        ASSERT(locker.unlock(resId));

        ASSERT(locker.isLockHeldForMode(resId, MODE_NONE));
    }

    TEST(Locker, ReLockNoConflict) {
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        Locker locker(1);

        ASSERT(LOCK_OK == locker.lock(resId, MODE_S));
        ASSERT(LOCK_OK == locker.lock(resId, MODE_X));

        ASSERT(!locker.unlock(resId));
        ASSERT(locker.isLockHeldForMode(resId, MODE_X));

        ASSERT(locker.unlock(resId));
        ASSERT(locker.isLockHeldForMode(resId, MODE_NONE));
    }

    TEST(Locker, ConflictWithTimeout) {
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        Locker locker1(1);
        ASSERT(LOCK_OK == locker1.lock(resId, MODE_X));

        Locker locker2(2);
        ASSERT(LOCK_TIMEOUT == locker2.lock(resId, MODE_S, 0));
        ASSERT(locker2.isLockHeldForMode(resId, MODE_NONE));

        ASSERT(locker1.unlock(resId));
    }

    // Randomly acquires and releases locks, just to make sure that no assertions pop-up
    TEST(Locker, RandomizedAcquireRelease) {
        // TODO: Make sure to print the seed
    }

} // namespace newlm
} // namespace mongo
