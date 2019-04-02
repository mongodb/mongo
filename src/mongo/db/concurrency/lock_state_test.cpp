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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <algorithm>
#include <string>
#include <vector>

#include "mongo/config.h"
#include "mongo/db/concurrency/lock_manager_test_help.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

TEST(LockerImpl, LockNoConflict) {
    const ResourceId resId(RESOURCE_COLLECTION, "TestDB.collection"_sd);

    LockerImpl locker;
    locker.lockGlobal(MODE_IX);

    locker.lock(resId, MODE_X);

    ASSERT(locker.isLockHeldForMode(resId, MODE_X));
    ASSERT(locker.isLockHeldForMode(resId, MODE_S));

    ASSERT(locker.unlock(resId));

    ASSERT(locker.isLockHeldForMode(resId, MODE_NONE));

    locker.unlockGlobal();
}

TEST(LockerImpl, ReLockNoConflict) {
    const ResourceId resId(RESOURCE_COLLECTION, "TestDB.collection"_sd);

    LockerImpl locker;
    locker.lockGlobal(MODE_IX);

    locker.lock(resId, MODE_S);
    locker.lock(resId, MODE_X);

    ASSERT(!locker.unlock(resId));
    ASSERT(locker.isLockHeldForMode(resId, MODE_X));

    ASSERT(locker.unlock(resId));
    ASSERT(locker.isLockHeldForMode(resId, MODE_NONE));

    ASSERT(locker.unlockGlobal());
}

TEST(LockerImpl, ConflictWithTimeout) {
    const ResourceId resId(RESOURCE_COLLECTION, "TestDB.collection"_sd);

    LockerImpl locker1;
    locker1.lockGlobal(MODE_IX);
    locker1.lock(resId, MODE_X);

    LockerImpl locker2;
    locker2.lockGlobal(MODE_IX);
    ASSERT_THROWS_CODE(
        locker2.lock(resId, MODE_S, Date_t::now()), AssertionException, ErrorCodes::LockTimeout);

    ASSERT(locker2.getLockMode(resId) == MODE_NONE);

    ASSERT(locker1.unlock(resId));

    ASSERT(locker1.unlockGlobal());
    ASSERT(locker2.unlockGlobal());
}

TEST(LockerImpl, ConflictUpgradeWithTimeout) {
    const ResourceId resId(RESOURCE_COLLECTION, "TestDB.collection"_sd);

    LockerImpl locker1;
    locker1.lockGlobal(MODE_IS);
    locker1.lock(resId, MODE_S);

    LockerImpl locker2;
    locker2.lockGlobal(MODE_IS);
    locker2.lock(resId, MODE_S);

    // Try upgrading locker 1, which should block and timeout
    ASSERT_THROWS_CODE(locker1.lock(resId, MODE_X, Date_t::now() + Milliseconds(1)),
                       AssertionException,
                       ErrorCodes::LockTimeout);

    locker1.unlockGlobal();
    locker2.unlockGlobal();
}

TEST(LockerImpl, FailPointInLockFailsGlobalNonIntentLocksIfTheyCannotBeImmediatelyGranted) {
    LockerImpl locker1;
    locker1.lockGlobal(MODE_IX);

    {
        FailPointEnableBlock failWaitingNonPartitionedLocks("failNonIntentLocksIfWaitNeeded");

        // MODE_S attempt.
        LockerImpl locker2;
        ASSERT_THROWS_CODE(locker2.lockGlobal(MODE_S), DBException, ErrorCodes::LockTimeout);

        // MODE_X attempt.
        LockerImpl locker3;
        ASSERT_THROWS_CODE(locker3.lockGlobal(MODE_X), DBException, ErrorCodes::LockTimeout);
    }

    locker1.unlockGlobal();
}

