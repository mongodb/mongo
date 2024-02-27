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


#include <algorithm>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/client.h"
#include "mongo/db/concurrency/fast_map_noalloc.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/lock_stats.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/tenant_id.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

class LockerTest : public ServiceContextTest {};

TEST_F(LockerTest, LockNoConflict) {
    auto opCtx = makeOperationContext();

    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    Locker locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IX);

    locker.lock(opCtx.get(), resId, MODE_X);

    ASSERT(locker.isLockHeldForMode(resId, MODE_X));
    ASSERT(locker.isLockHeldForMode(resId, MODE_S));

    ASSERT(locker.unlock(resId));

    ASSERT(locker.isLockHeldForMode(resId, MODE_NONE));

    locker.unlockGlobal();
}

TEST_F(LockerTest, ReLockNoConflict) {
    auto opCtx = makeOperationContext();

    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    Locker locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IX);

    locker.lock(opCtx.get(), resId, MODE_X);
    locker.lock(opCtx.get(), resId, MODE_S);

    ASSERT(!locker.unlock(resId));
    ASSERT(locker.isLockHeldForMode(resId, MODE_X));

    ASSERT(locker.unlock(resId));
    ASSERT(locker.isLockHeldForMode(resId, MODE_NONE));

    ASSERT(locker.unlockGlobal());
}

TEST_F(LockerTest, ConflictWithTimeout) {
    auto opCtx = makeOperationContext();

    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    Locker locker1(opCtx->getServiceContext());
    locker1.lockGlobal(opCtx.get(), MODE_IX);
    locker1.lock(opCtx.get(), resId, MODE_X);

    Locker locker2(opCtx->getServiceContext());
    locker2.lockGlobal(opCtx.get(), MODE_IX);

    ASSERT_THROWS_CODE(locker2.lock(opCtx.get(), resId, MODE_S, Date_t::now()),
                       AssertionException,
                       ErrorCodes::LockTimeout);

    ASSERT(locker2.getLockMode(resId) == MODE_NONE);

    ASSERT(locker1.unlock(resId));

    ASSERT(locker1.unlockGlobal());
    ASSERT(locker2.unlockGlobal());
}

TEST_F(LockerTest, FailPointInLockFailsGlobalNonIntentLocksIfTheyCannotBeImmediatelyGranted) {
    transport::TransportLayerMock transportLayer;
    std::shared_ptr<transport::Session> session = transportLayer.createSession();

    // The session is need so that the operation is considered to be a user operation, which is the
    // requirement for the failNonIntentLocksIfWaitNeeded failpoint to kick-in
    auto newClient = getServiceContext()->getService()->makeClient("userClient", session);
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();

    Locker locker1(newOpCtx->getServiceContext());
    locker1.lockGlobal(newOpCtx.get(), MODE_IX);

    {
        FailPointEnableBlock failWaitingNonPartitionedLocks("failNonIntentLocksIfWaitNeeded");

        // MODE_S attempt.
        Locker locker2(newOpCtx->getServiceContext());
        ASSERT_THROWS_CODE(
            locker2.lockGlobal(newOpCtx.get(), MODE_S), DBException, ErrorCodes::LockTimeout);

        // MODE_X attempt.
        Locker locker3(newOpCtx->getServiceContext());
        ASSERT_THROWS_CODE(
            locker3.lockGlobal(newOpCtx.get(), MODE_X), DBException, ErrorCodes::LockTimeout);
    }

    locker1.unlockGlobal();
}

