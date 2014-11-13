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

#include "mongo/db/concurrency/lock_mgr_test_help.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    TEST(ResourceId, Semantics) {
        ResourceId resIdDb(RESOURCE_DATABASE, 324334234);
        ASSERT(resIdDb.getType() == RESOURCE_DATABASE);
        ASSERT(resIdDb.getHashId() == 324334234);

        ResourceId resIdColl(RESOURCE_COLLECTION, std::string("TestDB.collection"));
        ASSERT(resIdColl.getType() == RESOURCE_COLLECTION);

        // Comparison functions

        // Make sure the operator < is defined.
        ASSERT(resIdDb < resIdColl || resIdColl < resIdDb);

        ResourceId resId(RESOURCE_DATABASE, 324334234);
        ASSERT_EQUALS(resIdDb, resId);

        // Assignment functions
        resId = resIdColl;
        ASSERT_EQUALS(resId, resIdColl);
    }

    TEST(ResourceId, Constructors) {
        ResourceId resIdString(RESOURCE_COLLECTION, std::string("TestDB.collection"));
        ResourceId resIdStringData(RESOURCE_COLLECTION, StringData("TestDB.collection"));

        ASSERT_EQUALS(resIdString, resIdStringData);
    }


    //
    // LockManager
    //

    TEST(LockManager, Grant) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl locker(1);
        TrackingLockGrantNotification notify;

        LockRequest request;
        request.initNew(&locker, &notify);

        ASSERT(LOCK_OK == lockMgr.lock(resId, &request, MODE_S));
        ASSERT(request.mode == MODE_S);
        ASSERT(request.recursiveCount == 1);
        ASSERT(notify.numNotifies == 0);

        lockMgr.unlock(&request);
        ASSERT(request.recursiveCount == 0);
    }

    TEST(LockManager, GrantMultipleNoConflict) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl locker(1);
        TrackingLockGrantNotification notify;

        LockRequest request[6];
        for (int i = 0; i < 6; i++) {
            request[i].initNew(&locker, &notify);
            ASSERT(LOCK_OK == lockMgr.lock(resId, &request[i], MODE_S));

            ASSERT(request[i].mode == MODE_S);
            ASSERT(request[i].recursiveCount == 1);
        }

        ASSERT(notify.numNotifies == 0);

        // Free the first
        lockMgr.unlock(&request[0]);

        // Free the last
        lockMgr.unlock(&request[5]);

        // Free one in the middle
        lockMgr.unlock(&request[3]);

        // Free the remaining so the LockMgr does not compain about leaked locks
        lockMgr.unlock(&request[1]);
        lockMgr.unlock(&request[2]);
        lockMgr.unlock(&request[4]);
    }

    TEST(LockManager, GrantMultipleFIFOOrder) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        boost::scoped_ptr<MMAPV1LockerImpl> locker[6];
        for (int i = 0; i < 6; i++) {
            locker[i].reset(new MMAPV1LockerImpl(i));
        }

        TrackingLockGrantNotification notify[6];

        LockRequest request[6];
        for (int i = 0; i < 6; i++) {
            request[i].initNew(locker[i].get(), &notify[i]);
            lockMgr.lock(resId, &request[i], MODE_X);

            ASSERT(request[i].mode == MODE_X);
            ASSERT(request[i].recursiveCount == 1);
        }

        // Release the last held lock and ensure the next one, based on time is granted
        for (int i = 0; i < 5; i++) {
            lockMgr.unlock(&request[i]);

            ASSERT(notify[i + 1].numNotifies == 1);
            ASSERT(notify[i + 1].lastResId == resId);
            ASSERT(notify[i + 1].lastResult == LOCK_OK);
        }

        // Release the last one
        lockMgr.unlock(&request[5]);
    }

    TEST(LockManager, GrantRecursive) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl locker(1);
        LockRequestCombo request(&locker);

        ASSERT(LOCK_OK == lockMgr.lock(resId, &request, MODE_S));
        ASSERT(request.mode == MODE_S);
        ASSERT(request.recursiveCount == 1);
        ASSERT(request.numNotifies == 0);

        // Acquire again, in the same mode
        ASSERT(LOCK_OK == lockMgr.convert(resId, &request, MODE_S));
        ASSERT(request.mode == MODE_S);
        ASSERT(request.recursiveCount == 2);
        ASSERT(request.numNotifies == 0);

        // Release first acquire
        lockMgr.unlock(&request);
        ASSERT(request.mode == MODE_S);
        ASSERT(request.recursiveCount == 1);

        // Release second acquire
        lockMgr.unlock(&request);
        ASSERT(request.recursiveCount == 0);
    }

    TEST(LockManager, GrantRecursiveCompatibleConvertUp) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl locker(1);
        LockRequestCombo request(&locker);

        ASSERT(LOCK_OK == lockMgr.lock(resId, &request, MODE_IS));
        ASSERT(request.mode == MODE_IS);
        ASSERT(request.recursiveCount == 1);
        ASSERT(request.numNotifies == 0);

        // Acquire again, in *compatible*, but stricter mode
        ASSERT(LOCK_OK == lockMgr.convert(resId, &request, MODE_S));
        ASSERT(request.mode == MODE_S);
        ASSERT(request.recursiveCount == 2);
        ASSERT(request.numNotifies == 0);

        // Release the first acquire
        lockMgr.unlock(&request);
        ASSERT(request.mode == MODE_S);
        ASSERT(request.recursiveCount == 1);

        // Release the second acquire
        lockMgr.unlock(&request);
        ASSERT(request.recursiveCount == 0);
    }

    TEST(LockManager, GrantRecursiveNonCompatibleConvertUp) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl locker(1);
        LockRequestCombo request(&locker);

        ASSERT(LOCK_OK == lockMgr.lock(resId, &request, MODE_S));
        ASSERT(request.mode == MODE_S);
        ASSERT(request.recursiveCount == 1);
        ASSERT(request.numNotifies == 0);

        // Acquire again, in *non-compatible*, but stricter mode
        ASSERT(LOCK_OK == lockMgr.convert(resId, &request, MODE_X));
        ASSERT(request.mode == MODE_X);
        ASSERT(request.recursiveCount == 2);
        ASSERT(request.numNotifies == 0);

        // Release first acquire
        lockMgr.unlock(&request);
        ASSERT(request.mode == MODE_X);
        ASSERT(request.recursiveCount == 1);

        // Release second acquire
        lockMgr.unlock(&request);
        ASSERT(request.recursiveCount == 0);
    }

    TEST(LockManager, GrantRecursiveNonCompatibleConvertDown) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl locker(1);
        LockRequestCombo request(&locker);

        ASSERT(LOCK_OK == lockMgr.lock(resId, &request, MODE_X));
        ASSERT(request.mode == MODE_X);
        ASSERT(request.recursiveCount == 1);
        ASSERT(request.numNotifies == 0);

        // Acquire again, in *non-compatible*, but less strict mode
        ASSERT(LOCK_OK == lockMgr.convert(resId, &request, MODE_S));
        ASSERT(request.mode == MODE_X);
        ASSERT(request.recursiveCount == 2);
        ASSERT(request.numNotifies == 0);

        // Release first acquire
        lockMgr.unlock(&request);
        ASSERT(request.mode == MODE_X);
        ASSERT(request.recursiveCount == 1);

        // Release second acquire
        lockMgr.unlock(&request);
        ASSERT(request.recursiveCount == 0);
    }

    TEST(LockManager, Conflict) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl locker1(1);
        MMAPV1LockerImpl locker2(2);

        LockRequestCombo request1(&locker1);
        LockRequestCombo request2(&locker2);

        // First request granted right away
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request1, MODE_S));
        ASSERT(request1.recursiveCount == 1);
        ASSERT(request1.numNotifies == 0);

        // Second request must block
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request2, MODE_X));
        ASSERT(request2.mode == MODE_X);
        ASSERT(request2.recursiveCount == 1);
        ASSERT(request2.numNotifies == 0);

        // Release first request
        lockMgr.unlock(&request1);
        ASSERT(request1.recursiveCount == 0);
        ASSERT(request1.numNotifies == 0);

        ASSERT(request2.mode == MODE_X);
        ASSERT(request2.recursiveCount == 1);
        ASSERT(request2.numNotifies == 1);
        ASSERT(request2.lastResult == LOCK_OK);

        // Release second acquire
        lockMgr.unlock(&request2);
        ASSERT(request2.recursiveCount == 0);

        ASSERT(request1.numNotifies == 0);
        ASSERT(request2.numNotifies == 1);
    }

    TEST(LockManager, MultipleConflict) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl locker(1);
        TrackingLockGrantNotification notify;

        LockRequest request[6];
        for (int i = 0; i < 6; i++) {
            request[i].initNew(&locker, &notify);

            if (i == 0) {
                ASSERT(LOCK_OK == lockMgr.lock(resId, &request[i], MODE_X));
            }
            else {
                ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request[i], MODE_X));
            }

            ASSERT(request[i].mode == MODE_X);
            ASSERT(request[i].recursiveCount == 1);
        }

        ASSERT(notify.numNotifies == 0);

        // Free them one by one and make sure they get granted in the correct order
        for (int i = 0; i < 6; i++) {
            lockMgr.unlock(&request[i]);

            if (i < 5) {
                ASSERT(notify.numNotifies == i + 1);
            }
        }
    }

    TEST(LockManager, ConflictCancelWaiting) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl locker1(1);
        TrackingLockGrantNotification notify1;

        MMAPV1LockerImpl locker2(2);
        TrackingLockGrantNotification notify2;

        LockRequest request1;
        request1.initNew(&locker1, &notify1);

        LockRequest request2;
        request2.initNew(&locker2, &notify2);

        // First request granted right away
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request1, MODE_S));
        ASSERT(notify1.numNotifies == 0);

        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request2, MODE_X));

        // Release second request (which is still in the WAITING mode)
        lockMgr.unlock(&request2);
        ASSERT(notify2.numNotifies == 0);

        ASSERT(request1.mode == MODE_S);
        ASSERT(request1.recursiveCount == 1);

        // Release second acquire
        lockMgr.unlock(&request1);
    }

    TEST(LockManager, ConflictCancelMultipleWaiting) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl locker(1);
        TrackingLockGrantNotification notify;

        LockRequest request[6];
        for (int i = 0; i < 6; i++) {
            request[i].initNew(&locker, &notify);
            lockMgr.lock(resId, &request[i], MODE_X);

            ASSERT(request[i].mode == MODE_X);
            ASSERT(request[i].recursiveCount == 1);
        }

        ASSERT(notify.numNotifies == 0);

        // Free the second (waiting)
        lockMgr.unlock(&request[1]);

        // Free the last
        lockMgr.unlock(&request[5]);

        // Free one in the middle
        lockMgr.unlock(&request[3]);

        // Free the remaining so the LockMgr does not compain about leaked locks
        lockMgr.unlock(&request[2]);
        lockMgr.unlock(&request[4]);
        lockMgr.unlock(&request[0]);
    }

    TEST(LockManager, ConflictCancelWaitingConversion) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl locker1(1);
        MMAPV1LockerImpl locker2(2);

        LockRequestCombo request1(&locker1);
        LockRequestCombo request2(&locker2);

        // First request granted right away
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request1, MODE_S));
        ASSERT(request1.numNotifies == 0);

        // Second request is granted right away
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request2, MODE_S));
        ASSERT(request2.numNotifies == 0);

        // Convert second request to conflicting
        ASSERT(LOCK_WAITING == lockMgr.convert(resId, &request2, MODE_X));
        ASSERT(request2.mode == MODE_S);
        ASSERT(request2.convertMode == MODE_X);
        ASSERT(request2.numNotifies == 0);

        // Cancel the conflicting upgrade
        lockMgr.unlock(&request2);
        ASSERT(request2.mode == MODE_S);
        ASSERT(request2.convertMode == MODE_NONE);
        ASSERT(request2.numNotifies == 0);

        // Free the remaining locks so the LockManager destructor does not complain
        lockMgr.unlock(&request1);
        lockMgr.unlock(&request2);
    }

    TEST(LockManager, ConflictingConversion) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl locker1(1);
        MMAPV1LockerImpl locker2(2);

        LockRequestCombo request1(&locker1);
        LockRequestCombo request2(&locker2);

        // The S requests are granted right away
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request1, MODE_S));
        ASSERT(request1.numNotifies == 0);

        ASSERT(LOCK_OK == lockMgr.lock(resId, &request2, MODE_S));
        ASSERT(request2.numNotifies == 0);

        // Convert first request to conflicting
        ASSERT(LOCK_WAITING == lockMgr.convert(resId, &request1, MODE_X));
        ASSERT(request1.numNotifies == 0);

        // Free the second lock and make sure the first is granted
        lockMgr.unlock(&request2);
        ASSERT(request1.mode == MODE_X);
        ASSERT(request1.numNotifies == 1);
        ASSERT(request2.numNotifies == 0);

        // Frees the first reference, mode remains X
        lockMgr.unlock(&request1);
        ASSERT(request1.mode == MODE_X);
        ASSERT(request1.recursiveCount == 1);

        lockMgr.unlock(&request1);
    }

    TEST(LockManager, ConflictingConversionInTheMiddle) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl locker(1);
        TrackingLockGrantNotification notify;

        LockRequest request[3];
        for (int i = 0; i < 3; i++) {
            request[i].initNew(&locker, &notify);
            lockMgr.lock(resId, &request[i], MODE_S);
        }

        // Upgrade the one in the middle (not the first one)
        ASSERT(LOCK_WAITING == lockMgr.convert(resId, &request[1], MODE_X));

        ASSERT(notify.numNotifies == 0);

        // Release the two shared modes
        lockMgr.unlock(&request[0]);
        ASSERT(notify.numNotifies == 0);

        lockMgr.unlock(&request[2]);
        ASSERT(notify.numNotifies == 1);

        ASSERT(request[1].mode == MODE_X);

        // Request 1 should be unlocked twice
        lockMgr.unlock(&request[1]);
        lockMgr.unlock(&request[1]);
    }

    TEST(LockManager, ConvertUpgrade) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl locker1(1);
        LockRequestCombo request1(&locker1);
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request1, MODE_S));

        MMAPV1LockerImpl locker2(2);
        LockRequestCombo request2(&locker2);
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request2, MODE_S));

        // Upgrade the S lock to X
        ASSERT(LOCK_WAITING == lockMgr.convert(resId, &request1, MODE_X));

        ASSERT(!lockMgr.unlock(&request1));
        ASSERT(lockMgr.unlock(&request1));

        ASSERT(lockMgr.unlock(&request2));
    }

    TEST(LockManager, Downgrade) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl locker1(1);
        LockRequestCombo request1(&locker1);
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request1, MODE_X));

        MMAPV1LockerImpl locker2(2);
        LockRequestCombo request2(&locker2);
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request2, MODE_S));

        // Downgrade the X request to S
        lockMgr.downgrade(&request1, MODE_S);

        ASSERT(request2.numNotifies == 1);
        ASSERT(request2.lastResult == LOCK_OK);
        ASSERT(request2.recursiveCount == 1);

        ASSERT(lockMgr.unlock(&request1));
        ASSERT(lockMgr.unlock(&request2));
    }



    // Lock conflict matrix tests
    static void checkConflict(LockMode existingMode, LockMode newMode, bool hasConflict) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl lockerExisting(1);
        TrackingLockGrantNotification notifyExisting;
        LockRequest requestExisting;
        requestExisting.initNew(&lockerExisting, &notifyExisting);

        ASSERT(LOCK_OK == lockMgr.lock(resId, &requestExisting, existingMode));

        MMAPV1LockerImpl lockerNew(1);
        TrackingLockGrantNotification notifyNew;
        LockRequest requestNew;
        requestNew.initNew(&lockerNew, &notifyNew);

        LockResult result = lockMgr.lock(resId, &requestNew, newMode);
        if (hasConflict) {
            ASSERT_EQUALS(LOCK_WAITING, result);
        }
        else {
            ASSERT_EQUALS(LOCK_OK, result);
        }

        lockMgr.unlock(&requestNew);
        lockMgr.unlock(&requestExisting);
    }

    TEST(LockManager, ValidateConflictMatrix) {
        checkConflict(MODE_IS, MODE_IS, false);
        checkConflict(MODE_IS, MODE_IX, false);
        checkConflict(MODE_IS, MODE_S, false);
        checkConflict(MODE_IS, MODE_X, true);

        checkConflict(MODE_IX, MODE_IS, false);
        checkConflict(MODE_IX, MODE_IX, false);
        checkConflict(MODE_IX, MODE_S, true);
        checkConflict(MODE_IX, MODE_X, true);

        checkConflict(MODE_S, MODE_IS, false);
        checkConflict(MODE_S, MODE_IX, true);
        checkConflict(MODE_S, MODE_S, false);
        checkConflict(MODE_S, MODE_X, true);

        checkConflict(MODE_X, MODE_IS, true);
        checkConflict(MODE_X, MODE_IX, true);
        checkConflict(MODE_X, MODE_S, true);
        checkConflict(MODE_X, MODE_X, true);
    }

    TEST(LockManager, EnqueueAtFront) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        MMAPV1LockerImpl lockerX(1);
        LockRequestCombo requestX(&lockerX);

        ASSERT(LOCK_OK == lockMgr.lock(resId, &requestX, MODE_X));

        // The subsequent request will block
        MMAPV1LockerImpl lockerLow(2);
        LockRequestCombo requestLow(&lockerLow);

        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestLow, MODE_X));

        // This is a "queue jumping request", which will go before locker 2 above
        MMAPV1LockerImpl lockerHi(2);
        LockRequestCombo requestHi(&lockerHi);
        requestHi.enqueueAtFront = true;

        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestHi, MODE_X));

        // Once the X request is gone, lockerHi should be granted, because it's queue jumping
        ASSERT(lockMgr.unlock(&requestX));

        ASSERT(requestHi.lastResId == resId);
        ASSERT(requestHi.lastResult == LOCK_OK);

        // Finally lockerLow should be granted
        ASSERT(lockMgr.unlock(&requestHi));

        ASSERT(requestLow.lastResId == resId);
        ASSERT(requestLow.lastResult == LOCK_OK);

        // This avoids the lock manager asserting on leaked locks
        ASSERT(lockMgr.unlock(&requestLow));
    }

    TEST(LockManager, CompatibleFirstImmediateGrant) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_GLOBAL, 0);

        MMAPV1LockerImpl locker1(1);
        LockRequestCombo request1(&locker1);

        MMAPV1LockerImpl locker2(2);
        LockRequestCombo request2(&locker2);
        request2.compatibleFirst = true;

        MMAPV1LockerImpl locker3(3);
        LockRequestCombo request3(&locker3);

        // Lock all in IS mode
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request1, MODE_IS));
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request2, MODE_IS));
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request3, MODE_IS));

        // Now an exclusive mode comes, which would block
        MMAPV1LockerImpl lockerX(4);
        LockRequestCombo requestX(&lockerX);

        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestX, MODE_X));

        // If an S comes, it should be granted, because of request2
        {
            MMAPV1LockerImpl lockerS(5);
            LockRequestCombo requestS(&lockerS);
            ASSERT(LOCK_OK == lockMgr.lock(resId, &requestS, MODE_S));
            ASSERT(lockMgr.unlock(&requestS));
        }

        // If request1 goes away, the policy should still be compatible-first, because of request2
        ASSERT(lockMgr.unlock(&request1));

        // If S comes again, it should be granted, because of request2 still there
        {
            MMAPV1LockerImpl lockerS(6);
            LockRequestCombo requestS(&lockerS);
            ASSERT(LOCK_OK == lockMgr.lock(resId, &requestS, MODE_S));
            ASSERT(lockMgr.unlock(&requestS));
        }

        // With request2 gone the policy should go back to FIFO, even though request3 is active
        ASSERT(lockMgr.unlock(&request2));

        {
            MMAPV1LockerImpl lockerS(7);
            LockRequestCombo requestS(&lockerS);
            ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestS, MODE_S));
            ASSERT(lockMgr.unlock(&requestS));
        }

        // Unlock request3 to keep the lock mgr not assert for leaked locks
        ASSERT(lockMgr.unlock(&request3));
        ASSERT(lockMgr.unlock(&requestX));
    }

    TEST(LockManager, CompatibleFirstDelayedGrant) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_GLOBAL, 0);

        MMAPV1LockerImpl lockerXInitial(1);
        LockRequestCombo requestXInitial(&lockerXInitial);
        ASSERT(LOCK_OK == lockMgr.lock(resId, &requestXInitial, MODE_X));

        MMAPV1LockerImpl locker1(2);
        LockRequestCombo request1(&locker1);

        MMAPV1LockerImpl locker2(3);
        LockRequestCombo request2(&locker2);
        request2.compatibleFirst = true;

        MMAPV1LockerImpl locker3(4);
        LockRequestCombo request3(&locker3);

        // Lock all in IS mode (should block behind the global lock)
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request1, MODE_IS));
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request2, MODE_IS));
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request3, MODE_IS));

        // Now an exclusive mode comes, which would block behind the IS modes
        MMAPV1LockerImpl lockerX(5);
        LockRequestCombo requestX(&lockerX);
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestX, MODE_X));

        // Free the first X lock so all IS modes are granted
        ASSERT(lockMgr.unlock(&requestXInitial));
        ASSERT(request1.lastResult == LOCK_OK);
        ASSERT(request2.lastResult == LOCK_OK);
        ASSERT(request3.lastResult == LOCK_OK);

        // If an S comes, it should be granted, because of request2
        {
            MMAPV1LockerImpl lockerS(6);
            LockRequestCombo requestS(&lockerS);
            ASSERT(LOCK_OK == lockMgr.lock(resId, &requestS, MODE_S));
            ASSERT(lockMgr.unlock(&requestS));
        }

        // If request1 goes away, the policy should still be compatible-first, because of request2
        ASSERT(lockMgr.unlock(&request1));

        // If S comes again, it should be granted, because of request2 still there
        {
            MMAPV1LockerImpl lockerS(7);
            LockRequestCombo requestS(&lockerS);
            ASSERT(LOCK_OK == lockMgr.lock(resId, &requestS, MODE_S));
            ASSERT(lockMgr.unlock(&requestS));
        }

        // With request2 gone the policy should go back to FIFO, even though request3 is active
        ASSERT(lockMgr.unlock(&request2));

        {
            MMAPV1LockerImpl lockerS(8);
            LockRequestCombo requestS(&lockerS);
            ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestS, MODE_S));
            ASSERT(lockMgr.unlock(&requestS));
        }

        // Unlock request3 to keep the lock mgr not assert for leaked locks
        ASSERT(lockMgr.unlock(&request3));
        ASSERT(lockMgr.unlock(&requestX));
    }

    TEST(LockManager, CompatibleFirstCancelWaiting) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_GLOBAL, 0);

        MMAPV1LockerImpl lockerSInitial(1);
        LockRequestCombo requestSInitial(&lockerSInitial);
        ASSERT(LOCK_OK == lockMgr.lock(resId, &requestSInitial, MODE_S));

        MMAPV1LockerImpl lockerX(2);
        LockRequestCombo requestX(&lockerX);
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestX, MODE_X));

        MMAPV1LockerImpl lockerPending(3);
        LockRequestCombo requestPending(&lockerPending);
        requestPending.compatibleFirst = true;
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestPending, MODE_S));

        // S1 is not granted yet, so the policy should still be FIFO
        {
            MMAPV1LockerImpl lockerS(4);
            LockRequestCombo requestS(&lockerS);
            ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestS, MODE_S));
            ASSERT(lockMgr.unlock(&requestS));
        }

        // Unlock S1, the policy should still be FIFO
        ASSERT(lockMgr.unlock(&requestPending));

        {
            MMAPV1LockerImpl lockerS(5);
            LockRequestCombo requestS(&lockerS);
            ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestS, MODE_S));
            ASSERT(lockMgr.unlock(&requestS));
        }

        // Unlock remaining locks to keep the leak detection logic happy
        ASSERT(lockMgr.unlock(&requestSInitial));
        ASSERT(lockMgr.unlock(&requestX));
    }

} // namespace mongo