TEST(LockerImpl, FailPointInLockFailsNonIntentLocksIfTheyCannotBeImmediatelyGranted) {
    // Granted MODE_X lock, fail incoming MODE_S and MODE_X.
    const ResourceId resId(RESOURCE_COLLECTION, "TestDB.collection"_sd);

    LockerImpl locker1;
    locker1.lockGlobal(MODE_IX);
    locker1.lock(resId, MODE_X);

    {
        FailPointEnableBlock failWaitingNonPartitionedLocks("failNonIntentLocksIfWaitNeeded");

        // MODE_S attempt.
        LockerImpl locker2;
        locker2.lockGlobal(MODE_IS);
        ASSERT_THROWS_CODE(
            locker2.lock(resId, MODE_S, Date_t::max()), DBException, ErrorCodes::LockTimeout);

        // The timed out MODE_S attempt shouldn't be present in the map of lock requests because it
        // won't ever be granted.
        ASSERT(locker2.getRequestsForTest().find(resId).finished());
        locker2.unlockGlobal();

        // MODE_X attempt.
        LockerImpl locker3;
        locker3.lockGlobal(MODE_IX);
        ASSERT_THROWS_CODE(
            locker3.lock(resId, MODE_X, Date_t::max()), DBException, ErrorCodes::LockTimeout);

        // The timed out MODE_X attempt shouldn't be present in the map of lock requests because it
        // won't ever be granted.
        ASSERT(locker3.getRequestsForTest().find(resId).finished());
        locker3.unlockGlobal();
    }

    locker1.unlockGlobal();
}

TEST(LockerImpl, ReadTransaction) {
    LockerImpl locker;

    locker.lockGlobal(MODE_IS);
    locker.unlockGlobal();

    locker.lockGlobal(MODE_IX);
    locker.unlockGlobal();

    locker.lockGlobal(MODE_IX);
    locker.lockGlobal(MODE_IS);
    locker.unlockGlobal();
    locker.unlockGlobal();
}

/**
 * Test that saveLockerImpl works by examining the output.
 */
TEST(LockerImpl, saveAndRestoreGlobal) {
    Locker::LockSnapshot lockInfo;

    LockerImpl locker;

    // No lock requests made, no locks held.
    locker.saveLockStateAndUnlock(&lockInfo);
    ASSERT_EQUALS(0U, lockInfo.locks.size());

    // Lock the global lock, but just once.
    locker.lockGlobal(MODE_IX);

    // We've locked the global lock.  This should be reflected in the lockInfo.
    locker.saveLockStateAndUnlock(&lockInfo);
    ASSERT(!locker.isLocked());
    ASSERT_EQUALS(MODE_IX, lockInfo.globalMode);

    // Restore the lock(s) we had.
    locker.restoreLockState(lockInfo);

    ASSERT(locker.isLocked());
    ASSERT(locker.unlockGlobal());
}

/**
 * Test that saveLockerImpl can save and restore the RSTL.
 */
TEST(LockerImpl, saveAndRestoreRSTL) {
    Locker::LockSnapshot lockInfo;

    LockerImpl locker;

    const ResourceId resIdDatabase(RESOURCE_DATABASE, "TestDB"_sd);

    // Acquire locks.
    locker.lock(resourceIdReplicationStateTransitionLock, MODE_IX);
    locker.lockGlobal(MODE_IX);
    locker.lock(resIdDatabase, MODE_IX);

    // Save the lock state.
    locker.saveLockStateAndUnlock(&lockInfo);
    ASSERT(!locker.isLocked());
    ASSERT_EQUALS(MODE_IX, lockInfo.globalMode);

    // Check locks are unlocked.
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resourceIdReplicationStateTransitionLock));
    ASSERT(!locker.isLocked());
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));

    // Restore the lock(s) we had.
    locker.restoreLockState(lockInfo);

    // Check locks are re-locked.
    ASSERT(locker.isLocked());
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resourceIdReplicationStateTransitionLock));

    ASSERT(locker.unlockGlobal());
    ASSERT(locker.unlock(resourceIdReplicationStateTransitionLock));
}

/**
 * Test that we don't unlock when we have the global lock more than once.
 */