TEST_F(LockerTest, FailPointInLockFailsNonIntentLocksIfTheyCannotBeImmediatelyGranted) {
    transport::TransportLayerMock transportLayer;
    std::shared_ptr<transport::Session> session = transportLayer.createSession();

    // The session is need so that the operation is considered to be a user operation, which is the
    // requirement for the failNonIntentLocksIfWaitNeeded failpoint to kick-in
    auto newClient = getServiceContext()->getService()->makeClient("userClient", session);
    AlternativeClientRegion acr(newClient);
    auto newOpCtx = cc().makeOperationContext();

    // Granted MODE_X lock, fail incoming MODE_S and MODE_X.
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    Locker locker1(newOpCtx->getServiceContext());
    locker1.lockGlobal(newOpCtx.get(), MODE_IX);
    locker1.lock(newOpCtx.get(), resId, MODE_X);

    {
        FailPointEnableBlock failWaitingNonPartitionedLocks("failNonIntentLocksIfWaitNeeded");

        // MODE_S attempt.
        Locker locker2(newOpCtx->getServiceContext());
        locker2.lockGlobal(newOpCtx.get(), MODE_IS);
        ASSERT_THROWS_CODE(locker2.lock(newOpCtx.get(), resId, MODE_S, Date_t::max()),
                           DBException,
                           ErrorCodes::LockTimeout);

        // The timed out MODE_S attempt shouldn't be present in the map of lock requests because it
        // won't ever be granted.
        ASSERT(locker2.getRequestsForTest().find(resId).finished());
        locker2.unlockGlobal();

        // MODE_X attempt.
        Locker locker3(newOpCtx->getServiceContext());
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

TEST_F(LockerTest, ReadTransaction) {
    auto opCtx = makeOperationContext();

    Locker locker(opCtx->getServiceContext());

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
 * Test that saveLocker works by examining the output.
 */
TEST_F(LockerTest, saveAndRestoreGlobal) {
    auto opCtx = makeOperationContext();

    Locker locker(opCtx->getServiceContext());

    // No lock requests made, no locks held.
    ASSERT_FALSE(locker.canSaveLockState());

    // Lock the global lock, but just once.
    locker.lockGlobal(opCtx.get(), MODE_IX);

    // We've locked the global lock.  This should be reflected in the lockInfo.
    Locker::LockSnapshot lockInfo;
    locker.saveLockStateAndUnlock(&lockInfo);
    ASSERT(!locker.isLocked());
    ASSERT_EQUALS(MODE_IX, lockInfo.globalMode);

    // Restore the lock(s) we had.
    locker.restoreLockState(opCtx.get(), lockInfo);

    ASSERT(locker.isLocked());
    ASSERT(locker.unlockGlobal());
}

/**
 * Test that saveLocker can save and restore the RSTL.
 */
TEST_F(LockerTest, saveAndRestoreRSTL) {
    auto opCtx = makeOperationContext();

    Locker locker(opCtx->getServiceContext());

    const ResourceId resIdDatabase(RESOURCE_DATABASE,
                                   DatabaseName::createDatabaseName_forTest(boost::none, "TestDB"));

    // Acquire locks.
    locker.lock(opCtx.get(), resourceIdReplicationStateTransitionLock, MODE_IX);
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(opCtx.get(), resIdDatabase, MODE_IX);

    // Save the lock state.
    Locker::LockSnapshot lockInfo;
    locker.saveLockStateAndUnlock(&lockInfo);
    ASSERT(!locker.isLocked());
    ASSERT_EQUALS(MODE_IX, lockInfo.globalMode);

    // Check locks are unlocked.
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resourceIdReplicationStateTransitionLock));
    ASSERT(!locker.isLocked());
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));

    // Restore the lock(s) we had.
    locker.restoreLockState(opCtx.get(), lockInfo);

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
TEST_F(LockerTest, saveAndRestoreGlobalAcquiredTwice) {
    auto opCtx = makeOperationContext();

    Locker locker(opCtx->getServiceContext());

    // No lock requests made, no locks held.
    ASSERT_FALSE(locker.canSaveLockState());

    // Lock the global lock.
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lockGlobal(opCtx.get(), MODE_IX);

    // This shouldn't actually unlock as we're in a nested scope.
    ASSERT_FALSE(locker.canSaveLockState());

    ASSERT(locker.isLocked());

    // We must unlockGlobal twice.
    ASSERT(!locker.unlockGlobal());
    ASSERT(locker.unlockGlobal());
}

/**
 * Tests that restoreLocker works by locking a db and collection and saving + restoring.
 */
