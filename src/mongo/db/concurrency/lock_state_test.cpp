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

#include <algorithm>
#include <string>
#include <vector>

#include "mongo/config.h"
#include "mongo/db/concurrency/lock_manager_test_help.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {

class LockerImplTest : public ServiceContextTest {};

TEST_F(LockerImplTest, LockNoConflict) {
    auto opCtx = makeOperationContext();

    const ResourceId resId(RESOURCE_COLLECTION, NamespaceString(boost::none, "TestDB.collection"));

    LockerImpl locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IX);

    locker.lock(resId, MODE_X);

    ASSERT(locker.isLockHeldForMode(resId, MODE_X));
    ASSERT(locker.isLockHeldForMode(resId, MODE_S));

    ASSERT(locker.unlock(resId));

    ASSERT(locker.isLockHeldForMode(resId, MODE_NONE));

    locker.unlockGlobal();
}

TEST_F(LockerImplTest, ReLockNoConflict) {
    auto opCtx = makeOperationContext();

    const ResourceId resId(RESOURCE_COLLECTION, NamespaceString(boost::none, "TestDB.collection"));

    LockerImpl locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IX);

    locker.lock(resId, MODE_S);
    locker.lock(resId, MODE_X);

    ASSERT(!locker.unlock(resId));
    ASSERT(locker.isLockHeldForMode(resId, MODE_X));

    ASSERT(locker.unlock(resId));
    ASSERT(locker.isLockHeldForMode(resId, MODE_NONE));

    ASSERT(locker.unlockGlobal());
}

TEST_F(LockerImplTest, ConflictWithTimeout) {
    auto opCtx = makeOperationContext();

    const ResourceId resId(RESOURCE_COLLECTION, NamespaceString(boost::none, "TestDB.collection"));

    LockerImpl locker1(opCtx->getServiceContext());
    locker1.lockGlobal(opCtx.get(), MODE_IX);
    locker1.lock(resId, MODE_X);

    LockerImpl locker2(opCtx->getServiceContext());
    locker2.lockGlobal(opCtx.get(), MODE_IX);

    ASSERT_THROWS_CODE(locker2.lock(opCtx.get(), resId, MODE_S, Date_t::now()),
                       AssertionException,
                       ErrorCodes::LockTimeout);

    ASSERT(locker2.getLockMode(resId) == MODE_NONE);

    ASSERT(locker1.unlock(resId));

    ASSERT(locker1.unlockGlobal());
    ASSERT(locker2.unlockGlobal());
}

TEST_F(LockerImplTest, ConflictUpgradeWithTimeout) {
    auto opCtx = makeOperationContext();

    const ResourceId resId(RESOURCE_COLLECTION, NamespaceString(boost::none, "TestDB.collection"));

    LockerImpl locker1(opCtx->getServiceContext());
    locker1.lockGlobal(opCtx.get(), MODE_IS);
    locker1.lock(resId, MODE_S);

    LockerImpl locker2(opCtx->getServiceContext());
    locker2.lockGlobal(opCtx.get(), MODE_IS);
    locker2.lock(resId, MODE_S);

    // Try upgrading locker 1, which should block and timeout
    ASSERT_THROWS_CODE(locker1.lock(opCtx.get(), resId, MODE_X, Date_t::now() + Milliseconds(1)),
                       AssertionException,
                       ErrorCodes::LockTimeout);

    locker1.unlockGlobal();
    locker2.unlockGlobal();
}

TEST_F(LockerImplTest, FailPointInLockFailsGlobalNonIntentLocksIfTheyCannotBeImmediatelyGranted) {
    transport::TransportLayerMock transportLayer;
    transport::SessionHandle session = transportLayer.createSession();

    auto newClient = getServiceContext()->makeClient("userClient", session);
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();

    LockerImpl locker1(newOpCtx->getServiceContext());
    locker1.lockGlobal(newOpCtx.get(), MODE_IX);

    {
        FailPointEnableBlock failWaitingNonPartitionedLocks("failNonIntentLocksIfWaitNeeded");

        // MODE_S attempt.
        LockerImpl locker2(newOpCtx->getServiceContext());
        ASSERT_THROWS_CODE(
            locker2.lockGlobal(newOpCtx.get(), MODE_S), DBException, ErrorCodes::LockTimeout);

        // MODE_X attempt.
        LockerImpl locker3(newOpCtx->getServiceContext());
        ASSERT_THROWS_CODE(
            locker3.lockGlobal(newOpCtx.get(), MODE_X), DBException, ErrorCodes::LockTimeout);
    }

    locker1.unlockGlobal();
}