TEST(LockerImpl, saveAndRestoreGlobalAcquiredTwice) {
    Locker::LockSnapshot lockInfo;

    LockerImpl locker;

    // No lock requests made, no locks held.
    locker.saveLockStateAndUnlock(&lockInfo);
    ASSERT_EQUALS(0U, lockInfo.locks.size());

    // Lock the global lock.
    locker.lockGlobal(MODE_IX);
    locker.lockGlobal(MODE_IX);

    // This shouldn't actually unlock as we're in a nested scope.
    ASSERT(!locker.saveLockStateAndUnlock(&lockInfo));

    ASSERT(locker.isLocked());

    // We must unlockGlobal twice.
    ASSERT(!locker.unlockGlobal());
    ASSERT(locker.unlockGlobal());
}

/**
 * Tests that restoreLockerImpl works by locking a db and collection and saving + restoring.
 */
TEST(LockerImpl, saveAndRestoreDBAndCollection) {
    Locker::LockSnapshot lockInfo;

    LockerImpl locker;

    const ResourceId resIdDatabase(RESOURCE_DATABASE, "TestDB"_sd);
    const ResourceId resIdCollection(RESOURCE_COLLECTION, "TestDB.collection"_sd);

    // Lock some stuff.
    locker.lockGlobal(MODE_IX);
    locker.lock(resIdDatabase, MODE_IX);
    locker.lock(resIdCollection, MODE_X);
    locker.saveLockStateAndUnlock(&lockInfo);

    // Things shouldn't be locked anymore.
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));

    // Restore lock state.
    locker.restoreLockState(lockInfo);

    // Make sure things were re-locked.
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_X, locker.getLockMode(resIdCollection));

    ASSERT(locker.unlockGlobal());
}

TEST(LockerImpl, releaseWriteUnitOfWork) {
    Locker::LockSnapshot lockInfo;

    LockerImpl locker;

    const ResourceId resIdDatabase(RESOURCE_DATABASE, "TestDB"_sd);
    const ResourceId resIdCollection(RESOURCE_COLLECTION, "TestDB.collection"_sd);

    locker.beginWriteUnitOfWork();
    // Lock some stuff.
    locker.lockGlobal(MODE_IX);
    locker.lock(resIdDatabase, MODE_IX);
    locker.lock(resIdCollection, MODE_X);
    // Unlock them so that they will be pending to unlock.
    ASSERT_FALSE(locker.unlock(resIdCollection));
    ASSERT_FALSE(locker.unlock(resIdDatabase));
    ASSERT_FALSE(locker.unlockGlobal());

    ASSERT(locker.releaseWriteUnitOfWork(&lockInfo));

    // Things shouldn't be locked anymore.
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());

    // Destructor should succeed since the locker's state should be empty.
}

TEST(LockerImpl, restoreWriteUnitOfWork) {
    Locker::LockSnapshot lockInfo;

    LockerImpl locker;

    const ResourceId resIdDatabase(RESOURCE_DATABASE, "TestDB"_sd);
    const ResourceId resIdCollection(RESOURCE_COLLECTION, "TestDB.collection"_sd);

    locker.beginWriteUnitOfWork();
    // Lock some stuff.
    locker.lockGlobal(MODE_IX);
    locker.lock(resIdDatabase, MODE_IX);
    locker.lock(resIdCollection, MODE_X);
    // Unlock them so that they will be pending to unlock.
    ASSERT_FALSE(locker.unlock(resIdCollection));
    ASSERT_FALSE(locker.unlock(resIdDatabase));
    ASSERT_FALSE(locker.unlockGlobal());

    ASSERT(locker.releaseWriteUnitOfWork(&lockInfo));

    // Things shouldn't be locked anymore.
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());

    // Restore lock state.
    locker.restoreWriteUnitOfWork(nullptr, lockInfo);

    // Make sure things were re-locked.
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_X, locker.getLockMode(resIdCollection));
    ASSERT(locker.isLocked());

    locker.endWriteUnitOfWork();

    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());
}