TEST_F(LockerTest, saveAndRestoreDBAndCollection) {
    auto opCtx = makeOperationContext();

    Locker::LockSnapshot lockInfo;

    Locker locker(opCtx->getServiceContext());

    const ResourceId resIdDatabase(RESOURCE_DATABASE,
                                   DatabaseName::createDatabaseName_forTest(boost::none, "TestDB"));
    const ResourceId resIdCollection(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    // Lock some stuff.
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(opCtx.get(), resIdDatabase, MODE_IX);
    locker.lock(opCtx.get(), resIdCollection, MODE_IX);
    locker.saveLockStateAndUnlock(&lockInfo);

    // Things shouldn't be locked anymore.
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));

    // Restore lock state.
    locker.restoreLockState(opCtx.get(), lockInfo);

    // Make sure things were re-locked.
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdCollection));

    ASSERT(locker.unlockGlobal());
}

TEST_F(LockerTest, releaseWriteUnitOfWork) {
    auto opCtx = makeOperationContext();

    Locker::LockSnapshot lockInfo;

    Locker locker(opCtx->getServiceContext());

    const ResourceId resIdDatabase(RESOURCE_DATABASE,
                                   DatabaseName::createDatabaseName_forTest(boost::none, "TestDB"));
    const ResourceId resIdCollection(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    locker.beginWriteUnitOfWork();
    // Lock some stuff.
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(opCtx.get(), resIdDatabase, MODE_IX);
    locker.lock(opCtx.get(), resIdCollection, MODE_IX);
    // Unlock them so that they will be pending to unlock.
    ASSERT_FALSE(locker.unlock(resIdCollection));
    ASSERT_FALSE(locker.unlock(resIdDatabase));
    ASSERT_FALSE(locker.unlockGlobal());

    locker.releaseWriteUnitOfWorkAndUnlock(&lockInfo);

    // Things shouldn't be locked anymore.
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());

    // Destructor should succeed since the locker's state should be empty.
}

TEST_F(LockerTest, restoreWriteUnitOfWork) {
    auto opCtx = makeOperationContext();

    Locker::LockSnapshot lockInfo;

    Locker locker(opCtx->getServiceContext());

    const ResourceId resIdDatabase(RESOURCE_DATABASE,
                                   DatabaseName::createDatabaseName_forTest(boost::none, "TestDB"));
    const ResourceId resIdCollection(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    locker.beginWriteUnitOfWork();
    // Lock some stuff.
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(opCtx.get(), resIdDatabase, MODE_IX);
    locker.lock(opCtx.get(), resIdCollection, MODE_IX);
    // Unlock them so that they will be pending to unlock.
    ASSERT_FALSE(locker.unlock(resIdCollection));
    ASSERT_FALSE(locker.unlock(resIdDatabase));
    ASSERT_FALSE(locker.unlockGlobal());

    locker.releaseWriteUnitOfWorkAndUnlock(&lockInfo);

    // Things shouldn't be locked anymore.
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());

    // Restore lock state.
    locker.restoreWriteUnitOfWorkAndLock(opCtx.get(), lockInfo);

    // Make sure things were re-locked.
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_IX, locker.getLockMode(resIdCollection));
    ASSERT(locker.isLocked());

    locker.endWriteUnitOfWork();

    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());
}