TEST_F(LockerImplTest, FailPointInLockFailsNonIntentLocksIfTheyCannotBeImmediatelyGranted) {
    transport::TransportLayerMock transportLayer;
    transport::SessionHandle session = transportLayer.createSession();

    auto newClient = getServiceContext()->makeClient("userClient", session);
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();

    // Granted MODE_X lock, fail incoming MODE_S and MODE_X.
    const ResourceId resId(RESOURCE_COLLECTION, NamespaceString(boost::none, "TestDB.collection"));

    LockerImpl locker1(newOpCtx->getServiceContext());
    locker1.lockGlobal(newOpCtx.get(), MODE_IX);
    locker1.lock(newOpCtx.get(), resId, MODE_X);

    {
        FailPointEnableBlock failWaitingNonPartitionedLocks("failNonIntentLocksIfWaitNeeded");

        // MODE_S attempt.
        LockerImpl locker2(newOpCtx->getServiceContext());
        locker2.lockGlobal(newOpCtx.get(), MODE_IS);
        ASSERT_THROWS_CODE(locker2.lock(newOpCtx.get(), resId, MODE_S, Date_t::max()),
                           DBException,
                           ErrorCodes::LockTimeout);

        // The timed out MODE_S attempt shouldn't be present in the map of lock requests because it
        // won't ever be granted.
        ASSERT(locker2.getRequestsForTest().find(resId).finished());
        locker2.unlockGlobal();

        // MODE_X attempt.
        LockerImpl locker3(newOpCtx->getServiceContext());
        locker3.lockGlobal(newOpCtx.get(), MODE_IX);
        ASSERT_THROWS_CODE(locker3.lock(newOpCtx.get(), resId, MODE_X, Date_t::max()),
                           DBException,
                           ErrorCodes::LockTimeout);

        // The timed out MODE_X attempt shouldn't be present in the map of lock requests because it
        // won't ever be granted.
        ASSERT(locker3.getRequestsForTest().find(resId).finished());
        locker3.unlockGlobal();
    }

    locker1.unlockGlobal();
}

TEST_F(LockerImplTest, ReadTransaction) {
    auto opCtx = makeOperationContext();

    LockerImpl locker(opCtx->getServiceContext());

    locker.lockGlobal(opCtx.get(), MODE_IS);
    locker.unlockGlobal();

    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.unlockGlobal();

    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lockGlobal(opCtx.get(), MODE_IS);
    locker.unlockGlobal();
    locker.unlockGlobal();
}

/**
 * Test that saveLockerImpl works by examining the output.
 */
TEST_F(LockerImplTest, saveAndRestoreGlobal) {
    auto opCtx = makeOperationContext();

    Locker::LockSnapshot lockInfo;

    LockerImpl locker(opCtx->getServiceContext());

    // No lock requests made, no locks held.
    locker.saveLockStateAndUnlock(&lockInfo);
    ASSERT_EQUALS(0U, lockInfo.locks.size());

    // Lock the global lock, but just once.
    locker.lockGlobal(opCtx.get(), MODE_IX);

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
TEST_F(LockerImplTest, saveAndRestoreRSTL) {
    auto opCtx = makeOperationContext();

    Locker::LockSnapshot lockInfo;

    LockerImpl locker(opCtx->getServiceContext());

    const ResourceId resIdDatabase(RESOURCE_DATABASE, DatabaseName(boost::none, "TestDB"));

    // Acquire locks.
    locker.lock(resourceIdReplicationStateTransitionLock, MODE_IX);
    locker.lockGlobal(opCtx.get(), MODE_IX);
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
TEST_F(LockerImplTest, saveAndRestoreGlobalAcquiredTwice) {
    auto opCtx = makeOperationContext();

    Locker::LockSnapshot lockInfo;

    LockerImpl locker(opCtx->getServiceContext());

    // No lock requests made, no locks held.
    locker.saveLockStateAndUnlock(&lockInfo);
    ASSERT_EQUALS(0U, lockInfo.locks.size());

    // Lock the global lock.
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lockGlobal(opCtx.get(), MODE_IX);

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
TEST_F(LockerImplTest, saveAndRestoreDBAndCollection) {
    auto opCtx = makeOperationContext();

    Locker::LockSnapshot lockInfo;

    LockerImpl locker(opCtx->getServiceContext());

    const ResourceId resIdDatabase(RESOURCE_DATABASE, DatabaseName(boost::none, "TestDB"));
    const ResourceId resIdCollection(RESOURCE_COLLECTION,
                                     NamespaceString(boost::none, "TestDB.collection"));

    // Lock some stuff.
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(resIdDatabase, MODE_IX);
    locker.lock(resIdCollection, MODE_IX);
    locker.saveLockStateAndUnlock(&lockInfo);

    // Things shouldn't be locked anymore.
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));

    // Restore lock state.
    locker.restoreLockState(lockInfo);

    // Make sure things were re-locked.
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdCollection));

    ASSERT(locker.unlockGlobal());
}

