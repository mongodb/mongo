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

#include "mongo/db/local_catalog/lock_manager/lock_manager.h"

#include "mongo/base/string_data.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_test_help.h"
#include "mongo/db/local_catalog/lock_manager/locker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {

namespace resource_id_test {

TEST(ResourceIdTest, Semantics) {
    ResourceId resIdDb(RESOURCE_DATABASE, 324334234);
    ASSERT(resIdDb.getType() == RESOURCE_DATABASE);
    ASSERT(resIdDb.getHashId() == 324334234);

    ResourceId resIdColl(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));
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

TEST(ResourceIdTest, Masking) {
    const uint64_t maxHash =
        (1ULL << (64 - ResourceId::resourceTypeBits)) - 1;  //  Only 60 bits usable for hash
    ResourceType resources[3] = {RESOURCE_GLOBAL, RESOURCE_COLLECTION, RESOURCE_METADATA};
    uint64_t hashes[3] = {maxHash, maxHash / 3, maxHash / 3 * 2};

    //  The test below verifies that types/hashes are stored/retrieved unchanged
    for (int h = 0; h < 3; h++) {
        for (int r = 0; r < 3; r++) {
            ResourceId id(resources[r], hashes[h]);
            ASSERT_EQUALS(id.getHashId(), hashes[h]);
            ASSERT_EQUALS(id.getType(), resources[r]);
        }
    }
}

}  // namespace resource_id_test