TEST_F(LockerTest, releaseAndRestoreWriteUnitOfWorkWithoutUnlock) {
    auto opCtx = makeOperationContext();

    Locker::WUOWLockSnapshot lockInfo;

    Locker locker(opCtx->getServiceContext());

    const ResourceId resIdDatabase(RESOURCE_DATABASE,
                                   DatabaseName::createDatabaseName_forTest(boost::none, "TestDB"));
    const ResourceId resIdCollection(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));
    const ResourceId resIdCollection2(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection2"));

    locker.beginWriteUnitOfWork();
    // Lock some stuff.
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(opCtx.get(), resIdDatabase, MODE_IX);
    locker.lock(opCtx.get(), resIdCollection, MODE_X);

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
        locker.lock(opCtx.get(), resIdDatabase, MODE_IX);
        locker.lock(opCtx.get(), resIdCollection2, MODE_IX);

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

TEST_F(LockerTest, releaseAndRestoreReadOnlyWriteUnitOfWork) {
    auto opCtx = makeOperationContext();

    Locker::LockSnapshot lockInfo;

    Locker locker(opCtx->getServiceContext());

    const ResourceId resIdDatabase(RESOURCE_DATABASE,
                                   DatabaseName::createDatabaseName_forTest(boost::none, "TestDB"));
    const ResourceId resIdCollection(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    // Snapshot transactions delay shared locks as well.
    locker.setSharedLocksShouldTwoPhaseLock(true);

    locker.beginWriteUnitOfWork();
    // Lock some stuff in IS mode.
    locker.lockGlobal(opCtx.get(), MODE_IS);
    locker.lock(opCtx.get(), resIdDatabase, MODE_IS);
    locker.lock(opCtx.get(), resIdCollection, MODE_IS);
    // Unlock them.
    ASSERT_FALSE(locker.unlock(resIdCollection));
    ASSERT_FALSE(locker.unlock(resIdDatabase));
    ASSERT_FALSE(locker.unlockGlobal());
    ASSERT_EQ(3u, locker.numResourcesToUnlockAtEndUnitOfWorkForTest());

    // Things shouldn't be locked anymore.
    locker.releaseWriteUnitOfWorkAndUnlock(&lockInfo);

    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());

    // Restore lock state.
    locker.restoreWriteUnitOfWorkAndLock(opCtx.get(), lockInfo);

    ASSERT_EQUALS(MODE_IS, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_IS, locker.getLockMode(resIdCollection));
    ASSERT_TRUE(locker.isLocked());

    locker.endWriteUnitOfWork();

    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());
}

TEST_F(LockerTest, releaseAndRestoreEmptyWriteUnitOfWork) {
    Locker::LockSnapshot lockInfo;
    auto opCtx = makeOperationContext();
    Locker locker(opCtx->getServiceContext());

    // Snapshot transactions delay shared locks as well.
    locker.setSharedLocksShouldTwoPhaseLock(true);

    locker.beginWriteUnitOfWork();

    // Nothing to yield.
    ASSERT_FALSE(locker.canSaveLockState());
    ASSERT_FALSE(locker.isLocked());

    locker.endWriteUnitOfWork();
}

TEST_F(LockerTest, releaseAndRestoreWriteUnitOfWorkWithRecursiveLocks) {
    auto opCtx = makeOperationContext();

    Locker::LockSnapshot lockInfo;

    Locker locker(opCtx->getServiceContext());

    const ResourceId resIdDatabase(RESOURCE_DATABASE,
                                   DatabaseName::createDatabaseName_forTest(boost::none, "TestDB"));
    const ResourceId resIdCollection(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    locker.beginWriteUnitOfWork();
    // Lock some stuff.
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(opCtx.get(), resIdDatabase, MODE_IX);
    locker.lock(opCtx.get(), resIdCollection, MODE_IX);
    // Recursively lock them again with a weaker mode.
    locker.lockGlobal(opCtx.get(), MODE_IS);
    locker.lock(opCtx.get(), resIdDatabase, MODE_IS);
    locker.lock(opCtx.get(), resIdCollection, MODE_IS);

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

    locker.releaseWriteUnitOfWorkAndUnlock(&lockInfo);

    // Things shouldn't be locked anymore.
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdDatabase));
    ASSERT_EQUALS(MODE_NONE, locker.getLockMode(resIdCollection));
    ASSERT_FALSE(locker.isLocked());

    // Restore lock state.
    locker.restoreWriteUnitOfWorkAndLock(opCtx.get(), lockInfo);

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

TEST_F(LockerTest, DefaultLocker) {
    auto opCtx = makeOperationContext();

    const ResourceId resId(RESOURCE_DATABASE,
                           DatabaseName::createDatabaseName_forTest(boost::none, "TestDB"));

    Locker locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(opCtx.get(), resId, MODE_X);

    // Make sure only Global and TestDB resources are locked.
    Locker::LockerInfo info;
    locker.getLockerInfo(&info, boost::none);
    ASSERT(!info.waitingResource.isValid());
    ASSERT_EQUALS(2U, info.locks.size());
    ASSERT_EQUALS(RESOURCE_GLOBAL, info.locks[0].resourceId.getType());
    ASSERT_EQUALS(resId, info.locks[1].resourceId);

    ASSERT(locker.unlockGlobal());
}