TEST_F(LockerImplTest, releaseWriteUnitOfWork) {
    auto opCtx = makeOperationContext();

    Locker::LockSnapshot lockInfo;

    LockerImpl locker(opCtx->getServiceContext());

    const ResourceId resIdDatabase(RESOURCE_DATABASE, DatabaseName(boost::none, "TestDB"));
    const ResourceId resIdCollection(RESOURCE_COLLECTION,
                                     NamespaceString(boost::none, "TestDB.collection"));

    locker.beginWriteUnitOfWork();
    // Lock some stuff.
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(resIdDatabase, MODE_IX);
    locker.lock(resIdCollection, MODE_IX);
    // Unlock them so that they will be pending to unlock.
    ASSERT_FALSE(locker.unlock(resIdCollection));
    ASSERT_FALSE(locker.unlock(resIdDatabase));
    ASSERT_FALSE(locker.unlockGlobal());

    ASSERT(locker.releaseWriteUnitOfWorkAndUnlock(&lockInfo));

    // Things shouldn't be locked anymore.
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());

    // Destructor should succeed since the locker's state should be empty.
}

TEST_F(LockerImplTest, restoreWriteUnitOfWork) {
    auto opCtx = makeOperationContext();

    Locker::LockSnapshot lockInfo;

    LockerImpl locker(opCtx->getServiceContext());

    const ResourceId resIdDatabase(RESOURCE_DATABASE, DatabaseName(boost::none, "TestDB"));
    const ResourceId resIdCollection(RESOURCE_COLLECTION,
                                     NamespaceString(boost::none, "TestDB.collection"));

    locker.beginWriteUnitOfWork();
    // Lock some stuff.
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(resIdDatabase, MODE_IX);
    locker.lock(resIdCollection, MODE_IX);
    // Unlock them so that they will be pending to unlock.
    ASSERT_FALSE(locker.unlock(resIdCollection));
    ASSERT_FALSE(locker.unlock(resIdDatabase));
    ASSERT_FALSE(locker.unlockGlobal());

    ASSERT(locker.releaseWriteUnitOfWorkAndUnlock(&lockInfo));

    // Things shouldn't be locked anymore.
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());

    // Restore lock state.
    locker.restoreWriteUnitOfWorkAndLock(nullptr, lockInfo);

    // Make sure things were re-locked.
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdCollection));
    ASSERT(locker.isLocked());

    locker.endWriteUnitOfWork();

    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());
}

