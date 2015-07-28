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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/lock_manager_test_help.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(LockStats, NoWait) {
    const ResourceId resId(RESOURCE_COLLECTION, std::string("LockStats.NoWait"));

    resetGlobalLockStats();

    LockerForTests locker(MODE_IX);
    locker.lock(resId, MODE_X);
    locker.unlock(resId);

    // Make sure that the waits/blocks are zero
    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);

    ASSERT_EQUALS(1, stats.get(resId, MODE_X).numAcquisitions);
    ASSERT_EQUALS(0, stats.get(resId, MODE_X).numWaits);
    ASSERT_EQUALS(0, stats.get(resId, MODE_X).combinedWaitTimeMicros);
}

TEST(LockStats, Wait) {
    const ResourceId resId(RESOURCE_COLLECTION, std::string("LockStats.Wait"));

    resetGlobalLockStats();

    LockerForTests locker(MODE_IX);
    locker.lock(resId, MODE_X);

    {
        // This will block
        LockerForTests lockerConflict(MODE_IX);
        ASSERT_EQUALS(LOCK_WAITING, lockerConflict.lockBegin(resId, MODE_S));

        // Sleep 1 millisecond so the wait time passes
        ASSERT_EQUALS(LOCK_TIMEOUT, lockerConflict.lockComplete(resId, MODE_S, 1, false));
    }

    // Make sure that the waits/blocks are non-zero
    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);

    ASSERT_EQUALS(1, stats.get(resId, MODE_X).numAcquisitions);
    ASSERT_EQUALS(0, stats.get(resId, MODE_X).numWaits);
    ASSERT_EQUALS(0, stats.get(resId, MODE_X).combinedWaitTimeMicros);

    ASSERT_EQUALS(1, stats.get(resId, MODE_S).numAcquisitions);
    ASSERT_EQUALS(1, stats.get(resId, MODE_S).numWaits);
    ASSERT_GREATER_THAN(stats.get(resId, MODE_S).combinedWaitTimeMicros, 0);
}

TEST(LockStats, Reporting) {
    const ResourceId resId(RESOURCE_COLLECTION, std::string("LockStats.Reporting"));

    resetGlobalLockStats();

    LockerForTests locker(MODE_IX);
    locker.lock(resId, MODE_X);
    locker.unlock(resId);

    // Make sure that the waits/blocks are zero
    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);

    BSONObjBuilder builder;
    stats.report(&builder);
}

}  // namespace mongo
