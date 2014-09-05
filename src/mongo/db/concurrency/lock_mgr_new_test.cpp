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

#include "mongo/unittest/unittest.h"
#include "mongo/db/concurrency/lock_mgr_new.h"
#include "mongo/db/concurrency/lock_state.h"


namespace mongo {
namespace newlm {

    class TrackingLockGrantNotification : public LockGrantNotification {
    public:
        TrackingLockGrantNotification() : numNotifies(0), lastResult(LOCK_INVALID) {

        }

        virtual void notify(const ResourceId& resId, LockResult result) {
            numNotifies++;
            lastResId = resId;
            lastResult = result;
        }

    public:
        int numNotifies;

        ResourceId lastResId;
        LockResult lastResult;
    };



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

        LockState locker;
        TrackingLockGrantNotification notify;

        LockRequest request;
        request.initNew(resId, &locker, &notify);

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

        LockState locker;
        TrackingLockGrantNotification notify;

        LockRequest request[6];
        for (int i = 0; i < 6; i++) {
            request[i].initNew(resId, &locker, &notify);
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

        LockState locker[6];
        TrackingLockGrantNotification notify[6];

        LockRequest request[6];
        for (int i = 0; i < 6; i++) {
            request[i].initNew(resId, &locker[i], &notify[i]);
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

        LockState locker;
        TrackingLockGrantNotification notify;

        LockRequest request;
        request.initNew(resId, &locker, &notify);

        ASSERT(LOCK_OK == lockMgr.lock(resId, &request, MODE_S));
        ASSERT(request.mode == MODE_S);
        ASSERT(request.recursiveCount == 1);
        ASSERT(notify.numNotifies == 0);

        // Acquire again, in the same mode
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request, MODE_S));
        ASSERT(request.mode == MODE_S);
        ASSERT(request.recursiveCount == 2);
        ASSERT(notify.numNotifies == 0);

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

        LockState locker;
        TrackingLockGrantNotification notify;

        LockRequest request;
        request.initNew(resId, &locker, &notify);

        ASSERT(LOCK_OK == lockMgr.lock(resId, &request, MODE_IS));
        ASSERT(request.mode == MODE_IS);
        ASSERT(request.recursiveCount == 1);
        ASSERT(notify.numNotifies == 0);