TEST_F(LockerImplTest, releaseAndRestoreWriteUnitOfWorkWithoutUnlock) {
    auto opCtx = makeOperationContext();

    Locker::WUOWLockSnapshot lockInfo;

    LockerImpl locker(opCtx->getServiceContext());

    const ResourceId resIdDatabase(RESOURCE_DATABASE, DatabaseName(boost::none, "TestDB"));
    const ResourceId resIdCollection(RESOURCE_COLLECTION,
                                     NamespaceString(boost::none, "TestDB.collection"));
    const ResourceId resIdCollection2(RESOURCE_COLLECTION,
                                      NamespaceString(boost::none, "TestDB.collection2"));

    locker.beginWriteUnitOfWork();
    // Lock some stuff.
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(resIdDatabase, MODE_IX);
    locker.lock(resIdCollection, MODE_X);

    // Recursive global lock.
    locker.lockGlobal(opCtx.get(), MODE_IX);
    ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->recursiveCount, 2U);

    ASSERT_FALSE(locker.unlockGlobal());

    // Unlock them so that they will be pending to unlock.
    ASSERT_FALSE(locker.unlock(resIdCollection));
    ASSERT_FALSE(locker.unlock(resIdDatabase));
    ASSERT_FALSE(locker.unlockGlobal());
    ASSERT_EQ(locker.numResourcesToUnlockAtEndUnitOfWorkForTest(), 3UL);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->unlockPending, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->recursiveCount, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->unlockPending, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->recursiveCount, 1U);

    locker.releaseWriteUnitOfWork(&lockInfo);
    ASSERT_EQ(lockInfo.unlockPendingLocks.size(), 3UL);

    // Things should still be locked.
    ASSERT_EQUALS(MODE_X, locker.getLockMode(resIdCollection));
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdDatabase));
    ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->unlockPending, 0U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->recursiveCount, 1U);
    ASSERT(locker.isLocked());
    ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->unlockPending, 0U);
    ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->recursiveCount, 1U);

    // The locker is no longer participating the two-phase locking.
    ASSERT_FALSE(locker.inAWriteUnitOfWork());
    ASSERT_EQ(locker.numResourcesToUnlockAtEndUnitOfWorkForTest(), 0UL);

    // Start a new WUOW with the same locker. Any new locks acquired in the new WUOW
    // should participate two-phase locking.
    {
        locker.beginWriteUnitOfWork();

        // Grab new locks inside the new WUOW.
        locker.lockGlobal(opCtx.get(), MODE_IX);
        locker.lock(resIdDatabase, MODE_IX);
        locker.lock(resIdCollection2, MODE_IX);

        ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdDatabase));
        ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdCollection2));
        ASSERT(locker.isLocked());

        locker.unlock(resIdCollection2);
        ASSERT_EQ(locker.numResourcesToUnlockAtEndUnitOfWorkForTest(), 1UL);
        ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->unlockPending, 0U);
        ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->recursiveCount, 2U);
        locker.unlock(resIdDatabase);
        ASSERT_EQ(locker.numResourcesToUnlockAtEndUnitOfWorkForTest(), 1UL);
        ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->unlockPending, 0U);
        ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->recursiveCount, 1U);
        locker.unlockGlobal();
        ASSERT_EQ(locker.numResourcesToUnlockAtEndUnitOfWorkForTest(), 1UL);
        ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->unlockPending, 0U);
        ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->recursiveCount, 1U);
        locker.endWriteUnitOfWork();
    }
    ASSERT_FALSE(locker.inAWriteUnitOfWork());
    ASSERT_EQ(locker.numResourcesToUnlockAtEndUnitOfWorkForTest(), 0UL);

    ASSERT_EQUALS(MODE_X, locker.getLockMode(resIdCollection));
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdDatabase));
    ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->unlockPending, 0U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->recursiveCount, 1U);
    ASSERT(locker.isLocked());
    ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->unlockPending, 0U);
    ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->recursiveCount, 1U);
    // The new locks has been released.
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection2));

    // Restore lock state.
    locker.restoreWriteUnitOfWork(lockInfo);

    ASSERT_TRUE(locker.inAWriteUnitOfWork());

    // Make sure things are still locked.
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_X, locker.getLockMode(resIdCollection));
    ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->unlockPending, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->recursiveCount, 1U);
    ASSERT(locker.isLocked());
    ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->unlockPending, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->recursiveCount, 1U);

    locker.endWriteUnitOfWork();

    ASSERT_FALSE(locker.inAWriteUnitOfWork());

    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection2));
    ASSERT_FALSE(locker.isLocked());
    ASSERT_EQ(locker.numResourcesToUnlockAtEndUnitOfWorkForTest(), 0U);
    ASSERT(locker.getRequestsForTest().find(resourceIdGlobal).finished());
}

TEST_F(LockerImplTest, releaseAndRestoreReadOnlyWriteUnitOfWork) {
    auto opCtx = makeOperationContext();

    Locker::LockSnapshot lockInfo;

    LockerImpl locker(opCtx->getServiceContext());

    const ResourceId resIdDatabase(RESOURCE_DATABASE, DatabaseName(boost::none, "TestDB"));
    const ResourceId resIdCollection(RESOURCE_COLLECTION,
                                     NamespaceString(boost::none, "TestDB.collection"));

    // Snapshot transactions delay shared locks as well.
    locker.setSharedLocksShouldTwoPhaseLock(true);

    locker.beginWriteUnitOfWork();
    // Lock some stuff in IS mode.
    locker.lockGlobal(opCtx.get(), MODE_IS);
    locker.lock(resIdDatabase, MODE_IS);
    locker.lock(resIdCollection, MODE_IS);
    // Unlock them.
    ASSERT_FALSE(locker.unlock(resIdCollection));
    ASSERT_FALSE(locker.unlock(resIdDatabase));
    ASSERT_FALSE(locker.unlockGlobal());
    ASSERT_EQ(3u, locker.numResourcesToUnlockAtEndUnitOfWorkForTest());

    // Things shouldn't be locked anymore.
    ASSERT_TRUE(locker.releaseWriteUnitOfWorkAndUnlock(&lockInfo));

    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());

    // Restore lock state.
    locker.restoreWriteUnitOfWorkAndLock(nullptr, lockInfo);

    ASSERT_EQUALS(MODE_IS, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_IS, locker.getLockMode(resIdCollection));
    ASSERT_TRUE(locker.isLocked());

    locker.endWriteUnitOfWork();

    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());
}

TEST_F(LockerImplTest, releaseAndRestoreEmptyWriteUnitOfWork) {
    Locker::LockSnapshot lockInfo;
    auto opCtx = makeOperationContext();
    LockerImpl locker(opCtx->getServiceContext());

    // Snapshot transactions delay shared locks as well.
    locker.setSharedLocksShouldTwoPhaseLock(true);

    locker.beginWriteUnitOfWork();

    // Nothing to yield.
    ASSERT_FALSE(locker.releaseWriteUnitOfWorkAndUnlock(&lockInfo));
    ASSERT_FALSE(locker.isLocked());

    // Restore lock state.
    locker.restoreWriteUnitOfWorkAndLock(nullptr, lockInfo);
    ASSERT_FALSE(locker.isLocked());

    locker.endWriteUnitOfWork();
    ASSERT_FALSE(locker.isLocked());
}