TEST(LockerImpl, releaseAndRestoreReadOnlyWriteUnitOfWork) {
    Locker::LockSnapshot lockInfo;

    LockerImpl locker;

    const ResourceId resIdDatabase(RESOURCE_DATABASE, "TestDB"_sd);
    const ResourceId resIdCollection(RESOURCE_COLLECTION, "TestDB.collection"_sd);

    // Snapshot transactions delay shared locks as well.
    locker.setSharedLocksShouldTwoPhaseLock(true);

    locker.beginWriteUnitOfWork();
    // Lock some stuff in IS mode.
    locker.lockGlobal(MODE_IS);
    locker.lock(resIdDatabase, MODE_IS);
    locker.lock(resIdCollection, MODE_IS);
    // Unlock them.
    ASSERT_FALSE(locker.unlock(resIdCollection));
    ASSERT_FALSE(locker.unlock(resIdDatabase));
    ASSERT_FALSE(locker.unlockGlobal());
    ASSERT_EQ(3u, locker.numResourcesToUnlockAtEndUnitOfWorkForTest());

    // Things shouldn't be locked anymore.
    ASSERT_TRUE(locker.releaseWriteUnitOfWork(&lockInfo));

    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());

    // Restore lock state.
    locker.restoreWriteUnitOfWork(nullptr, lockInfo);

    ASSERT_EQUALS(MODE_IS, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_IS, locker.getLockMode(resIdCollection));
    ASSERT_TRUE(locker.isLocked());

    locker.endWriteUnitOfWork();

    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());
}

TEST(LockerImpl, releaseAndRestoreEmptyWriteUnitOfWork) {
    Locker::LockSnapshot lockInfo;
    LockerImpl locker;

    // Snapshot transactions delay shared locks as well.
    locker.setSharedLocksShouldTwoPhaseLock(true);

    locker.beginWriteUnitOfWork();

    // Nothing to yield.
    ASSERT_FALSE(locker.releaseWriteUnitOfWork(&lockInfo));
    ASSERT_FALSE(locker.isLocked());

    // Restore lock state.
    locker.restoreWriteUnitOfWork(nullptr, lockInfo);
    ASSERT_FALSE(locker.isLocked());

    locker.endWriteUnitOfWork();
    ASSERT_FALSE(locker.isLocked());
}

TEST(LockerImpl, DefaultLocker) {
    const ResourceId resId(RESOURCE_DATABASE, "TestDB"_sd);

    LockerImpl locker;
    locker.lockGlobal(MODE_IX);
    locker.lock(resId, MODE_X);

    // Make sure the flush lock IS NOT held
    Locker::LockerInfo info;
    locker.getLockerInfo(&info, boost::none);
    ASSERT(!info.waitingResource.isValid());
    ASSERT_EQUALS(2U, info.locks.size());
    ASSERT_EQUALS(RESOURCE_GLOBAL, info.locks[0].resourceId.getType());
    ASSERT_EQUALS(resId, info.locks[1].resourceId);

    ASSERT(locker.unlockGlobal());
}

TEST(LockerImpl, SharedLocksShouldTwoPhaseLockIsTrue) {
    // Test that when setSharedLocksShouldTwoPhaseLock is true and we are in a WUOW, unlock on IS
    // and S locks are postponed until endWriteUnitOfWork() is called. Mode IX and X locks always
    // participate in two-phased locking, regardless of the setting.

    const ResourceId globalResId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL);
    const ResourceId resId1(RESOURCE_DATABASE, "TestDB1"_sd);
    const ResourceId resId2(RESOURCE_DATABASE, "TestDB2"_sd);
    const ResourceId resId3(RESOURCE_COLLECTION, "TestDB.collection3"_sd);
    const ResourceId resId4(RESOURCE_COLLECTION, "TestDB.collection4"_sd);

    LockerImpl locker;
    locker.setSharedLocksShouldTwoPhaseLock(true);

    locker.lockGlobal(MODE_IS);
    ASSERT_EQ(locker.getLockMode(globalResId), MODE_IS);

    locker.lock(resourceIdReplicationStateTransitionLock, MODE_IS);
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_IS);

    locker.lock(resId1, MODE_IS);
    locker.lock(resId2, MODE_IX);
    locker.lock(resId3, MODE_S);
    locker.lock(resId4, MODE_X);
    ASSERT_EQ(locker.getLockMode(resId1), MODE_IS);
    ASSERT_EQ(locker.getLockMode(resId2), MODE_IX);
    ASSERT_EQ(locker.getLockMode(resId3), MODE_S);
    ASSERT_EQ(locker.getLockMode(resId4), MODE_X);

    locker.beginWriteUnitOfWork();

    ASSERT_FALSE(locker.unlock(resId1));
    ASSERT_FALSE(locker.unlock(resId2));
    ASSERT_FALSE(locker.unlock(resId3));
    ASSERT_FALSE(locker.unlock(resId4));
    ASSERT_EQ(locker.getLockMode(resId1), MODE_IS);
    ASSERT_EQ(locker.getLockMode(resId2), MODE_IX);
    ASSERT_EQ(locker.getLockMode(resId3), MODE_S);
    ASSERT_EQ(locker.getLockMode(resId4), MODE_X);

    ASSERT_FALSE(locker.unlock(resourceIdReplicationStateTransitionLock));
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_IS);

    ASSERT_FALSE(locker.unlockGlobal());
    ASSERT_EQ(locker.getLockMode(globalResId), MODE_IS);

    locker.endWriteUnitOfWork();

    ASSERT_EQ(locker.getLockMode(resId1), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(resId2), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(resId3), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(resId4), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(globalResId), MODE_NONE);
}

