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

#include <vector>

#include "mongo/db/concurrency/lock_mgr_test_help.h"
#include "mongo/unittest/unittest.h"


namespace mongo {
    
    TEST(LockerImpl, LockNoConflict) {
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        LockerImpl<true> locker(1);
        locker.lockGlobal(MODE_IX);

        ASSERT(LOCK_OK == locker.lock(resId, MODE_X));

        ASSERT(locker.isLockHeldForMode(resId, MODE_X));
        ASSERT(locker.isLockHeldForMode(resId, MODE_S));

        ASSERT(locker.unlock(resId));

        ASSERT(locker.isLockHeldForMode(resId, MODE_NONE));

        locker.unlockAll();
    }

    TEST(LockerImpl, ReLockNoConflict) {
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        LockerImpl<true> locker(1);
        locker.lockGlobal(MODE_IX);

        ASSERT(LOCK_OK == locker.lock(resId, MODE_S));
        ASSERT(LOCK_OK == locker.lock(resId, MODE_X));

        ASSERT(!locker.unlock(resId));
        ASSERT(locker.isLockHeldForMode(resId, MODE_X));

        ASSERT(locker.unlock(resId));
        ASSERT(locker.isLockHeldForMode(resId, MODE_NONE));

        ASSERT(locker.unlockAll());
    }

    TEST(LockerImpl, ConflictWithTimeout) {
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        LockerImpl<true> locker1(1);
        ASSERT(LOCK_OK == locker1.lockGlobal(MODE_IX));
        ASSERT(LOCK_OK == locker1.lock(resId, MODE_X));

        LockerImpl<true> locker2(2);
        ASSERT(LOCK_OK == locker2.lockGlobal(MODE_IX));
        ASSERT(LOCK_TIMEOUT == locker2.lock(resId, MODE_S, 0));

        ASSERT(locker2.isLockHeldForMode(resId, MODE_NONE));

        ASSERT(locker1.unlock(resId));

        ASSERT(locker1.unlockAll());
        ASSERT(locker2.unlockAll());
    }

    TEST(LockerImpl, ConflictUpgradeWithTimeout) {
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        LockerImpl<true> locker1(1);
        ASSERT(LOCK_OK == locker1.lockGlobal(MODE_IS));
        ASSERT(LOCK_OK == locker1.lock(resId, MODE_S));

        LockerImpl<true> locker2(2);
        ASSERT(LOCK_OK == locker2.lockGlobal(MODE_IS));
        ASSERT(LOCK_OK == locker2.lock(resId, MODE_S));

        // Try upgrading locker 1, which should block and timeout
        ASSERT(LOCK_TIMEOUT == locker1.lock(resId, MODE_X, 1));

        locker1.unlockAll();
        locker2.unlockAll();
    }

    TEST(LockerImpl, ReadTransaction) {
        LockerImpl<true> locker(1);

        locker.lockGlobal(MODE_IS);
        locker.unlockAll();

        locker.lockGlobal(MODE_IX);
        locker.unlockAll();

        locker.lockGlobal(MODE_IX);
        locker.lockGlobal(MODE_IS);
        locker.unlockAll();
        locker.unlockAll();
    }