TEST_F(LockerImplTest, releaseAndRestoreWriteUnitOfWorkWithRecursiveLocks) {
    auto opCtx = makeOperationContext();

    Locker::LockSnapshot lockInfo;

    LockerImpl locker(opCtx->getServiceContext());

    const ResourceId resIdDatabase(RESOURCE_DATABASE, DatabaseName(boost::none, "TestDB"));
    const ResourceId resIdCollection(RESOURCE_COLLECTION,
                                     NamespaceString(boost::none, "TestDB.collection"));

    locker.beginWriteUnitOfWork();
    // Lock some stuff.
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(resIdDatabase, MODE_IX);
    locker.lock(resIdCollection, MODE_IX);
    // Recursively lock them again with a weaker mode.
    locker.lockGlobal(opCtx.get(), MODE_IS);
    locker.lock(resIdDatabase, MODE_IS);
    locker.lock(resIdCollection, MODE_IS);

    // Make sure locks are converted.
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdCollection));
    ASSERT_TRUE(locker.isWriteLocked());
    ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->recursiveCount, 2U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->recursiveCount, 2U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdCollection).objAddr()->recursiveCount, 2U);

    // Unlock them so that they will be pending to unlock.
    ASSERT_FALSE(locker.unlock(resIdCollection));
    ASSERT_FALSE(locker.unlock(resIdDatabase));
    ASSERT_FALSE(locker.unlockGlobal());
    // Make sure locks are still acquired in the correct mode.
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdCollection));
    ASSERT_TRUE(locker.isWriteLocked());
    // Make sure unlocking converted locks decrements the locks' recursiveCount instead of
    // incrementing unlockPending.
    ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->recursiveCount, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->unlockPending, 0U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->recursiveCount, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->unlockPending, 0U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdCollection).objAddr()->recursiveCount, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdCollection).objAddr()->unlockPending, 0U);

    // Unlock again so unlockPending == recursiveCount.
    ASSERT_FALSE(locker.unlock(resIdCollection));
    ASSERT_FALSE(locker.unlock(resIdDatabase));
    ASSERT_FALSE(locker.unlockGlobal());
    ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->recursiveCount, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->unlockPending, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->recursiveCount, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->unlockPending, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdCollection).objAddr()->recursiveCount, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdCollection).objAddr()->unlockPending, 1U);

    ASSERT(locker.releaseWriteUnitOfWorkAndUnlock(&lockInfo));

    // Things shouldn't be locked anymore.
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());

    // Restore lock state.
    locker.restoreWriteUnitOfWorkAndLock(nullptr, lockInfo);

    // Make sure things were re-locked in the correct mode.
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdCollection));
    ASSERT_TRUE(locker.isWriteLocked());
    // Make sure locks were coalesced after restore and are pending to unlock as before.
    ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->recursiveCount, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resourceIdGlobal).objAddr()->unlockPending, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->recursiveCount, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdDatabase).objAddr()->unlockPending, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdCollection).objAddr()->recursiveCount, 1U);
    ASSERT_EQ(locker.getRequestsForTest().find(resIdCollection).objAddr()->unlockPending, 1U);

    locker.endWriteUnitOfWork();

    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());
}

TEST_F(LockerImplTest, DefaultLocker) {
    auto opCtx = makeOperationContext();

    const ResourceId resId(RESOURCE_DATABASE, DatabaseName(boost::none, "TestDB"));

    LockerImpl locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(resId, MODE_X);

    // Make sure only Global and TestDB resources are locked.
    Locker::LockerInfo info;
    locker.getLockerInfo(&info, boost::none);
    ASSERT(!info.waitingResource.isValid());
    ASSERT_EQUALS(2U, info.locks.size());
    ASSERT_EQUALS(RESOURCE_GLOBAL, info.locks[0].resourceId.getType());
    ASSERT_EQUALS(resId, info.locks[1].resourceId);

    ASSERT(locker.unlockGlobal());
}