TEST(LockerImpl, ModeIXAndXLockParticipatesInTwoPhaseLocking) {
    // Unlock on mode IX and X locks during a WUOW should always be postponed until
    // endWriteUnitOfWork() is called. Mode IS and S locks should unlock immediately.

    const ResourceId globalResId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL);
    const ResourceId resId1(RESOURCE_DATABASE, "TestDB1"_sd);
    const ResourceId resId2(RESOURCE_DATABASE, "TestDB2"_sd);
    const ResourceId resId3(RESOURCE_COLLECTION, "TestDB.collection3"_sd);
    const ResourceId resId4(RESOURCE_COLLECTION, "TestDB.collection4"_sd);

    LockerImpl locker;

    locker.lockGlobal(MODE_IX);
    ASSERT_EQ(locker.getLockMode(globalResId), MODE_IX);

    locker.lock(resourceIdReplicationStateTransitionLock, MODE_IX);
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    locker.lock(resId1, MODE_IS);
    locker.lock(resId2, MODE_IX);
    locker.lock(resId3, MODE_S);
    locker.lock(resId4, MODE_X);
    ASSERT_EQ(locker.getLockMode(resId1), MODE_IS);
    ASSERT_EQ(locker.getLockMode(resId2), MODE_IX);
    ASSERT_EQ(locker.getLockMode(resId3), MODE_S);
    ASSERT_EQ(locker.getLockMode(resId4), MODE_X);

    locker.beginWriteUnitOfWork();

    ASSERT_TRUE(locker.unlock(resId1));
    ASSERT_FALSE(locker.unlock(resId2));
    ASSERT_TRUE(locker.unlock(resId3));
    ASSERT_FALSE(locker.unlock(resId4));
    ASSERT_EQ(locker.getLockMode(resId1), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(resId2), MODE_IX);
    ASSERT_EQ(locker.getLockMode(resId3), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(resId4), MODE_X);

    ASSERT_FALSE(locker.unlock(resourceIdReplicationStateTransitionLock));
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    ASSERT_FALSE(locker.unlockGlobal());
    ASSERT_EQ(locker.getLockMode(globalResId), MODE_IX);

    locker.endWriteUnitOfWork();

    ASSERT_EQ(locker.getLockMode(resId2), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(resId4), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(globalResId), MODE_NONE);
}

TEST(LockerImpl, RSTLUnlocksWithNestedLock) {
    LockerImpl locker;

    locker.lock(resourceIdReplicationStateTransitionLock, MODE_IX);
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    locker.beginWriteUnitOfWork();

    // Do a nested lock acquisition.
    locker.lock(resourceIdReplicationStateTransitionLock, MODE_IX);
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    ASSERT(locker.unlockRSTLforPrepare());
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);

    ASSERT_FALSE(locker.unlockRSTLforPrepare());

    locker.endWriteUnitOfWork();

    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);

    ASSERT_FALSE(locker.unlockRSTLforPrepare());
    ASSERT_FALSE(locker.unlock(resourceIdReplicationStateTransitionLock));
}