namespace lock_manager_test {

class LockManagerTest : public ServiceContextTest {};

TEST(SimpleLockManagerTest, IsModeCovered) {
    ASSERT(isModeCovered(MODE_IS, MODE_IX));
}

TEST_F(LockManagerTest, Grant) {
    LockManager lockMgr;
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    Locker locker(getServiceContext());
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

TEST_F(LockManagerTest, GrantMultipleNoConflict) {
    LockManager lockMgr;
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    Locker locker(getServiceContext());
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

TEST_F(LockManagerTest, GrantMultipleFIFOOrder) {
    LockManager lockMgr;
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    std::unique_ptr<Locker> locker[6];
    for (int i = 0; i < 6; i++) {
        locker[i] = std::make_unique<Locker>(getServiceContext());
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

TEST_F(LockManagerTest, GrantRecursive) {
    LockManager lockMgr;
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    Locker locker(getServiceContext());
    LockRequestCombo request(&locker);

    ASSERT(LOCK_OK == lockMgr.lock(resId, &request, MODE_S));
    ASSERT(request.mode == MODE_S);
    ASSERT(request.recursiveCount == 1);
    ASSERT(request.numNotifies == 0);

    // Acquire again, in the same mode
    ++request.recursiveCount;

    // Release first acquire
    lockMgr.unlock(&request);
    ASSERT(request.mode == MODE_S);
    ASSERT(request.recursiveCount == 1);

    // Release second acquire
    lockMgr.unlock(&request);
    ASSERT(request.recursiveCount == 0);
}

TEST_F(LockManagerTest, Conflict) {
    LockManager lockMgr;
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    Locker locker1(getServiceContext());
    Locker locker2(getServiceContext());

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

TEST_F(LockManagerTest, MultipleConflict) {
    LockManager lockMgr;
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    Locker locker(getServiceContext());
    TrackingLockGrantNotification notify;

    LockRequest request[6];
    for (int i = 0; i < 6; i++) {
        request[i].initNew(&locker, &notify);

        if (i == 0) {
            ASSERT(LOCK_OK == lockMgr.lock(resId, &request[i], MODE_X));
        } else {
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

TEST_F(LockManagerTest, ConflictCancelWaiting) {
    LockManager lockMgr;
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    Locker locker1(getServiceContext());
    TrackingLockGrantNotification notify1;

    Locker locker2(getServiceContext());
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

TEST_F(LockManagerTest, ConflictCancelMultipleWaiting) {
    LockManager lockMgr;
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    Locker locker(getServiceContext());
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

// Lock conflict matrix tests
static void checkConflict(ServiceContext* serviceContext,
                          LockMode existingMode,
                          LockMode newMode,
                          bool hasConflict) {
    LockManager lockMgr;
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    Locker lockerExisting(serviceContext);
    TrackingLockGrantNotification notifyExisting;
    LockRequest requestExisting;
    requestExisting.initNew(&lockerExisting, &notifyExisting);

    ASSERT(LOCK_OK == lockMgr.lock(resId, &requestExisting, existingMode));

    Locker lockerNew(serviceContext);
    TrackingLockGrantNotification notifyNew;
    LockRequest requestNew;
    requestNew.initNew(&lockerNew, &notifyNew);

    LockResult result = lockMgr.lock(resId, &requestNew, newMode);
    if (hasConflict) {
        ASSERT_EQUALS(LOCK_WAITING, result);
    } else {
        ASSERT_EQUALS(LOCK_OK, result);
    }

    lockMgr.unlock(&requestNew);
    lockMgr.unlock(&requestExisting);
}

TEST_F(LockManagerTest, ValidateConflictMatrix) {
    checkConflict(getServiceContext(), MODE_IS, MODE_IS, false);
    checkConflict(getServiceContext(), MODE_IS, MODE_IX, false);
    checkConflict(getServiceContext(), MODE_IS, MODE_S, false);
    checkConflict(getServiceContext(), MODE_IS, MODE_X, true);

    checkConflict(getServiceContext(), MODE_IX, MODE_IS, false);
    checkConflict(getServiceContext(), MODE_IX, MODE_IX, false);
    checkConflict(getServiceContext(), MODE_IX, MODE_S, true);
    checkConflict(getServiceContext(), MODE_IX, MODE_X, true);

    checkConflict(getServiceContext(), MODE_S, MODE_IS, false);
    checkConflict(getServiceContext(), MODE_S, MODE_IX, true);
    checkConflict(getServiceContext(), MODE_S, MODE_S, false);
    checkConflict(getServiceContext(), MODE_S, MODE_X, true);

    checkConflict(getServiceContext(), MODE_X, MODE_IS, true);
    checkConflict(getServiceContext(), MODE_X, MODE_IX, true);
    checkConflict(getServiceContext(), MODE_X, MODE_S, true);
    checkConflict(getServiceContext(), MODE_X, MODE_X, true);
}

TEST_F(LockManagerTest, EnqueueAtFront) {
    LockManager lockMgr;
    const ResourceId resId(
        RESOURCE_COLLECTION,
        NamespaceString::createNamespaceString_forTest(boost::none, "TestDB.collection"));

    Locker lockerX(getServiceContext());
    LockRequestCombo requestX(&lockerX);

    ASSERT(LOCK_OK == lockMgr.lock(resId, &requestX, MODE_X));

    // The subsequent request will block
    Locker lockerLow(getServiceContext());
    LockRequestCombo requestLow(&lockerLow);

    ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestLow, MODE_X));

    // This is a "queue jumping request", which will go before locker 2 above
    Locker lockerHi(getServiceContext());
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

TEST_F(LockManagerTest, CompatibleFirstImmediateGrant) {
    LockManager lockMgr;
    const ResourceId resId(RESOURCE_GLOBAL, 0);

    Locker locker1(getServiceContext());
    LockRequestCombo request1(&locker1);

    Locker locker2(getServiceContext());
    LockRequestCombo request2(&locker2);
    request2.compatibleFirst = true;

    Locker locker3(getServiceContext());
    LockRequestCombo request3(&locker3);

    // Lock all in IS mode
    ASSERT(LOCK_OK == lockMgr.lock(resId, &request1, MODE_IS));
    ASSERT(LOCK_OK == lockMgr.lock(resId, &request2, MODE_IS));
    ASSERT(LOCK_OK == lockMgr.lock(resId, &request3, MODE_IS));

    // Now an exclusive mode comes, which would block
    Locker lockerX(getServiceContext());
    LockRequestCombo requestX(&lockerX);

    ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestX, MODE_X));

    // If an S comes, it should be granted, because of request2
    {
        Locker lockerS(getServiceContext());
        LockRequestCombo requestS(&lockerS);
        ASSERT(LOCK_OK == lockMgr.lock(resId, &requestS, MODE_S));
        ASSERT(lockMgr.unlock(&requestS));
    }

    // If request1 goes away, the policy should still be compatible-first, because of request2
    ASSERT(lockMgr.unlock(&request1));

    // If S comes again, it should be granted, because of request2 still there
    {
        Locker lockerS(getServiceContext());
        LockRequestCombo requestS(&lockerS);
        ASSERT(LOCK_OK == lockMgr.lock(resId, &requestS, MODE_S));
        ASSERT(lockMgr.unlock(&requestS));
    }

    // With request2 gone the policy should go back to FIFO, even though request3 is active
    ASSERT(lockMgr.unlock(&request2));

    {
        Locker lockerS(getServiceContext());
        LockRequestCombo requestS(&lockerS);
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestS, MODE_S));
        ASSERT(lockMgr.unlock(&requestS));
    }

    // Unlock request3 to keep the lock mgr not assert for leaked locks
    ASSERT(lockMgr.unlock(&request3));
    ASSERT(lockMgr.unlock(&requestX));
}

TEST_F(LockManagerTest, CompatibleFirstGrantAlreadyQueued) {
    LockManager lockMgr;
    const ResourceId resId(RESOURCE_GLOBAL, 0);

    // This tests the following behaviors (alternatives indicated with '|'):
    //   Lock held in X, queue: S X|IX IS, where S is compatibleFirst.
    //   Once X unlocks both the S and IS requests should proceed.

    LockMode conflictingModes[2] = {MODE_IX, MODE_X};

    for (LockMode writerMode : conflictingModes) {
        Locker locker1(getServiceContext());
        LockRequestCombo request1(&locker1);

        Locker locker2(getServiceContext());
        LockRequestCombo request2(&locker2);
        request2.compatibleFirst = true;

        Locker locker3(getServiceContext());
        LockRequestCombo request3(&locker3);

        Locker locker4(getServiceContext());
        LockRequestCombo request4(&locker4);

        // Hold the lock in X and establish the S IX|X IS queue.
        ASSERT(LOCK_OK == lockMgr.lock(resId, &request1, MODE_X));
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request2, MODE_S));
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request3, writerMode));
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request4, MODE_IS));

        // Now unlock the initial X, so all readers should be able to proceed, while the writer
        // remains queued.
        ASSERT(lockMgr.unlock(&request1));

        ASSERT(request2.lastResult == LOCK_OK);
        ASSERT(request3.lastResult == LOCK_INVALID);
        ASSERT(request4.lastResult == LOCK_OK);

        // Now unlock the readers, and the writer succeeds as well.
        ASSERT(lockMgr.unlock(&request2));
        ASSERT(lockMgr.unlock(&request4));
        ASSERT(request3.lastResult == LOCK_OK);

        // Unlock the writer
        ASSERT(lockMgr.unlock(&request3));
    }
}