TEST_F(LockerImplTest, SharedLocksShouldTwoPhaseLockIsTrue) {
    // Test that when setSharedLocksShouldTwoPhaseLock is true and we are in a WUOW, unlock on IS
    // and S locks are postponed until endWriteUnitOfWork() is called. Mode IX and X locks always
    // participate in two-phased locking, regardless of the setting.

    auto opCtx = makeOperationContext();

    const ResourceId resId1(RESOURCE_DATABASE, DatabaseName(boost::none, "TestDB1"));
    const ResourceId resId2(RESOURCE_DATABASE, DatabaseName(boost::none, "TestDB2"));
    const ResourceId resId3(RESOURCE_COLLECTION,
                            NamespaceString(boost::none, "TestDB.collection3"));
    const ResourceId resId4(RESOURCE_COLLECTION,
                            NamespaceString(boost::none, "TestDB.collection4"));

    LockerImpl locker(opCtx->getServiceContext());
    locker.setSharedLocksShouldTwoPhaseLock(true);

    locker.lockGlobal(opCtx.get(), MODE_IS);
    ASSERT_EQ(locker.getLockMode(resourceIdGlobal), MODE_IS);

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
    ASSERT_EQ(locker.getLockMode(resourceIdGlobal), MODE_IS);

    locker.endWriteUnitOfWork();

    ASSERT_EQ(locker.getLockMode(resId1), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(resId2), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(resId3), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(resId4), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(resourceIdGlobal), MODE_NONE);
}

TEST_F(LockerImplTest, ModeIXAndXLockParticipatesInTwoPhaseLocking) {
    // Unlock on mode IX and X locks during a WUOW should always be postponed until
    // endWriteUnitOfWork() is called. Mode IS and S locks should unlock immediately.

    auto opCtx = makeOperationContext();

    const ResourceId resId1(RESOURCE_DATABASE, DatabaseName(boost::none, "TestDB1"));
    const ResourceId resId2(RESOURCE_DATABASE, DatabaseName(boost::none, "TestDB2"));
    const ResourceId resId3(RESOURCE_COLLECTION,
                            NamespaceString(boost::none, "TestDB.collection3"));
    const ResourceId resId4(RESOURCE_COLLECTION,
                            NamespaceString(boost::none, "TestDB.collection4"));

    LockerImpl locker(opCtx->getServiceContext());

    locker.lockGlobal(opCtx.get(), MODE_IX);
    ASSERT_EQ(locker.getLockMode(resourceIdGlobal), MODE_IX);

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
    ASSERT_EQ(locker.getLockMode(resourceIdGlobal), MODE_IX);

    locker.endWriteUnitOfWork();

    ASSERT_EQ(locker.getLockMode(resId2), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(resId4), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);
    ASSERT_EQ(locker.getLockMode(resourceIdGlobal), MODE_NONE);
}