TEST_F(LockerTest, SharedLocksShouldTwoPhaseLockIsTrue) {
    // Test that when setSharedLocksShouldTwoPhaseLock is true and we are in a WUOW, unlock on IS
    // and S locks are postponed until endWriteUnitOfWork() is called. Mode IX and X locks always
    // participate in two-phased locking, regardless of the setting.

    auto opCtx = makeOperationContext();

    const ResourceId resId1(RESOURCE_DATABASE,
                            DatabaseName::createDatabaseName_forTest(boost::none, "TestDB1"));
    const ResourceId resId2(RESOURCE_DATABASE,
                            DatabaseName::createDatabaseName_forTest(boost::none, "TestDB2"));
    const ResourceId resId3(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection3"));
    const ResourceId resId4(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection4"));

    Locker locker(opCtx->getServiceContext());
    locker.setSharedLocksShouldTwoPhaseLock(true);

    locker.lockGlobal(opCtx.get(), MODE_IS);
    ASSERT_EQ(locker.getLockMode(resourceIdGlobal), MODE_IS);

    locker.lock(opCtx.get(), resourceIdReplicationStateTransitionLock, MODE_IS);
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_IS);

    locker.lock(opCtx.get(), resId1, MODE_IS);
    locker.lock(opCtx.get(), resId2, MODE_IX);
    locker.lock(opCtx.get(), resId3, MODE_S);
    locker.lock(opCtx.get(), resId4, MODE_X);
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

TEST_F(LockerTest, ModeIXAndXLockParticipatesInTwoPhaseLocking) {
    // Unlock on mode IX and X locks during a WUOW should always be postponed until
    // endWriteUnitOfWork() is called. Mode IS and S locks should unlock immediately.

    auto opCtx = makeOperationContext();

    const ResourceId resId1(RESOURCE_DATABASE,
                            DatabaseName::createDatabaseName_forTest(boost::none, "TestDB1"));
    const ResourceId resId2(RESOURCE_DATABASE,
                            DatabaseName::createDatabaseName_forTest(boost::none, "TestDB2"));
    const ResourceId resId3(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection3"));
    const ResourceId resId4(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection4"));

    Locker locker(opCtx->getServiceContext());

    locker.lockGlobal(opCtx.get(), MODE_IX);
    ASSERT_EQ(locker.getLockMode(resourceIdGlobal), MODE_IX);

    locker.lock(opCtx.get(), resourceIdReplicationStateTransitionLock, MODE_IX);
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    locker.lock(opCtx.get(), resId1, MODE_IS);
    locker.lock(opCtx.get(), resId2, MODE_IX);
    locker.lock(opCtx.get(), resId3, MODE_S);
    locker.lock(opCtx.get(), resId4, MODE_X);
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

TEST_F(LockerTest, RSTLUnlocksWithNestedLock) {
    auto opCtx = makeOperationContext();
    Locker locker(opCtx->getServiceContext());

    locker.lock(opCtx.get(), resourceIdReplicationStateTransitionLock, MODE_IX);
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    locker.beginWriteUnitOfWork();

    // Do a nested lock acquisition.
    locker.lock(opCtx.get(), resourceIdReplicationStateTransitionLock, MODE_IX);
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_IX);

    ASSERT(locker.unlockRSTLforPrepare());
    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);

    ASSERT_FALSE(locker.unlockRSTLforPrepare());

    locker.endWriteUnitOfWork();

    ASSERT_EQ(locker.getLockMode(resourceIdReplicationStateTransitionLock), MODE_NONE);

    ASSERT_FALSE(locker.unlockRSTLforPrepare());
    ASSERT_FALSE(locker.unlock(resourceIdReplicationStateTransitionLock));
}

TEST_F(LockerTest, RSTLModeIXWithTwoPhaseLockingCanBeUnlockedWhenPrepared) {
    auto opCtx = makeOperationContext();
    Locker locker(opCtx->getServiceContext());

    locker.lock(opCtx.get(), resourceIdReplicationStateTransitionLock, MODE_IX);
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

TEST_F(LockerTest, RSTLModeISWithTwoPhaseLockingCanBeUnlockedWhenPrepared) {
    auto opCtx = makeOperationContext();
    Locker locker(opCtx->getServiceContext());

    locker.lock(opCtx.get(), resourceIdReplicationStateTransitionLock, MODE_IS);
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

TEST_F(LockerTest, RSTLTwoPhaseLockingBehaviorModeIS) {
    auto opCtx = makeOperationContext();
    Locker locker(opCtx->getServiceContext());

    locker.lock(opCtx.get(), resourceIdReplicationStateTransitionLock, MODE_IS);
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

TEST_F(LockerTest, OverrideLockRequestTimeout) {
    auto opCtx = makeOperationContext();

    const ResourceId resIdFirstDB(RESOURCE_DATABASE,
                                  DatabaseName::createDatabaseName_forTest(boost::none, "FirstDB"));
    const ResourceId resIdSecondDB(
        RESOURCE_DATABASE, DatabaseName::createDatabaseName_forTest(boost::none, "SecondDB"));

    Locker locker1(opCtx->getServiceContext());
    Locker locker2(opCtx->getServiceContext());

    // Set up locker2 to override lock requests' provided timeout if greater than 1000 milliseconds.
    locker2.setMaxLockTimeout(Milliseconds(1000));

    locker1.lockGlobal(opCtx.get(), MODE_IX);
    locker2.lockGlobal(opCtx.get(), MODE_IX);

    // locker1 acquires FirstDB under an exclusive lock.
    locker1.lock(opCtx.get(), resIdFirstDB, MODE_X);
    ASSERT_TRUE(locker1.isLockHeldForMode(resIdFirstDB, MODE_X));

    // locker2's attempt to acquire FirstDB with unlimited wait time should timeout after 1000
    // milliseconds and throw because _maxLockRequestTimeout is set to 1000 milliseconds.
    ASSERT_THROWS_CODE(locker2.lock(opCtx.get(), resIdFirstDB, MODE_X, Date_t::max()),
                       AssertionException,
                       ErrorCodes::LockTimeout);

    // locker2's attempt to acquire an uncontested lock should still succeed normally.
    locker2.lock(opCtx.get(), resIdSecondDB, MODE_X);

    ASSERT_TRUE(locker1.unlock(resIdFirstDB));
    ASSERT_TRUE(locker1.isLockHeldForMode(resIdFirstDB, MODE_NONE));
    ASSERT_TRUE(locker2.unlock(resIdSecondDB));
    ASSERT_TRUE(locker2.isLockHeldForMode(resIdSecondDB, MODE_NONE));

    ASSERT(locker1.unlockGlobal());
    ASSERT(locker2.unlockGlobal());
}

TEST_F(LockerTest, DoNotWaitForLockAcquisition) {
    auto opCtx = makeOperationContext();

    const ResourceId resIdFirstDB(RESOURCE_DATABASE,
                                  DatabaseName::createDatabaseName_forTest(boost::none, "FirstDB"));
    const ResourceId resIdSecondDB(
        RESOURCE_DATABASE, DatabaseName::createDatabaseName_forTest(boost::none, "SecondDB"));

    Locker locker1(opCtx->getServiceContext());
    Locker locker2(opCtx->getServiceContext());

    // Set up locker2 to immediately return if a lock is unavailable, regardless of supplied
    // deadlines in the lock request.
    locker2.setMaxLockTimeout(Milliseconds(0));

    locker1.lockGlobal(opCtx.get(), MODE_IX);
    locker2.lockGlobal(opCtx.get(), MODE_IX);

    // locker1 acquires FirstDB under an exclusive lock.
    locker1.lock(opCtx.get(), resIdFirstDB, MODE_X);
    ASSERT_TRUE(locker1.isLockHeldForMode(resIdFirstDB, MODE_X));

    // locker2's attempt to acquire FirstDB with unlimited wait time should fail immediately and
    // throw because _maxLockRequestTimeout was set to 0.
    ASSERT_THROWS_CODE(locker2.lock(opCtx.get(), resIdFirstDB, MODE_X, Date_t::max()),
                       AssertionException,
                       ErrorCodes::LockTimeout);

    // locker2's attempt to acquire an uncontested lock should still succeed normally.
    locker2.lock(opCtx.get(), resIdSecondDB, MODE_X);

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

TEST_F(LockerTest, GetLockerInfoShouldReportHeldLocks) {
    auto opCtx = makeOperationContext();

    const ResourceId dbId(RESOURCE_DATABASE,
                          DatabaseName::createDatabaseName_forTest(boost::none, "TestDB"));
    const ResourceId collectionId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    // Take an exclusive lock on the collection.
    Locker locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(opCtx.get(), dbId, MODE_IX);
    locker.lock(opCtx.get(), collectionId, MODE_X);

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

TEST_F(LockerTest, GetLockerInfoShouldReportPendingLocks) {
    auto opCtx = makeOperationContext();

    const ResourceId dbId(RESOURCE_DATABASE,
                          DatabaseName::createDatabaseName_forTest(boost::none, "TestDB"));
    const ResourceId collectionId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    // Take an exclusive lock on the collection.
    Locker successfulLocker(opCtx->getServiceContext());
    successfulLocker.lockGlobal(opCtx.get(), MODE_IX);
    successfulLocker.lock(opCtx.get(), dbId, MODE_IX);
    successfulLocker.lock(opCtx.get(), collectionId, MODE_X);

    // Now attempt to get conflicting locks.
    Locker conflictingLocker(opCtx->getServiceContext());
    conflictingLocker.lockGlobal(opCtx.get(), MODE_IS);
    conflictingLocker.lock(opCtx.get(), dbId, MODE_IS);
    ASSERT_EQ(LOCK_WAITING, conflictingLocker.lockBeginForTest(opCtx.get(), collectionId, MODE_IS));

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

    conflictingLocker.lockCompleteForTest(opCtx.get(), collectionId, MODE_IS, Date_t::now());

    conflictingLocker.getLockerInfo(&lockerInfo, boost::none);
    ASSERT_FALSE(lockerInfo.waitingResource.isValid());

    ASSERT(conflictingLocker.unlock(collectionId));
    ASSERT(conflictingLocker.unlock(dbId));
    ASSERT(conflictingLocker.unlockGlobal());
}

TEST_F(LockerTest, GetLockerInfoShouldSubtractBase) {
    auto opCtx = makeOperationContext();
    auto locker = shard_role_details::getLocker(opCtx.get());
    const ResourceId dbId(RESOURCE_DATABASE,
                          DatabaseName::createDatabaseName_forTest(boost::none, "SubtractTestDB"));

    auto numAcquisitions = [&](boost::optional<SingleThreadedLockStats> baseStats) {
        Locker::LockerInfo info;
        locker->getLockerInfo(&info, baseStats);
        return info.stats.get(dbId, MODE_IX).numAcquisitions;
    };
    auto getBaseStats = [&] {
        return CurOp::get(opCtx.get())->getLockStatsBase();
    };

    locker->lockGlobal(opCtx.get(), MODE_IX);

    // Obtain a lock before any other ops have been pushed to the stack.
    locker->lock(opCtx.get(), dbId, MODE_IX);
    locker->unlock(dbId);

    ASSERT_EQUALS(numAcquisitions(getBaseStats()), 1) << "The acquisition should be reported";

    // Push another op to the stack and obtain a lock.
    CurOp superOp;
    superOp.push(opCtx.get());
    locker->lock(opCtx.get(), dbId, MODE_IX);
    locker->unlock(dbId);

    ASSERT_EQUALS(numAcquisitions(getBaseStats()), 1)
        << "Only superOp's acquisition should be reported";

    // Then push another op to the stack and obtain another lock.
    CurOp subOp;
    subOp.push(opCtx.get());
    locker->lock(opCtx.get(), dbId, MODE_IX);
    locker->unlock(dbId);

    ASSERT_EQUALS(numAcquisitions(getBaseStats()), 1)
        << "Only the latest acquisition should be reported";

    ASSERT_EQUALS(numAcquisitions({}), 3)
        << "All acquisitions should be reported when no base is subtracted out.";

    ASSERT(locker->unlockGlobal());
}

TEST_F(LockerTest, ReaquireLockPendingUnlock) {
    auto opCtx = makeOperationContext();

    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    Locker locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IS);

    locker.lock(opCtx.get(), resId, MODE_X);
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_X));

    locker.beginWriteUnitOfWork();

    ASSERT_FALSE(locker.unlock(resId));
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_X));
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 1);

    // Reacquire lock pending unlock.
    locker.lock(opCtx.get(), resId, MODE_X);
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 0);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 0);

    locker.endWriteUnitOfWork();

    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_X));

    locker.unlockGlobal();
}