TEST_F(LockManagerTest, CompatibleFirstDelayedGrant) {
    LockManager lockMgr;
    const ResourceId resId(RESOURCE_GLOBAL, 0);

    Locker lockerXInitial(getServiceContext());
    LockRequestCombo requestXInitial(&lockerXInitial);
    ASSERT(LOCK_OK == lockMgr.lock(resId, &requestXInitial, MODE_X));

    Locker locker1(getServiceContext());
    LockRequestCombo request1(&locker1);

    Locker locker2(getServiceContext());
    LockRequestCombo request2(&locker2);
    request2.compatibleFirst = true;

    Locker locker3(getServiceContext());
    LockRequestCombo request3(&locker3);

    // Lock all in IS mode (should block behind the global lock)
    ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request1, MODE_IS));
    ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request2, MODE_IS));
    ASSERT(LOCK_WAITING == lockMgr.lock(resId, &request3, MODE_IS));

    // Now an exclusive mode comes, which would block behind the IS modes
    Locker lockerX(getServiceContext());
    LockRequestCombo requestX(&lockerX);
    ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestX, MODE_X));

    // Free the first X lock so all IS modes are granted
    ASSERT(lockMgr.unlock(&requestXInitial));
    ASSERT(request1.lastResult == LOCK_OK);
    ASSERT(request2.lastResult == LOCK_OK);
    ASSERT(request3.lastResult == LOCK_OK);

    // If an S comes, it should be granted, because of request2
    {
        Locker lockerS(getServiceContext());
        LockRequestCombo requestS(&lockerS);
        ASSERT(LOCK_OK == lockMgr.lock(resId, &requestS, MODE_S));
        ASSERT(lockMgr.unlock(&requestS));
    }

    // If request1 goes away, the policy should still be compatible-first, because of request2
    ASSERT(lockMgr.unlock(&request1));

    // If S comes again, it should be granted, because of request2 still there
    {
        Locker lockerS(getServiceContext());
        LockRequestCombo requestS(&lockerS);
        ASSERT(LOCK_OK == lockMgr.lock(resId, &requestS, MODE_S));
        ASSERT(lockMgr.unlock(&requestS));
    }

    // With request2 gone the policy should go back to FIFO, even though request3 is active
    ASSERT(lockMgr.unlock(&request2));

    {
        Locker lockerS(getServiceContext());
        LockRequestCombo requestS(&lockerS);
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestS, MODE_S));
        ASSERT(lockMgr.unlock(&requestS));
    }

    // Unlock request3 to keep the lock mgr not assert for leaked locks
    ASSERT(lockMgr.unlock(&request3));
    ASSERT(lockMgr.unlock(&requestX));
}