TEST_F(LockerImplTest, RSTLUnlocksWithNestedLock) {
    auto opCtx = makeOperationContext();
    LockerImpl locker(opCtx->getServiceContext());

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

TEST_F(LockerImplTest, RSTLModeIXWithTwoPhaseLockingCanBeUnlockedWhenPrepared) {
    auto opCtx = makeOperationContext();
    LockerImpl locker(opCtx->getServiceContext());

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

TEST_F(LockerImplTest, RSTLModeISWithTwoPhaseLockingCanBeUnlockedWhenPrepared) {
    auto opCtx = makeOperationContext();
    LockerImpl locker(opCtx->getServiceContext());

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

TEST_F(LockerImplTest, RSTLTwoPhaseLockingBehaviorModeIS) {
    auto opCtx = makeOperationContext();
    LockerImpl locker(opCtx->getServiceContext());

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

TEST_F(LockerImplTest, OverrideLockRequestTimeout) {
    auto opCtx = makeOperationContext();

    const ResourceId resIdFirstDB(RESOURCE_DATABASE, DatabaseName(boost::none, "FirstDB"));
    const ResourceId resIdSecondDB(RESOURCE_DATABASE, DatabaseName(boost::none, "SecondDB"));

    LockerImpl locker1(opCtx->getServiceContext());
    LockerImpl locker2(opCtx->getServiceContext());

    // Set up locker2 to override lock requests' provided timeout if greater than 1000 milliseconds.
    locker2.setMaxLockTimeout(Milliseconds(1000));

    locker1.lockGlobal(opCtx.get(), MODE_IX);
    locker2.lockGlobal(opCtx.get(), MODE_IX);

    // locker1 acquires FirstDB under an exclusive lock.
    locker1.lock(resIdFirstDB, MODE_X);
    ASSERT_TRUE(locker1.isLockHeldForMode(resIdFirstDB, MODE_X));

    // locker2's attempt to acquire FirstDB with unlimited wait time should timeout after 1000
    // milliseconds and throw because _maxLockRequestTimeout is set to 1000 milliseconds.
    ASSERT_THROWS_CODE(locker2.lock(opCtx.get(), resIdFirstDB, MODE_X, Date_t::max()),
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

TEST_F(LockerImplTest, DoNotWaitForLockAcquisition) {
    auto opCtx = makeOperationContext();

    const ResourceId resIdFirstDB(RESOURCE_DATABASE, DatabaseName(boost::none, "FirstDB"));
    const ResourceId resIdSecondDB(RESOURCE_DATABASE, DatabaseName(boost::none, "SecondDB"));

    LockerImpl locker1(opCtx->getServiceContext());
    LockerImpl locker2(opCtx->getServiceContext());

    // Set up locker2 to immediately return if a lock is unavailable, regardless of supplied
    // deadlines in the lock request.
    locker2.setMaxLockTimeout(Milliseconds(0));

    locker1.lockGlobal(opCtx.get(), MODE_IX);
    locker2.lockGlobal(opCtx.get(), MODE_IX);

    // locker1 acquires FirstDB under an exclusive lock.
    locker1.lock(resIdFirstDB, MODE_X);
    ASSERT_TRUE(locker1.isLockHeldForMode(resIdFirstDB, MODE_X));

    // locker2's attempt to acquire FirstDB with unlimited wait time should fail immediately and
    // throw because _maxLockRequestTimeout was set to 0.
    ASSERT_THROWS_CODE(locker2.lock(opCtx.get(), resIdFirstDB, MODE_X, Date_t::max()),
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
    return (1U ==
            std::count_if(lockerInfo.locks.begin(),
                          lockerInfo.locks.end(),
                          [&resourceId, &mode](const Locker::OneLock& lock) {
                              return lock.resourceId == resourceId && lock.mode == mode;
                          }));
}
}  // namespace

TEST_F(LockerImplTest, GetLockerInfoShouldReportHeldLocks) {
    auto opCtx = makeOperationContext();

    const ResourceId dbId(RESOURCE_DATABASE, DatabaseName(boost::none, "TestDB"));
    const ResourceId collectionId(RESOURCE_COLLECTION,
                                  NamespaceString(boost::none, "TestDB.collection"));

    // Take an exclusive lock on the collection.
    LockerImpl locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(dbId, MODE_IX);
    locker.lock(collectionId, MODE_X);

    // Assert it shows up in the output of getLockerInfo().
    Locker::LockerInfo lockerInfo;
    locker.getLockerInfo(&lockerInfo, boost::none);

    ASSERT(lockerInfoContainsLock(lockerInfo, resourceIdGlobal, MODE_IX));
    ASSERT(lockerInfoContainsLock(lockerInfo, dbId, MODE_IX));
    ASSERT(lockerInfoContainsLock(lockerInfo, collectionId, MODE_X));
    ASSERT_EQ(3U, lockerInfo.locks.size());

    ASSERT(locker.unlock(collectionId));
    ASSERT(locker.unlock(dbId));
    ASSERT(locker.unlockGlobal());
}

TEST_F(LockerImplTest, GetLockerInfoShouldReportPendingLocks) {
    auto opCtx = makeOperationContext();

    const ResourceId dbId(RESOURCE_DATABASE, DatabaseName(boost::none, "TestDB"));
    const ResourceId collectionId(RESOURCE_COLLECTION,
                                  NamespaceString(boost::none, "TestDB.collection"));

    // Take an exclusive lock on the collection.
    LockerImpl successfulLocker(opCtx->getServiceContext());
    successfulLocker.lockGlobal(opCtx.get(), MODE_IX);
    successfulLocker.lock(dbId, MODE_IX);
    successfulLocker.lock(collectionId, MODE_X);

    // Now attempt to get conflicting locks.
    LockerImpl conflictingLocker(opCtx->getServiceContext());
    conflictingLocker.lockGlobal(opCtx.get(), MODE_IS);
    conflictingLocker.lock(dbId, MODE_IS);
    ASSERT_EQ(LOCK_WAITING,
              conflictingLocker.lockBeginForTest(nullptr /* opCtx */, collectionId, MODE_IS));

    // Assert the held locks show up in the output of getLockerInfo().
    Locker::LockerInfo lockerInfo;
    conflictingLocker.getLockerInfo(&lockerInfo, boost::none);
    ASSERT(lockerInfoContainsLock(lockerInfo, resourceIdGlobal, MODE_IS));
    ASSERT(lockerInfoContainsLock(lockerInfo, dbId, MODE_IS));
    ASSERT(lockerInfoContainsLock(lockerInfo, collectionId, MODE_IS));
    ASSERT_EQ(3U, lockerInfo.locks.size());

    // Assert it reports that it is waiting for the collection lock.
    ASSERT_EQ(collectionId, lockerInfo.waitingResource);

    // Make sure it no longer reports waiting once unlocked.
    ASSERT(successfulLocker.unlock(collectionId));
    ASSERT(successfulLocker.unlock(dbId));
    ASSERT(successfulLocker.unlockGlobal());

    conflictingLocker.lockCompleteForTest(
        nullptr /* opCtx */, collectionId, MODE_IS, Date_t::now());

    conflictingLocker.getLockerInfo(&lockerInfo, boost::none);
    ASSERT_FALSE(lockerInfo.waitingResource.isValid());

    ASSERT(conflictingLocker.unlock(collectionId));
    ASSERT(conflictingLocker.unlock(dbId));
    ASSERT(conflictingLocker.unlockGlobal());
}

TEST_F(LockerImplTest, ReaquireLockPendingUnlock) {
    auto opCtx = makeOperationContext();

    const ResourceId resId(RESOURCE_COLLECTION, NamespaceString(boost::none, "TestDB.collection"));

    LockerImpl locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IS);

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

TEST_F(LockerImplTest, AcquireLockPendingUnlockWithCoveredMode) {
    auto opCtx = makeOperationContext();

    const ResourceId resId(RESOURCE_COLLECTION, NamespaceString(boost::none, "TestDB.collection"));

    LockerImpl locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IS);

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

TEST_F(LockerImplTest, ConvertLockPendingUnlock) {
    auto opCtx = makeOperationContext();

    const ResourceId resId(RESOURCE_COLLECTION, NamespaceString(boost::none, "TestDB.collection"));

    LockerImpl locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IS);

    locker.lock(resId, MODE_IX);
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_IX));

    locker.beginWriteUnitOfWork();

    ASSERT_FALSE(locker.unlock(resId));
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_IX));
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->recursiveCount == 1);

    // Convert lock pending unlock.
    locker.lock(resId, MODE_X);
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->recursiveCount == 2);

    locker.endWriteUnitOfWork();

    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 0);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 0);
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_X));

    locker.unlockGlobal();
}