TEST_F(LockerTest, AcquireLockPendingUnlockWithCoveredMode) {
    auto opCtx = makeOperationContext();

    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    Locker locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IS);

    locker.lock(opCtx.get(), resId, MODE_X);
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_X));

    locker.beginWriteUnitOfWork();

    ASSERT_FALSE(locker.unlock(resId));
    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_X));
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 1);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 1);

    // Attempt to lock the resource with a mode that is covered by the existing mode.
    locker.lock(opCtx.get(), resId, MODE_IX);
    ASSERT(locker.numResourcesToUnlockAtEndUnitOfWorkForTest() == 0);
    ASSERT(locker.getRequestsForTest().find(resId).objAddr()->unlockPending == 0);

    locker.endWriteUnitOfWork();

    ASSERT_TRUE(locker.isLockHeldForMode(resId, MODE_IX));

    locker.unlockGlobal();
}

TEST_F(LockerTest, SetTicketAcquisitionForLockRAIIType) {
    auto opCtx = makeOperationContext();

    // By default, ticket acquisition is required.
    ASSERT_TRUE(shard_role_details::getLocker(opCtx.get())->shouldWaitForTicket(opCtx.get()));

    {
        ScopedAdmissionPriority setTicketAquisition(opCtx.get(),
                                                    AdmissionContext::Priority::kImmediate);
        ASSERT_FALSE(shard_role_details::getLocker(opCtx.get())->shouldWaitForTicket(opCtx.get()));
    }

    ASSERT_TRUE(shard_role_details::getLocker(opCtx.get())->shouldWaitForTicket(opCtx.get()));

    ScopedAdmissionPriority admissionPriority(opCtx.get(), AdmissionContext::Priority::kImmediate);
    ASSERT_FALSE(shard_role_details::getLocker(opCtx.get())->shouldWaitForTicket(opCtx.get()));

    {
        ScopedAdmissionPriority setTicketAquisition(opCtx.get(),
                                                    AdmissionContext::Priority::kImmediate);
        ASSERT_FALSE(shard_role_details::getLocker(opCtx.get())->shouldWaitForTicket(opCtx.get()));
    }

    ASSERT_FALSE(shard_role_details::getLocker(opCtx.get())->shouldWaitForTicket(opCtx.get()));
}