TEST_F(LockManagerTest, CompatibleFirstCancelWaiting) {
    LockManager lockMgr;
    const ResourceId resId(RESOURCE_GLOBAL, 0);

    Locker lockerSInitial(getServiceContext());
    LockRequestCombo requestSInitial(&lockerSInitial);
    ASSERT(LOCK_OK == lockMgr.lock(resId, &requestSInitial, MODE_S));

    Locker lockerX(getServiceContext());
    LockRequestCombo requestX(&lockerX);
    ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestX, MODE_X));

    Locker lockerPending(getServiceContext());
    LockRequestCombo requestPending(&lockerPending);
    requestPending.compatibleFirst = true;
    ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestPending, MODE_S));

    // S1 is not granted yet, so the policy should still be FIFO
    {
        Locker lockerS(getServiceContext());
        LockRequestCombo requestS(&lockerS);
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestS, MODE_S));
        ASSERT(lockMgr.unlock(&requestS));
    }

    // Unlock S1, the policy should still be FIFO
    ASSERT(lockMgr.unlock(&requestPending));

    {
        Locker lockerS(getServiceContext());
        LockRequestCombo requestS(&lockerS);
        ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestS, MODE_S));
        ASSERT(lockMgr.unlock(&requestS));
    }

    // Unlock remaining locks to keep the leak detection logic happy
    ASSERT(lockMgr.unlock(&requestSInitial));
    ASSERT(lockMgr.unlock(&requestX));
}

TEST_F(LockManagerTest, Fairness) {
    LockManager lockMgr;
    const ResourceId resId(RESOURCE_GLOBAL, 0);

    // Start with some 'regular' intent locks
    Locker lockerIS(getServiceContext());
    LockRequestCombo requestIS(&lockerIS);
    ASSERT(LOCK_OK == lockMgr.lock(resId, &requestIS, MODE_IS));

    Locker lockerIX(getServiceContext());
    LockRequestCombo requestIX(&lockerIX);
    ASSERT(LOCK_OK == lockMgr.lock(resId, &requestIX, MODE_IX));

    // Now a conflicting lock comes
    Locker lockerX(getServiceContext());
    LockRequestCombo requestX(&lockerX);
    ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestX, MODE_X));

    // Now, whoever comes next should be blocked
    Locker lockerIX1(getServiceContext());
    LockRequestCombo requestIX1(&lockerIX1);
    ASSERT(LOCK_WAITING == lockMgr.lock(resId, &requestIX1, MODE_IX));

    // Freeing the first two locks should grant the X lock
    ASSERT(lockMgr.unlock(&requestIS));
    ASSERT(lockMgr.unlock(&requestIX));
    ASSERT_EQ(LOCK_OK, requestX.lastResult);
    ASSERT_EQ(1, requestX.numNotifies);
    ASSERT_EQ(LOCK_INVALID, requestIX1.lastResult);
    ASSERT_EQ(0, requestIX1.numNotifies);

    ASSERT(lockMgr.unlock(&requestX));
    ASSERT_EQ(LOCK_OK, requestIX1.lastResult);
    ASSERT_EQ(1, requestIX1.numNotifies);

    // Unlock all locks so we don't assert for leaked locks
    ASSERT(lockMgr.unlock(&requestIX1));
}

TEST_F(LockManagerTest, HasConflictingRequests) {
    LockManager lockMgr;
    ResourceId resId{RESOURCE_GLOBAL, 0};

    Locker lockerIX{getServiceContext()};
    LockRequestCombo requestIX{&lockerIX};
    ASSERT_EQ(lockMgr.lock(resId, &requestIX, LockMode::MODE_IX), LockResult::LOCK_OK);
    ASSERT_FALSE(lockMgr.hasConflictingRequests(resId, &requestIX));

    Locker lockerX{getServiceContext()};
    LockRequestCombo requestX{&lockerX};
    ASSERT_EQ(lockMgr.lock(resId, &requestX, LockMode::MODE_X), LockResult::LOCK_WAITING);
    ASSERT_TRUE(lockMgr.hasConflictingRequests(resId, &requestIX));
    ASSERT_TRUE(lockMgr.hasConflictingRequests(resId, &requestX));

    ASSERT(lockMgr.unlock(&requestIX));
    ASSERT(lockMgr.unlock(&requestX));
}

}  // namespace lock_manager_test

}  // namespace
}  // namespace mongo