TEST(LockerImpl, RSTLModeIXWithTwoPhaseLockingCanBeUnlockedWhenPrepared) {
    LockerImpl locker;

    locker.lock(resourceIdReplicationStateTransitionLock, MODE_IX);
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    locker.beginWriteUnitOfWork();

    ASSERT_FALSE(locker.unlock(resourceIdReplicationStateTransitionLock));
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    ASSERT(locker.unlockRSTLforPrepare());
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);

    ASSERT_FALSE(locker.unlockRSTLforPrepare());

    locker.endWriteUnitOfWork();

    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);

    ASSERT_FALSE(locker.unlockRSTLforPrepare());
    ASSERT_FALSE(locker.unlock(resourceIdReplicationStateTransitionLock));
}

TEST(LockerImpl, RSTLModeISWithTwoPhaseLockingCanBeUnlockedWhenPrepared) {
    LockerImpl locker;

    locker.lock(resourceIdReplicationStateTransitionLock, MODE_IS);
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_IS);

    locker.beginWriteUnitOfWork();

    ASSERT(locker.unlockRSTLforPrepare());
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);

    ASSERT_FALSE(locker.unlockRSTLforPrepare());

    locker.endWriteUnitOfWork();

    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);

    ASSERT_FALSE(locker.unlockRSTLforPrepare());
    ASSERT_FALSE(locker.unlock(resourceIdReplicationStateTransitionLock));
}

TEST(LockerImpl, RSTLTwoPhaseLockingBehaviorModeIS) {
    LockerImpl locker;

    locker.lock(resourceIdReplicationStateTransitionLock, MODE_IS);
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_IS);

    locker.beginWriteUnitOfWork();

    ASSERT_TRUE(locker.unlock(resourceIdReplicationStateTransitionLock));
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);

    ASSERT_FALSE(locker.unlockRSTLforPrepare());

    locker.endWriteUnitOfWork();

    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);

    ASSERT_FALSE(locker.unlockRSTLforPrepare());
    ASSERT_FALSE(locker.unlock(resourceIdReplicationStateTransitionLock));
}

TEST(LockerImpl, OverrideLockRequestTimeout) {
    const ResourceId resIdFirstDB(RESOURCE_DATABASE, "FirstDB"_sd);
    const ResourceId resIdSecondDB(RESOURCE_DATABASE, "SecondDB"_sd);

    LockerImpl locker1;
    LockerImpl locker2;

    // Set up locker2 to override lock requests' provided timeout if greater than 1000 milliseconds.
    locker2.setMaxLockTimeout(Milliseconds(1000));

    locker1.lockGlobal(MODE_IX);
    locker2.lockGlobal(MODE_IX);

    // locker1 acquires FirstDB under an exclusive lock.
    locker1.lock(resIdFirstDB, MODE_X);
    ASSERT_TRUE(locker1.isLockHeldForMode(resIdFirstDB, MODE_X));

    // locker2's attempt to acquire FirstDB with unlimited wait time should timeout after 1000
    // milliseconds and throw because _maxLockRequestTimeout is set to 1000 milliseconds.
    ASSERT_THROWS_CODE(locker2.lock(resIdFirstDB, MODE_X, Date_t::max()),
                       AssertionException,
                       ErrorCodes::LockTimeout);

    // locker2's attempt to acquire an uncontested lock should still succeed normally.
    locker2.lock(resIdSecondDB, MODE_X);

    ASSERT_TRUE(locker1.unlock(resIdFirstDB));
    ASSERT_TRUE(locker1.isLockHeldForMode(resIdFirstDB, MODE_NONE));
    ASSERT_TRUE(locker2.unlock(resIdSecondDB));
    ASSERT_TRUE(locker2.isLockHeldForMode(resIdSecondDB, MODE_NONE));

    ASSERT(locker1.unlockGlobal());
    ASSERT(locker2.unlockGlobal());
}