        // Acquire again, in *compatible*, but stricter mode
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request, MODE_S));
        ASSERT(request.mode == MODE_S);
        ASSERT(request.recursiveCount == 2);
        ASSERT(notify.numNotifies == 0);

        // Release first acquire
        lockMgr.unlock(&request);
        ASSERT(request.mode == MODE_S);
        ASSERT(request.recursiveCount == 1);

        // Release second acquire
        lockMgr.unlock(&request);
        ASSERT(request.recursiveCount == 0);
    }

    TEST(LockManager, GrantRecursiveNonCompatibleConvertUp) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        LockState locker;
        TrackingLockGrantNotification notify;

        LockRequest request;
        request.initNew(resId, &locker, &notify);

        ASSERT(LOCK_OK == lockMgr.lock(resId, &request, MODE_S));
        ASSERT(request.mode == MODE_S);
        ASSERT(request.recursiveCount == 1);
        ASSERT(notify.numNotifies == 0);

        // Acquire again, in *non-compatible*, but stricter mode
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request, MODE_X));
        ASSERT(request.mode == MODE_X);
        ASSERT(request.recursiveCount == 2);
        ASSERT(notify.numNotifies == 0);

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

        LockState locker;
        TrackingLockGrantNotification notify;

        LockRequest request;
        request.initNew(resId, &locker, &notify);

        ASSERT(LOCK_OK == lockMgr.lock(resId, &request, MODE_X));
        ASSERT(request.mode == MODE_X);
        ASSERT(request.recursiveCount == 1);
        ASSERT(notify.numNotifies == 0);

        // Acquire again, in *non-compatible*, but stricter mode
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request, MODE_S));
        ASSERT(request.mode == MODE_X);
        ASSERT(request.recursiveCount == 2);
        ASSERT(notify.numNotifies == 0);

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

        LockState locker1;
        TrackingLockGrantNotification notify1;

        LockState locker2;
        TrackingLockGrantNotification notify2;        

        LockRequest request1;
        request1.initNew(resId, &locker1, &notify1);

        LockRequest request2;
        request2.initNew(resId, &locker2, &notify2);

        // First request granted right away
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request1, MODE_S));
        ASSERT(request1.recursiveCount == 1);
        ASSERT(notify1.numNotifies == 0);

        // Second request must block
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request2, MODE_X));
        ASSERT(request2.mode == MODE_X);
        ASSERT(request2.recursiveCount == 1);
        ASSERT(notify2.numNotifies == 0);

        // Release first request
        lockMgr.unlock(&request1);
        ASSERT(request1.recursiveCount == 0);
        ASSERT(notify1.numNotifies == 0);

        ASSERT(request2.mode == MODE_X);
        ASSERT(request2.recursiveCount == 1);
        ASSERT(notify2.numNotifies == 1);
        ASSERT(notify2.lastResult == LOCK_OK);

        // Release second acquire
        lockMgr.unlock(&request2);
        ASSERT(request2.recursiveCount == 0);

        ASSERT(notify1.numNotifies == 0);
        ASSERT(notify2.numNotifies == 1);
    }

    TEST(LockManager, MultipleConflict) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        LockState locker;
        TrackingLockGrantNotification notify;

        LockRequest request[6];
        for (int i = 0; i < 6; i++) {
            request[i].initNew(resId, &locker, &notify);

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

        // Free them one by one and make sure they get granted
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

        LockState locker1;
        TrackingLockGrantNotification notify1;

        LockState locker2;
        TrackingLockGrantNotification notify2;

        LockRequest request1;
        request1.initNew(resId, &locker1, &notify1);

        LockRequest request2;
        request2.initNew(resId, &locker2, &notify2);

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

        LockState locker;
        TrackingLockGrantNotification notify;

        LockRequest request[6];
        for (int i = 0; i < 6; i++) {
            request[i].initNew(resId, &locker, &notify);
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

        LockState locker1;
        TrackingLockGrantNotification notify1;

        LockState locker2;
        TrackingLockGrantNotification notify2;

        LockRequest request1;
        request1.initNew(resId, &locker1, &notify1);

        LockRequest request2;
        request2.initNew(resId, &locker2, &notify2);

        // First request granted right away
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request1, MODE_S));
        ASSERT(notify1.numNotifies == 0);

        // Second request is granted right away
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request2, MODE_S));
        ASSERT(notify2.numNotifies == 0);

        // Convert second request to conflicting
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request2, MODE_X));
        ASSERT(request2.mode == MODE_S);
        ASSERT(request2.convertMode == MODE_X);
        ASSERT(notify2.numNotifies == 0);

        // Cancel the conflicting upgrade
        lockMgr.unlock(&request2);
        ASSERT(request2.mode == MODE_S);
        ASSERT(request2.convertMode == MODE_NONE);
        ASSERT(notify2.numNotifies == 0);

        // Free the remaining locks so the LockManager destructor does not complain
        lockMgr.unlock(&request1);
        lockMgr.unlock(&request2);
    }

    TEST(LockManager, ConflictingConversion) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        LockState locker1;
        TrackingLockGrantNotification notify1;

        LockState locker2;
        TrackingLockGrantNotification notify2;

        LockRequest request1;
        request1.initNew(resId, &locker1, &notify1);

        LockRequest request2;
        request2.initNew(resId, &locker2, &notify2);

        // First request granted right away
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request1, MODE_S));
        ASSERT(notify1.numNotifies == 0);

        ASSERT(LOCK_OK == lockMgr.lock(resId, &request2, MODE_S));
        ASSERT(notify2.numNotifies == 0);

        // Convert first request to conflicting
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request1, MODE_X));
        ASSERT(notify1.numNotifies == 0);

        // Free the second lock and make sure the first is granted
        lockMgr.unlock(&request2);
        ASSERT(request1.mode == MODE_X);
        ASSERT(notify1.numNotifies == 1);
        ASSERT(notify2.numNotifies == 0);

        // Frees the first reference, mode remains X
        lockMgr.unlock(&request1);
        ASSERT(request1.mode == MODE_X);
        ASSERT(request1.recursiveCount == 1);

        lockMgr.unlock(&request1);
    }

    TEST(LockManager, ConflictingConversionInTheMiddle) {
        LockManager lockMgr;
        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        LockState locker;
        TrackingLockGrantNotification notify;

        LockRequest request[3];
        for (int i = 0; i < 3; i++) {
            request[i].initNew(resId, &locker, &notify);
            lockMgr.lock(resId, &request[i], MODE_S);
        }

        // Upgrade the one in the middle (not the first one)
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request[1], MODE_X));

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



    static void checkConflict(LockMode existingMode, LockMode newMode, bool hasConflict) {
        LockManager lockMgr;
        lockMgr.setNoCheckForLeakedLocksTestOnly(true);

        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        {
            LockState locker;
            TrackingLockGrantNotification notify;
            LockRequest request;
            request.initNew(resId, &locker, &notify);

            ASSERT(LOCK_OK == lockMgr.lock(resId, &request, existingMode));
        }

        {
            LockState locker;
            TrackingLockGrantNotification notify;
            LockRequest request;
            request.initNew(resId, &locker, &notify);

            LockResult result = lockMgr.lock(resId, &request, newMode);
            if (hasConflict) {
                ASSERT_EQUALS(LOCK_WAITING, result);
            }
            else {
                ASSERT_EQUALS(LOCK_OK, result);
            }
        }
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

} // namespace newlm
} // namespace mongo