// This test exercises the lock dumping code in ~Locker in case locks are held on destruction.
DEATH_TEST_F(LockerTest,
             LocksHeldOnDestructionCausesALocksDump,
             "Operation ending while holding locks.") {
    auto opCtx = makeOperationContext();

    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    Locker locker(opCtx->getServiceContext());
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lock(opCtx.get(), resId, MODE_X);

    ASSERT(locker.isLockHeldForMode(resId, MODE_X));
    ASSERT(locker.isLockHeldForMode(resId, MODE_S));

    // 'locker' destructor should invariant because locks are still held.
}

DEATH_TEST_F(LockerTest, SaveAndRestoreGlobalRecursivelyIsFatal, "7033800") {
    auto opCtx = makeOperationContext();

    Locker::LockSnapshot lockInfo;

    Locker locker(opCtx->getServiceContext());

    // No lock requests made, no locks held.
    locker.saveLockStateAndUnlock(&lockInfo);
    ASSERT_EQUALS(0U, lockInfo.locks.size());

    // Lock the global lock.
    locker.lockGlobal(opCtx.get(), MODE_IX);
    locker.lockGlobal(opCtx.get(), MODE_IX);

    // Should invariant
    locker.saveLockStateAndUnlock(&lockInfo);
}

}  // namespace
}  // namespace mongo