TEST(LockerImpl, DoNotWaitForLockAcquisition) {
    const ResourceId resIdFirstDB(RESOURCE_DATABASE, "FirstDB"_sd);
    const ResourceId resIdSecondDB(RESOURCE_DATABASE, "SecondDB"_sd);

    LockerImpl locker1;
    LockerImpl locker2;

    // Set up locker2 to immediately return if a lock is unavailable, regardless of supplied
    // deadlines in the lock request.
    locker2.setMaxLockTimeout(Milliseconds(0));

    locker1.lockGlobal(MODE_IX);
    locker2.lockGlobal(MODE_IX);

    // locker1 acquires FirstDB under an exclusive lock.
    locker1.lock(resIdFirstDB, MODE_X);
    ASSERT_TRUE(locker1.isLockHeldForMode(resIdFirstDB, MODE_X));

    // locker2's attempt to acquire FirstDB with unlimited wait time should fail immediately and
    // throw because _maxLockRequestTimeout was set to 0.
    ASSERT_THROWS_CODE(locker2.lock(resIdFirstDB, MODE_X, Date_t::max()),
                       AssertionException,
                       ErrorCodes::LockTimeout);

    // locker2's attempt to acquire an uncontested lock should still succeed normally.
    locker2.lock(resIdSecondDB, MODE_X);

    ASSERT_TRUE(locker1.unlock(resIdFirstDB));
    ASSERT_TRUE(locker1.isLockHeldForMode(resIdFirstDB, MODE_NONE));
    ASSERT_TRUE(locker2.unlock(resIdSecondDB));
    ASSERT_TRUE(locker2.isLockHeldForMode(resIdSecondDB, MODE_NONE));

    ASSERT(locker1.unlockGlobal());
    ASSERT(locker2.unlockGlobal());
}

namespace {
/**
 * Helper function to determine if 'lockerInfo' contains a lock with ResourceId 'resourceId' and
 * lock mode 'mode' within 'lockerInfo.locks'.
 */
bool lockerInfoContainsLock(const Locker::LockerInfo& lockerInfo,
                            const ResourceId& resourceId,
                            const LockMode& mode) {
    return (1U == std::count_if(lockerInfo.locks.begin(),
                                lockerInfo.locks.end(),
                                [&resourceId, &mode](const Locker::OneLock& lock) {
                                    return lock.resourceId == resourceId && lock.mode == mode;
                                }));
}
}  // namespace

TEST(LockerImpl, GetLockerInfoShouldReportHeldLocks) {
    const ResourceId globalId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL);
    const ResourceId dbId(RESOURCE_DATABASE, "TestDB"_sd);
    const ResourceId collectionId(RESOURCE_COLLECTION, "TestDB.collection"_sd);

    // Take an exclusive lock on the collection.
    LockerImpl locker;
    locker.lockGlobal(MODE_IX);
    locker.lock(dbId, MODE_IX);
    locker.lock(collectionId, MODE_X);

    // Assert it shows up in the output of getLockerInfo().
    Locker::LockerInfo lockerInfo;
    locker.getLockerInfo(&lockerInfo, boost::none);

    ASSERT(lockerInfoContainsLock(lockerInfo, globalId, MODE_IX));
    ASSERT(lockerInfoContainsLock(lockerInfo, dbId, MODE_IX));
    ASSERT(lockerInfoContainsLock(lockerInfo, collectionId, MODE_X));
    ASSERT_EQ(3U, lockerInfo.locks.size());

    ASSERT(locker.unlock(collectionId));
    ASSERT(locker.unlock(dbId));
    ASSERT(locker.unlockGlobal());
}