TEST_F(LockerImplTest, ConvertLockPendingUnlockAndUnlock) {
    auto opCtx = makeOperationContext();

    const ResourceId resId(RESOURCE_COLLECTION, NamespaceString(boost::none, "TestDB.collection"));

    LockerImpl locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IS);

    locker.lock(resId, MODE_IX);
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_IX));

    locker.beginWriteUnitOfWork();

    ASSERT_FALSE(locker.unlock(resId));
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_IX));
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->recursiveCount == 1);

    // Convert lock pending unlock.
    locker.lock(resId, MODE_X);
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->recursiveCount == 2);

    // Unlock the lock conversion.
    ASSERT_FALSE(locker.unlock(resId));
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 1);
    // Make sure we still hold X lock and unlock the weaker mode to decrement recursiveCount instead
    // of incrementing unlockPending.
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_X));
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->recursiveCount == 1);

    locker.endWriteUnitOfWork();

    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 0);
    ASSERT(locker.getRequestsForTest().find(resId).finished());
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_NONE));

    locker.unlockGlobal();
}

TEST_F(LockerImplTest, SetTicketAcquisitionForLockRAIIType) {
    auto opCtx = makeOperationContext();

    // By default, ticket acquisition is required.
    ASSERT_TRUE(opCtx->lockState()->shouldAcquireTicket());

    {
        SetTicketAquisitionPriorityForLock setTicketAquisition(
            opCtx.get(), AdmissionContext::Priority::kImmediate);
        ASSERT_FALSE(opCtx->lockState()->shouldAcquireTicket());
    }

    ASSERT_TRUE(opCtx->lockState()->shouldAcquireTicket());

    // If ticket acquisitions are disabled on the lock state, the RAII type has no effect.
    opCtx->lockState()->setAdmissionPriority(AdmissionContext::Priority::kImmediate);
    ASSERT_FALSE(opCtx->lockState()->shouldAcquireTicket());

    {
        SetTicketAquisitionPriorityForLock setTicketAquisition(
            opCtx.get(), AdmissionContext::Priority::kImmediate);
        ASSERT_FALSE(opCtx->lockState()->shouldAcquireTicket());
    }

    ASSERT_FALSE(opCtx->lockState()->shouldAcquireTicket());
}

// This test exercises the lock dumping code in ~LockerImpl in case locks are held on destruction.
DEATH_TEST_F(LockerImplTest,
             LocksHeldOnDestructionCausesALocksDump,
             "Operation ending while holding locks.") {
    auto opCtx = makeOperationContext();

    const ResourceId resId(RESOURCE_COLLECTION, NamespaceString(boost::none, "TestDB.collection"));

    LockerImpl locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(resId, MODE_X);

    ASSERT(locker.isLockHeldForMode(resId, MODE_X));
    ASSERT(locker.isLockHeldForMode(resId, MODE_S));

    // 'locker' destructor should invariant because locks are still held.
}

}  // namespace mongo