    TEST(LockerImpl, WriteTransactionWithCommit) {
        const ResourceId resIdCollection(RESOURCE_COLLECTION, std::string("TestDB.collection"));
        const ResourceId resIdRecordS(RESOURCE_DOCUMENT, 1);
        const ResourceId resIdRecordX(RESOURCE_DOCUMENT, 2);

        LockerImpl<true> locker(1);

        locker.lockGlobal(MODE_IX);
        {
            ASSERT(LOCK_OK == locker.lock(resIdCollection, MODE_IX, 0));

            locker.beginWriteUnitOfWork();

            ASSERT(LOCK_OK == locker.lock(resIdRecordS, MODE_S, 0));
            ASSERT(locker.getLockMode(resIdRecordS) == MODE_S);

            ASSERT(LOCK_OK == locker.lock(resIdRecordX, MODE_X, 0));
            ASSERT(locker.getLockMode(resIdRecordX) == MODE_X);

            ASSERT(locker.unlock(resIdRecordS));
            ASSERT(locker.getLockMode(resIdRecordS) == MODE_NONE);

            ASSERT(!locker.unlock(resIdRecordX));
            ASSERT(locker.getLockMode(resIdRecordX) == MODE_X);

            locker.endWriteUnitOfWork();

            {
                AutoYieldFlushLockForMMAPV1Commit flushLockYield(&locker);

                // This block simulates the flush/remap thread
                {
                    LockerImpl<true> flushLocker(2);
                    AutoAcquireFlushLockForMMAPV1Commit flushLockAcquire(&flushLocker);
                }
            }

            ASSERT(locker.getLockMode(resIdRecordX) == MODE_NONE);

            ASSERT(locker.unlock(resIdCollection));
        }
        locker.unlockAll();
    }

    /**
     * Test that saveLockerImpl<true> works by examining the output.
     */
    TEST(LockerImpl, saveAndRestoreGlobal) {
        Locker::LockSnapshot lockInfo;

        LockerImpl<true> locker(1);

        // No lock requests made, no locks held.
        locker.saveLockStateAndUnlock(&lockInfo);
        ASSERT_EQUALS(0U, lockInfo.locks.size());

        // Lock the global lock, but just once.
        locker.lockGlobal(MODE_IX);

        // We've locked the global lock.  This should be reflected in the lockInfo.
        locker.saveLockStateAndUnlock(&lockInfo);
        ASSERT(!locker.isLocked());
        ASSERT_EQUALS(MODE_IX, lockInfo.globalMode);
        ASSERT_EQUALS(1U, lockInfo.globalRecursiveCount);

        // Restore the lock(s) we had.
        locker.restoreLockState(lockInfo);

        ASSERT(locker.isLocked());
        ASSERT(locker.unlockAll());
    }

    /**
     * Test that we don't unlock when we have the global lock more than once.
     */
    TEST(LockerImpl, saveAndRestoreGlobalAcquiredTwice) {
        Locker::LockSnapshot lockInfo;

        LockerImpl<true> locker(1);

        // No lock requests made, no locks held.
        locker.saveLockStateAndUnlock(&lockInfo);
        ASSERT_EQUALS(0U, lockInfo.locks.size());

        // Lock the global lock.
        locker.lockGlobal(MODE_IX);
        locker.lockGlobal(MODE_IX);

        // This shouldn't actually unlock as we're in a nested scope.
        ASSERT(!locker.saveLockStateAndUnlock(&lockInfo));

        ASSERT(locker.isLocked());

        // We must unlockAll twice.
        ASSERT(!locker.unlockAll());
        ASSERT(locker.unlockAll());
    }

    /**
     * Tests that restoreLockerImpl<true> works by locking a db and collection and saving + restoring.
     */
    TEST(LockerImpl, saveAndRestoreDBAndCollection) {
        Locker::LockSnapshot lockInfo;

        LockerImpl<true> locker(1);

        const ResourceId resIdDatabase(RESOURCE_DATABASE, std::string("TestDB"));
        const ResourceId resIdCollection(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        // Lock some stuff.
        locker.lockGlobal(MODE_IX);
        ASSERT(LOCK_OK == locker.lock(resIdDatabase, MODE_IX));
        ASSERT(LOCK_OK == locker.lock(resIdCollection, MODE_X));
        locker.saveLockStateAndUnlock(&lockInfo);

        // Things shouldn't be locked anymore.
        ASSERT(locker.getLockMode(resIdDatabase) == MODE_NONE);
        ASSERT(locker.getLockMode(resIdCollection) == MODE_NONE);

        // Restore lock state.
        locker.restoreLockState(lockInfo);

        // Make sure things were re-locked.
        ASSERT(locker.getLockMode(resIdDatabase) == MODE_IX);
        ASSERT(locker.getLockMode(resIdCollection) == MODE_X);

        locker.unlockAll();
    }

} // namespace mongo