TEST(LockerImpl, GetLockerInfoShouldReportPendingLocks) {
    const ResourceId globalId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL);
    const ResourceId dbId(RESOURCE_DATABASE, "TestDB"_sd);
    const ResourceId collectionId(RESOURCE_COLLECTION, "TestDB.collection"_sd);

    // Take an exclusive lock on the collection.
    LockerImpl successfulLocker;
    successfulLocker.lockGlobal(MODE_IX);
    successfulLocker.lock(dbId, MODE_IX);
    successfulLocker.lock(collectionId, MODE_X);

    // Now attempt to get conflicting locks.
    LockerImpl conflictingLocker;
    conflictingLocker.lockGlobal(MODE_IS);
    conflictingLocker.lock(dbId, MODE_IS);
    ASSERT_EQ(LOCK_WAITING, conflictingLocker.lockBegin(nullptr, collectionId, MODE_IS));

    // Assert the held locks show up in the output of getLockerInfo().
    Locker::LockerInfo lockerInfo;
    conflictingLocker.getLockerInfo(&lockerInfo, boost::none);
    ASSERT(lockerInfoContainsLock(lockerInfo, globalId, MODE_IS));
    ASSERT(lockerInfoContainsLock(lockerInfo, dbId, MODE_IS));
    ASSERT(lockerInfoContainsLock(lockerInfo, collectionId, MODE_IS));
    ASSERT_EQ(3U, lockerInfo.locks.size());

    // Assert it reports that it is waiting for the collection lock.
    ASSERT_EQ(collectionId, lockerInfo.waitingResource);

    // Make sure it no longer reports waiting once unlocked.
    ASSERT(successfulLocker.unlock(collectionId));
    ASSERT(successfulLocker.unlock(dbId));
    ASSERT(successfulLocker.unlockGlobal());

    conflictingLocker.lockComplete(collectionId, MODE_IS, Date_t::now());

    conflictingLocker.getLockerInfo(&lockerInfo, boost::none);
    ASSERT_FALSE(lockerInfo.waitingResource.isValid());

    ASSERT(conflictingLocker.unlock(collectionId));
    ASSERT(conflictingLocker.unlock(dbId));
    ASSERT(conflictingLocker.unlockGlobal());
}

TEST(LockerImpl, ReaquireLockPendingUnlock) {
    const ResourceId resId(RESOURCE_COLLECTION, "TestDB.collection"_sd);

    LockerImpl locker;
    locker.lockGlobal(MODE_IS);

    locker.lock(resId, MODE_X);
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_X));

    locker.beginWriteUnitOfWork();

    ASSERT_FALSE(locker.unlock(resId));
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_X));
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 1);

    // Reacquire lock pending unlock.
    locker.lock(resId, MODE_X);
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 0);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 0);

    locker.endWriteUnitOfWork();

    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_X));

    locker.unlockGlobal();
}

TEST(LockerImpl, AcquireLockPendingUnlockWithCoveredMode) {
    const ResourceId resId(RESOURCE_COLLECTION, "TestDB.collection"_sd);

    LockerImpl locker;
    locker.lockGlobal(MODE_IS);

    locker.lock(resId, MODE_X);
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_X));

    locker.beginWriteUnitOfWork();

    ASSERT_FALSE(locker.unlock(resId));
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_X));
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 1);

    // Attempt to lock the resource with a mode that is covered by the existing mode.
    locker.lock(resId, MODE_IX);
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 0);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 0);

    locker.endWriteUnitOfWork();

    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_IX));

    locker.unlockGlobal();
}

TEST(LockerImpl, ConvertLockPendingUnlock) {
    const ResourceId resId(RESOURCE_COLLECTION, "TestDB.collection"_sd);

    LockerImpl locker;
    locker.lockGlobal(MODE_IS);

    locker.lock(resId, MODE_IX);
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_IX));

    locker.beginWriteUnitOfWork();

    ASSERT_FALSE(locker.unlock(resId));
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_IX));
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 1);

    // Convert lock pending unlock.
    locker.lock(resId, MODE_X);
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 1);

    locker.endWriteUnitOfWork();

    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 0);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 0);
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_X));

    locker.unlockGlobal();
}

TEST(LockerImpl, ConvertLockPendingUnlockAndUnlock) {
    const ResourceId resId(RESOURCE_COLLECTION, "TestDB.collection"_sd);

    LockerImpl locker;
    locker.lockGlobal(MODE_IS);

    locker.lock(resId, MODE_IX);
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_IX));

    locker.beginWriteUnitOfWork();

    ASSERT_FALSE(locker.unlock(resId));
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_IX));
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 1);

    // Convert lock pending unlock.
    locker.lock(resId, MODE_X);
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 1);

    // Unlock the lock conversion.
    ASSERT_FALSE(locker.unlock(resId));
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 2);

    locker.endWriteUnitOfWork();

    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 0);
    ASSERT(locker.getRequestsForTest().find(resId).finished());
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_NONE));

    locker.unlockGlobal();
}

}  // namespace mongo
