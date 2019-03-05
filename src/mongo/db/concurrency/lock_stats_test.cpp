/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
        ASSERT_EQUALS(LOCK_WAITING, lockerConflict.lockBegin(nullptr, resId, MODE_S));

        // Sleep 1 millisecond so the wait time passes
        ASSERT_THROWS_CODE(
            lockerConflict.lockComplete(resId, MODE_S, Date_t::now() + Milliseconds(5)),
            AssertionException,
            ErrorCodes::LockTimeout);
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

TEST(LockStats, Subtraction) {
    const ResourceId resId(RESOURCE_COLLECTION, std::string("LockStats.Subtraction"));

    resetGlobalLockStats();

    LockerForTests locker(MODE_IX);
    locker.lock(resId, MODE_X);

    {
        LockerForTests lockerConflict(MODE_IX);
        ASSERT_THROWS_CODE(lockerConflict.lock(resId, MODE_S, Date_t::now() + Milliseconds(5)),
                           AssertionException,
                           ErrorCodes::LockTimeout);
    }

    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);
    ASSERT_EQUALS(1, stats.get(resId, MODE_S).numAcquisitions);
    ASSERT_EQUALS(1, stats.get(resId, MODE_S).numWaits);
    ASSERT_GREATER_THAN(stats.get(resId, MODE_S).combinedWaitTimeMicros, 0);

    {
        LockerForTests lockerConflict(MODE_IX);
        ASSERT_THROWS_CODE(lockerConflict.lock(resId, MODE_S, Date_t::now() + Milliseconds(5)),
                           AssertionException,
                           ErrorCodes::LockTimeout);
    }

    SingleThreadedLockStats stats2;
    reportGlobalLockingStats(&stats2);
    ASSERT_EQUALS(2, stats2.get(resId, MODE_S).numAcquisitions);
    ASSERT_EQUALS(2, stats2.get(resId, MODE_S).numWaits);
    ASSERT_GREATER_THAN(stats2.get(resId, MODE_S).combinedWaitTimeMicros, 0);

    stats2.subtract(stats);
    ASSERT_EQUALS(1, stats2.get(resId, MODE_S).numAcquisitions);
    ASSERT_EQUALS(1, stats2.get(resId, MODE_S).numWaits);
    ASSERT_GREATER_THAN(stats2.get(resId, MODE_S).combinedWaitTimeMicros, 0);
}

namespace {
void assertAcquisitionStats(ResourceId rid) {
    resetGlobalLockStats();

    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);
    ASSERT_EQUALS(0, stats.get(rid, LockMode::MODE_IX).numAcquisitions);

    LockerImpl locker;
    if (rid == resourceIdGlobal) {
        locker.lockGlobal(LockMode::MODE_IX);
    } else {
        locker.lock(rid, LockMode::MODE_IX);
    }

    reportGlobalLockingStats(&stats);
    if (rid == resourceIdGlobal) {
        ASSERT_EQUALS(1, stats.get(resourceIdGlobal, LockMode::MODE_IX).numAcquisitions);
    } else {
        ASSERT_EQUALS(0, stats.get(resourceIdGlobal, LockMode::MODE_IX).numAcquisitions);
    }

    if (rid == resourceIdGlobal) {
        ASSERT_TRUE(locker.unlockGlobal());
    } else {
        ASSERT_TRUE(locker.unlock(rid));
    }
}
}  // namespace

TEST(LockStats, GlobalRetrievableSeparately) {
    assertAcquisitionStats(resourceIdGlobal);
    assertAcquisitionStats(resourceIdParallelBatchWriterMode);
    assertAcquisitionStats(resourceIdReplicationStateTransitionLock);
}

TEST(LockStats, ServerStatusAggregatesAllGlobal) {
    resetGlobalLockStats();

    SingleThreadedLockStats stats;
    reportGlobalLockingStats(&stats);
    BSONObjBuilder builder;
    stats.report(&builder);
    ASSERT_EQUALS(0, builder.done().nFields());

    LockerImpl locker;
    locker.lockGlobal(LockMode::MODE_IX);
    locker.lock(resourceIdParallelBatchWriterMode, LockMode::MODE_IX);
    locker.lock(resourceIdReplicationStateTransitionLock, LockMode::MODE_IX);

    locker.unlock(resourceIdReplicationStateTransitionLock);
    locker.unlock(resourceIdParallelBatchWriterMode);
    locker.unlockGlobal();

    reportGlobalLockingStats(&stats);
    BSONObjBuilder builder2;
    stats.report(&builder2);
    ASSERT_EQUALS(
        3,
        builder2.done().getObjectField("Global").getObjectField("acquireCount").getIntField("w"));
}

}  // namespace mongo
