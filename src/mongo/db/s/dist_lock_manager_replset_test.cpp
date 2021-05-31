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

#include <boost/optional.hpp>
#include <map>
#include <string>
#include <vector>

#include "mongo/bson/json.h"
#include "mongo/db/s/dist_lock_catalog_mock.h"
#include "mongo/db/s/dist_lock_manager_replset.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/time_support.h"

/**
 * Tests for ReplSetDistLockManager. Note that unlock and ping operations are executed on a separate
 * thread. And since this thread cannot capture the assertion exceptions, all the assertion calls
 * should be performed on the main thread.
 */
namespace mongo {
namespace {

using unittest::assertGet;

// Max duration to wait to satisfy test invariant before joining with main test thread.
const Seconds kJoinTimeout(30);
const Milliseconds kPingInterval(2);
const Seconds kLockExpiration(10);

std::string mapToString(const StringMap<int>& map) {
    StringBuilder str;
    for (const auto& entry : map) {
        str << "(" << entry.first << ": " << entry.second << ")";
    }

    return str.str();
}

std::string vectorToString(const std::vector<std::string>& list) {
    StringBuilder str;
    for (const auto& entry : list) {
        str << "(" << entry << ")";
    }

    return str.str();
}

/**
 * Basic fixture for ReplSetDistLockManager that starts it up before the test begins
 * and shuts it down when a test finishes.
 */
class DistLockManagerReplSetTest : public ShardServerTestFixture {
protected:
    void tearDown() override {
        // Don't care about what shutDown passes to stopPing here.
        getMockCatalog()->expectStopPing([](StringData) {}, Status::OK());

        ShardServerTestFixture::tearDown();
    }

    std::unique_ptr<DistLockManager> makeDistLockManager() override {
        auto distLockCatalogMock = std::make_unique<DistLockCatalogMock>();
        _distLockCatalogMock = distLockCatalogMock.get();
        auto distLockManager =
            std::make_unique<ReplSetDistLockManager>(getServiceContext(),
                                                     _processID,
                                                     std::move(distLockCatalogMock),
                                                     kPingInterval,
                                                     kLockExpiration);
        distLockManager->markRecovered_forTest();
        return distLockManager;
    }

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        return std::make_unique<ShardingCatalogClientMock>();
    }

    std::unique_ptr<BalancerConfiguration> makeBalancerConfiguration() override {
        return std::make_unique<BalancerConfiguration>();
    }

    DistLockCatalogMock* getMockCatalog() const {
        return _distLockCatalogMock;
    }

    /**
     * Get the process id that was initialized with the lock manager being tested.
     */
    std::string getProcessID() const {
        return _processID;
    }

private:
    const std::string _processID = "test";

    DistLockCatalogMock* _distLockCatalogMock{nullptr};
};

/**
 * Test scenario:
 * 1. Grab lock.
 * 2. Unlock (on destructor of ScopedDistLock).
 * 3. Check lock id used in lock and unlock are the same.
 */
TEST_F(DistLockManagerReplSetTest, BasicLockLifeCycle) {
    std::string lockName("test");
    Date_t now(Date_t::now());
    std::string whyMsg("because");

    LocksType retLockDoc;
    retLockDoc.setName(lockName);
    retLockDoc.setState(LocksType::LOCKED);
    retLockDoc.setProcess(getProcessID());
    retLockDoc.setWho("me");
    retLockDoc.setWhy(whyMsg);
    // Will be different from the actual lock session id. For testing only.
    retLockDoc.setLockID(OID::gen());

    OID lockSessionIDPassed;

    getMockCatalog()->expectGrabLock(
        [this, &lockName, &now, &whyMsg, &lockSessionIDPassed](StringData lockID,
                                                               const OID& lockSessionID,
                                                               StringData who,
                                                               StringData processId,
                                                               Date_t time,
                                                               StringData why) {
            ASSERT_EQUALS(lockName, lockID);
            ASSERT_TRUE(lockSessionID.isSet());
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_GREATER_THAN_OR_EQUALS(time, now);
            ASSERT_EQUALS(whyMsg, why);

            lockSessionIDPassed = lockSessionID;
            getMockCatalog()->expectNoGrabLock();  // Call only once.
        },
        retLockDoc);

    int unlockCallCount = 0;
    OID unlockSessionIDPassed;

    {
        auto lockStatus = DistLockManager::get(operationContext())
                              ->lock(operationContext(),
                                     lockName,
                                     whyMsg,
                                     DistLockManager::kSingleLockAttemptTimeout);
        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->expectUnLock(
            [&unlockCallCount, &unlockSessionIDPassed](const OID& lockSessionID, StringData name) {
                unlockCallCount++;
                unlockSessionIDPassed = lockSessionID;
            },
            Status::OK());
    }

    ASSERT_EQUALS(1, unlockCallCount);
    ASSERT_EQUALS(lockSessionIDPassed, unlockSessionIDPassed);
}

/**
 * Test scenario:
 * 1. Set mock to error on grab lock.
 * 2. Grab lock attempted.
 * 3. Wait for unlock to be called.
 * 4. Check that lockSessionID used on all unlock is the same as the one used to grab lock.
 */
TEST_F(DistLockManagerReplSetTest, MustUnlockOnLockError) {
    std::string lockName("test");
    std::string me("me");
    OID lastTS;
    std::string whyMsg("because");

    getMockCatalog()->expectGrabLock(
        [this, &lockName, &lastTS, &me, &whyMsg](StringData lockID,
                                                 const OID& lockSessionID,
                                                 StringData who,
                                                 StringData processId,
                                                 Date_t time,
                                                 StringData why) {
            ASSERT_EQUALS(lockName, lockID);
            // Every attempt should have a unique sesssion ID.
            ASSERT_TRUE(lockSessionID.isSet());
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_EQUALS(whyMsg, why);

            lastTS = lockSessionID;
            getMockCatalog()->expectNoGrabLock();
        },
        {ErrorCodes::ExceededMemoryLimit, "bad remote server"});

    auto unlockMutex = MONGO_MAKE_LATCH();
    stdx::condition_variable unlockCV;
    int unlockCallCount = 0;
    OID unlockSessionIDPassed;

    getMockCatalog()->expectUnLock(
        [&unlockMutex, &unlockCV, &unlockCallCount, &unlockSessionIDPassed](
            const OID& lockSessionID, StringData name) {
            stdx::unique_lock<Latch> lk(unlockMutex);
            unlockCallCount++;
            unlockSessionIDPassed = lockSessionID;
            unlockCV.notify_all();
        },
        Status::OK());

    auto lockStatus = DistLockManager::get(operationContext())
                          ->lock(operationContext(), lockName, whyMsg, Milliseconds(10))
                          .getStatus();
    ASSERT_NOT_OK(lockStatus);
    ASSERT_EQUALS(ErrorCodes::ExceededMemoryLimit, lockStatus.code());

    bool didTimeout = false;
    {
        stdx::unique_lock<Latch> lk(unlockMutex);
        if (unlockCallCount == 0) {
            didTimeout =
                unlockCV.wait_for(lk, kJoinTimeout.toSystemDuration()) == stdx::cv_status::timeout;
        }
    }

    // Join the background thread before trying to call asserts. Shutdown calls
    // stopPing and we don't care in this test.
    getMockCatalog()->expectStopPing([](StringData) {}, Status::OK());
    DistLockManager::get(operationContext())->shutDown(operationContext());

    // No assert until shutDown has been called to make sure that the background thread
    // won't be trying to access the local variables that were captured by lamdas that
    // may have gone out of scope when the assert unwinds the stack.
    // No need to grab unlockMutex since there is only one thread running at this point.

    ASSERT_FALSE(didTimeout);
    ASSERT_EQUALS(1, unlockCallCount);
    ASSERT_EQUALS(lastTS, unlockSessionIDPassed);
}

/**
 * Test scenario:
 * 1. Ping thread started during setUp of fixture.
 * 2. Wait until ping was called at least 3 times.
 * 3. Check that correct process is being pinged.
 */
TEST_F(DistLockManagerReplSetTest, LockPinging) {
    auto testMutex = MONGO_MAKE_LATCH();
    stdx::condition_variable ping3TimesCV;
    std::vector<std::string> processIDList;

    getMockCatalog()->expectPing(
        [&testMutex, &ping3TimesCV, &processIDList](StringData processIDArg, Date_t ping) {
            stdx::lock_guard<Latch> lk(testMutex);
            processIDList.push_back(processIDArg.toString());

            if (processIDList.size() >= 3) {
                ping3TimesCV.notify_all();
            }
        },
        Status::OK());

    bool didTimeout = false;
    {
        stdx::unique_lock<Latch> lk(testMutex);
        if (processIDList.size() < 3) {
            didTimeout = ping3TimesCV.wait_for(lk, kJoinTimeout.toSystemDuration()) ==
                stdx::cv_status::timeout;
        }
    }

    // Join the background thread before trying to call asserts. Shutdown calls
    // stopPing and we don't care in this test.
    getMockCatalog()->expectStopPing([](StringData) {}, Status::OK());
    DistLockManager::get(operationContext())->shutDown(operationContext());

    // No assert until shutDown has been called to make sure that the background thread
    // won't be trying to access the local variables that were captured by lamdas that
    // may have gone out of scope when the assert unwinds the stack.
    // No need to grab testMutex since there is only one thread running at this point.

    ASSERT_FALSE(didTimeout);

    ASSERT_FALSE(processIDList.empty());
    for (const auto& processIDArg : processIDList) {
        ASSERT_EQUALS(getProcessID(), processIDArg);
    }
}

/**
 * Test scenario:
 * 1. Grab lock.
 * 2. Unlock fails 3 times.
 * 3. Unlock finally succeeds at the 4th time.
 * 4. Check that lockSessionID used on all unlock is the same as the one used to grab lock.
 */
TEST_F(DistLockManagerReplSetTest, UnlockUntilNoError) {
    auto unlockMutex = MONGO_MAKE_LATCH();
    stdx::condition_variable unlockCV;
    const unsigned int kUnlockErrorCount = 3;
    std::vector<OID> lockSessionIDPassed;

    getMockCatalog()->expectUnLock(
        [this, &unlockMutex, &unlockCV, &kUnlockErrorCount, &lockSessionIDPassed](
            const OID& lockSessionID, StringData name) {
            stdx::unique_lock<Latch> lk(unlockMutex);
            lockSessionIDPassed.push_back(lockSessionID);

            if (lockSessionIDPassed.size() >= kUnlockErrorCount) {
                getMockCatalog()->expectUnLock(
                    [&lockSessionIDPassed, &unlockMutex, &unlockCV](const OID& lockSessionID,
                                                                    StringData name) {
                        stdx::unique_lock<Latch> lk(unlockMutex);
                        lockSessionIDPassed.push_back(lockSessionID);
                        unlockCV.notify_all();
                    },
                    Status::OK());
            }
        },
        {ErrorCodes::NetworkTimeout, "bad test network"});

    OID lockSessionID;
    LocksType retLockDoc;
    retLockDoc.setName("test");
    retLockDoc.setState(LocksType::LOCKED);
    retLockDoc.setProcess(getProcessID());
    retLockDoc.setWho("me");
    retLockDoc.setWhy("why");
    // Will be different from the actual lock session id. For testing only.
    retLockDoc.setLockID(OID::gen());

    getMockCatalog()->expectGrabLock(
        [&lockSessionID](StringData lockID,
                         const OID& lockSessionIDArg,
                         StringData who,
                         StringData processId,
                         Date_t time,
                         StringData why) { lockSessionID = lockSessionIDArg; },
        retLockDoc);

    {
        auto lockStatus = DistLockManager::get(operationContext())
                              ->lock(operationContext(), "test", "why", Milliseconds(0));
    }

    bool didTimeout = false;
    {
        stdx::unique_lock<Latch> lk(unlockMutex);
        if (lockSessionIDPassed.size() < kUnlockErrorCount) {
            didTimeout =
                unlockCV.wait_for(lk, kJoinTimeout.toSystemDuration()) == stdx::cv_status::timeout;
        }
    }

    // Join the background thread before trying to call asserts. Shutdown calls
    // stopPing and we don't care in this test.
    getMockCatalog()->expectStopPing([](StringData) {}, Status::OK());
    DistLockManager::get(operationContext())->shutDown(operationContext());

    // No assert until shutDown has been called to make sure that the background thread
    // won't be trying to access the local variables that were captured by lamdas that
    // may have gone out of scope when the assert unwinds the stack.
    // No need to grab testMutex since there is only one thread running at this point.

    ASSERT_FALSE(didTimeout);

    for (const auto& id : lockSessionIDPassed) {
        ASSERT_EQUALS(lockSessionID, id);
    }
}

/**
 * Test scenario:
 * 1. Grab 2 locks.
 * 2. Trigger unlocks by making ScopedDistLock go out of scope.
 * 3. Unlocks fail and will be queued for retry.
 * 4. Unlocks will keep on failing until we see at least 3 unique ids being unlocked more
 *    than once. This implies that both ids have been retried at least 3 times.
 * 5. Check that the lock session id used when lock was called matches with unlock.
 */
TEST_F(DistLockManagerReplSetTest, MultipleQueuedUnlock) {
    auto testMutex = MONGO_MAKE_LATCH();
    stdx::condition_variable unlockCV;
    std::vector<std::string> lockIdsPassed;
    StringMap<int> unlockNameMap;  // id -> count

    /**
     * Returns true if all values in the map are greater than 2.
     */
    auto mapEntriesGreaterThanTwo = [](const decltype(unlockNameMap)& map) -> bool {
        auto iter = std::find_if(
            map.begin(),
            map.end(),
            [](const std::remove_reference<decltype(map)>::type::value_type& entry) -> bool {
                return entry.second < 3;
            });

        return iter == map.end();
    };

    getMockCatalog()->expectUnLock(
        [this, &unlockNameMap, &testMutex, &unlockCV, &mapEntriesGreaterThanTwo](
            const OID& lockSessionID, StringData name) {
            stdx::unique_lock<Latch> lk(testMutex);
            unlockNameMap[name]++;

            // Wait until we see at least 2 unique lockSessionID more than twice.
            if (unlockNameMap.size() >= 2 && mapEntriesGreaterThanTwo(unlockNameMap)) {
                getMockCatalog()->expectUnLock(
                    [&testMutex, &unlockCV](const OID& lockSessionID, StringData name) {
                        stdx::unique_lock<Latch> lk(testMutex);
                        unlockCV.notify_all();
                    },
                    Status::OK());
            }
        },
        {ErrorCodes::NetworkTimeout, "bad test network"});

    LocksType retLockDoc;
    retLockDoc.setName("test");
    retLockDoc.setState(LocksType::LOCKED);
    retLockDoc.setProcess(getProcessID());
    retLockDoc.setWho("me");
    retLockDoc.setWhy("why");
    // Will be different from the actual lock session id. For testing only.
    retLockDoc.setLockID(OID::gen());

    getMockCatalog()->expectGrabLock(
        [&testMutex, &lockIdsPassed](StringData lockID,
                                     const OID& lockSessionIDArg,
                                     StringData who,
                                     StringData processId,
                                     Date_t time,
                                     StringData why) {
            stdx::unique_lock<Latch> lk(testMutex);
            lockIdsPassed.push_back(lockID.toString());
        },
        retLockDoc);

    {
        auto lockStatus = DistLockManager::get(operationContext())
                              ->lock(operationContext(), "test", "why", Milliseconds(0));
        auto otherStatus = DistLockManager::get(operationContext())
                               ->lock(operationContext(), "lock", "why", Milliseconds(0));
    }

    bool didTimeout = false;
    {
        stdx::unique_lock<Latch> lk(testMutex);

        if (unlockNameMap.size() < 2 || !mapEntriesGreaterThanTwo(unlockNameMap)) {
            didTimeout =
                unlockCV.wait_for(lk, kJoinTimeout.toSystemDuration()) == stdx::cv_status::timeout;
        }
    }

    // Join the background thread before trying to call asserts. Shutdown calls
    // stopPing and we don't care in this test.
    getMockCatalog()->expectStopPing([](StringData) {}, Status::OK());
    DistLockManager::get(operationContext())->shutDown(operationContext());

    // No assert until shutDown has been called to make sure that the background thread
    // won't be trying to access the local variables that were captured by lamdas that
    // may have gone out of scope when the assert unwinds the stack.
    // No need to grab testMutex since there is only one thread running at this point.

    ASSERT_FALSE(didTimeout);
    ASSERT_EQUALS(2u, lockIdsPassed.size());

    for (const auto& id : lockIdsPassed) {
        ASSERT_GREATER_THAN(unlockNameMap[id], 2) << "lockIDList: " << vectorToString(lockIdsPassed)
                                                  << ", map: " << mapToString(unlockNameMap);
    }
}

TEST_F(DistLockManagerReplSetTest, CleanupPingOnShutdown) {
    bool stopPingCalled = false;
    getMockCatalog()->expectStopPing(
        [this, &stopPingCalled](StringData processID) {
            ASSERT_EQUALS(getProcessID(), processID);
            stopPingCalled = true;
        },
        Status::OK());

    DistLockManager::get(operationContext())->shutDown(operationContext());
    ASSERT_TRUE(stopPingCalled);
}

TEST_F(DistLockManagerReplSetTest, CheckLockStatusOK) {
    LocksType retLockDoc;
    retLockDoc.setName("test");
    retLockDoc.setState(LocksType::LOCKED);
    retLockDoc.setProcess(getProcessID());
    retLockDoc.setWho("me");
    retLockDoc.setWhy("why");
    // Will be different from the actual lock session id. For testing only.
    retLockDoc.setLockID(OID::gen());

    OID lockSessionID;

    getMockCatalog()->expectGrabLock(
        [&lockSessionID](StringData, const OID& ts, StringData, StringData, Date_t, StringData) {
            lockSessionID = ts;
        },
        retLockDoc);


    auto lockStatus = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "a", "", Milliseconds(0));
    ASSERT_OK(lockStatus.getStatus());

    getMockCatalog()->expectNoGrabLock();
    getMockCatalog()->expectUnLock(
        [](const OID&, StringData) {
            // Don't care
        },
        Status::OK());

    getMockCatalog()->expectNoGrabLock();
}

TEST_F(DistLockManagerReplSetTest, CheckLockStatusNoLongerOwn) {
    LocksType retLockDoc;
    retLockDoc.setName("test");
    retLockDoc.setState(LocksType::LOCKED);
    retLockDoc.setProcess(getProcessID());
    retLockDoc.setWho("me");
    retLockDoc.setWhy("why");
    // Will be different from the actual lock session id. For testing only.
    retLockDoc.setLockID(OID::gen());

    OID lockSessionID;

    getMockCatalog()->expectGrabLock(
        [&lockSessionID](StringData, const OID& ts, StringData, StringData, Date_t, StringData) {
            lockSessionID = ts;
        },
        retLockDoc);


    auto lockStatus = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "a", "", Milliseconds(0));
    ASSERT_OK(lockStatus.getStatus());

    getMockCatalog()->expectNoGrabLock();
    getMockCatalog()->expectUnLock(
        [](const OID&, StringData) {
            // Don't care
        },
        Status::OK());

    getMockCatalog()->expectNoGrabLock();
}

TEST_F(DistLockManagerReplSetTest, CheckLockStatusError) {
    LocksType retLockDoc;
    retLockDoc.setName("test");
    retLockDoc.setState(LocksType::LOCKED);
    retLockDoc.setProcess(getProcessID());
    retLockDoc.setWho("me");
    retLockDoc.setWhy("why");
    // Will be different from the actual lock session id. For testing only.
    retLockDoc.setLockID(OID::gen());

    OID lockSessionID;

    getMockCatalog()->expectGrabLock(
        [&lockSessionID](StringData, const OID& ts, StringData, StringData, Date_t, StringData) {
            lockSessionID = ts;
        },
        retLockDoc);


    auto lockStatus = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "a", "", Milliseconds(0));
    ASSERT_OK(lockStatus.getStatus());

    getMockCatalog()->expectNoGrabLock();
    getMockCatalog()->expectUnLock(
        [](const OID&, StringData) {
            // Don't care
        },
        Status::OK());

    getMockCatalog()->expectNoGrabLock();
}

/**
 * Test scenario:
 * 1. Attempt to grab lock fails because lock is already owned.
 * 2. Try to get ping data and config server clock.
 * 3. Since we don't have previous ping data to compare with, we cannot
 *    decide whether it's ok to overtake, so we can't.
 * 4. Lock expiration has elapsed and the ping has not been updated since.
 * 5. 2nd attempt to grab lock still fails for the same reason.
 * 6. But since the ping is not fresh anymore, dist lock manager should overtake lock.
 */
TEST_F(DistLockManagerReplSetTest, LockOvertakingAfterLockExpiration) {
    OID lastTS;

    getMockCatalog()->expectGrabLock(
        [&lastTS](
            StringData, const OID& lockSessionID, StringData, StringData, Date_t, StringData) {
            lastTS = lockSessionID;
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    LocksType currentLockDoc;
    currentLockDoc.setName("bar");
    currentLockDoc.setState(LocksType::LOCKED);
    currentLockDoc.setProcess("otherProcess");
    currentLockDoc.setLockID(OID("5572007fda9e476582bf3716"));
    currentLockDoc.setWho("me");
    currentLockDoc.setWhy("why");

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                          currentLockDoc);

    LockpingsType pingDoc;
    pingDoc.setProcess("otherProcess");
    pingDoc.setPing(Date_t());

    getMockCatalog()->expectGetPing(
        [](StringData process) { ASSERT_EQUALS("otherProcess", process); }, pingDoc);

    getMockCatalog()->expectGetServerInfo([]() {}, DistLockCatalog::ServerInfo(Date_t(), OID()));

    // First attempt will record the ping data.
    {
        auto status = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "bar", "", Milliseconds(0))
                          .getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }

    // Advance config server time to exceed lock expiration.
    getMockCatalog()->expectGetServerInfo(
        []() {}, DistLockCatalog::ServerInfo(Date_t() + kLockExpiration + Milliseconds(1), OID()));

    getMockCatalog()->expectOvertakeLock(
        [this, &lastTS, &currentLockDoc](StringData lockID,
                                         const OID& lockSessionID,
                                         const OID& currentHolderTS,
                                         StringData who,
                                         StringData processId,
                                         Date_t time,
                                         StringData why) {
            ASSERT_EQUALS("bar", lockID);
            ASSERT_EQUALS(lastTS, lockSessionID);
            ASSERT_EQUALS(currentLockDoc.getLockID(), currentHolderTS);
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_EQUALS("foo", why);
        },
        currentLockDoc);  // return arbitrary valid lock document, for testing purposes only.

    int unlockCallCount = 0;
    OID unlockSessionIDPassed;

    // Second attempt should overtake lock.
    {
        auto lockStatus = DistLockManager::get(operationContext())
                              ->lock(operationContext(), "bar", "foo", Milliseconds(0));

        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->expectUnLock(
            [&unlockCallCount, &unlockSessionIDPassed](const OID& lockSessionID, StringData name) {
                unlockCallCount++;
                unlockSessionIDPassed = lockSessionID;
            },
            Status::OK());
    }

    ASSERT_EQUALS(1, unlockCallCount);
    ASSERT_EQUALS(lastTS, unlockSessionIDPassed);
}

TEST_F(DistLockManagerReplSetTest, CannotOvertakeIfExpirationHasNotElapsed) {
    getMockCatalog()->expectGrabLock(
        [](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Don't care.
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    LocksType currentLockDoc;
    currentLockDoc.setName("bar");
    currentLockDoc.setState(LocksType::LOCKED);
    currentLockDoc.setProcess("otherProcess");
    currentLockDoc.setLockID(OID("5572007fda9e476582bf3716"));
    currentLockDoc.setWho("me");
    currentLockDoc.setWhy("why");

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                          currentLockDoc);

    LockpingsType pingDoc;
    pingDoc.setProcess("otherProcess");
    pingDoc.setPing(Date_t());

    getMockCatalog()->expectGetPing(
        [](StringData process) { ASSERT_EQUALS("otherProcess", process); }, pingDoc);

    getMockCatalog()->expectGetServerInfo([]() {}, DistLockCatalog::ServerInfo(Date_t(), OID()));

    // First attempt will record the ping data.
    {
        auto status = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "bar", "", Milliseconds(0))
                          .getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }

    // Advance config server time to 1 millisecond before lock expiration.
    getMockCatalog()->expectGetServerInfo(
        []() {}, DistLockCatalog::ServerInfo(Date_t() + kLockExpiration - Milliseconds(1), OID()));

    // Second attempt should still not overtake lock.
    {
        auto status = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "bar", "", Milliseconds(0))
                          .getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }
}

TEST_F(DistLockManagerReplSetTest, GetPingErrorWhileOvertaking) {
    getMockCatalog()->expectGrabLock(
        [](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Don't care
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    LocksType currentLockDoc;
    currentLockDoc.setName("bar");
    currentLockDoc.setState(LocksType::LOCKED);
    currentLockDoc.setProcess("otherProcess");
    currentLockDoc.setLockID(OID("5572007fda9e476582bf3716"));
    currentLockDoc.setWho("me");
    currentLockDoc.setWhy("why");

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                          currentLockDoc);

    getMockCatalog()->expectGetPing(
        [](StringData process) { ASSERT_EQUALS("otherProcess", process); },
        {ErrorCodes::NetworkTimeout, "bad test network"});

    auto status = DistLockManager::get(operationContext())
                      ->lock(operationContext(), "bar", "", Milliseconds(0))
                      .getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::NetworkTimeout, status.code());
}

TEST_F(DistLockManagerReplSetTest, GetInvalidPingDocumentWhileOvertaking) {
    getMockCatalog()->expectGrabLock(
        [](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Don't care
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    LocksType currentLockDoc;
    currentLockDoc.setName("bar");
    currentLockDoc.setState(LocksType::LOCKED);
    currentLockDoc.setProcess("otherProcess");
    currentLockDoc.setLockID(OID("5572007fda9e476582bf3716"));
    currentLockDoc.setWho("me");
    currentLockDoc.setWhy("why");

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                          currentLockDoc);

    LockpingsType invalidPing;
    getMockCatalog()->expectGetPing(
        [](StringData process) { ASSERT_EQUALS("otherProcess", process); }, invalidPing);

    auto status = DistLockManager::get(operationContext())
                      ->lock(operationContext(), "bar", "", Milliseconds(0))
                      .getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
}

TEST_F(DistLockManagerReplSetTest, GetServerInfoErrorWhileOvertaking) {
    getMockCatalog()->expectGrabLock(
        [](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Don't care
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    LocksType currentLockDoc;
    currentLockDoc.setName("bar");
    currentLockDoc.setState(LocksType::LOCKED);
    currentLockDoc.setProcess("otherProcess");
    currentLockDoc.setLockID(OID("5572007fda9e476582bf3716"));
    currentLockDoc.setWho("me");
    currentLockDoc.setWhy("why");

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                          currentLockDoc);

    LockpingsType pingDoc;
    pingDoc.setProcess("otherProcess");
    pingDoc.setPing(Date_t());

    getMockCatalog()->expectGetPing(
        [](StringData process) { ASSERT_EQUALS("otherProcess", process); }, pingDoc);

    getMockCatalog()->expectGetServerInfo([]() {},
                                          {ErrorCodes::NetworkTimeout, "bad test network"});

    auto status = DistLockManager::get(operationContext())
                      ->lock(operationContext(), "bar", "", Milliseconds(0))
                      .getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::NetworkTimeout, status.code());
}

TEST_F(DistLockManagerReplSetTest, GetLockErrorWhileOvertaking) {
    getMockCatalog()->expectGrabLock(
        [](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Don't care
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                          {ErrorCodes::NetworkTimeout, "bad test network"});

    auto status = DistLockManager::get(operationContext())
                      ->lock(operationContext(), "bar", "", Milliseconds(0))
                      .getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::NetworkTimeout, status.code());
}

TEST_F(DistLockManagerReplSetTest, GetLockDisappearedWhileOvertaking) {
    getMockCatalog()->expectGrabLock(
        [](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Don't care
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                          {ErrorCodes::LockNotFound, "disappeared!"});

    auto status = DistLockManager::get(operationContext())
                      ->lock(operationContext(), "bar", "", Milliseconds(0))
                      .getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
}

/**
 * 1. Try to grab lock multiple times.
 * 2. For each attempt, the ping is updated and the config server clock is advanced
 *    by increments of lock expiration duration.
 * 3. All of the previous attempt should result in lock busy.
 * 4. Try to grab lock again when the ping was not updated and lock expiration has elapsed.
 */
TEST_F(DistLockManagerReplSetTest, CannotOvertakeIfPingIsActive) {
    getMockCatalog()->expectGrabLock(
        [](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Don't care
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    LocksType currentLockDoc;
    currentLockDoc.setName("bar");
    currentLockDoc.setState(LocksType::LOCKED);
    currentLockDoc.setProcess("otherProcess");
    currentLockDoc.setLockID(OID("5572007fda9e476582bf3716"));
    currentLockDoc.setWho("me");
    currentLockDoc.setWhy("why");

    Date_t currentPing;
    LockpingsType pingDoc;
    pingDoc.setProcess("otherProcess");

    Date_t configServerLocalTime;
    int getServerInfoCallCount = 0;

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                          currentLockDoc);

    const int kLoopCount = 5;
    for (int x = 0; x < kLoopCount; x++) {
        // Advance config server time to reach lock expiration.
        configServerLocalTime += kLockExpiration;

        currentPing += Milliseconds(1);
        pingDoc.setPing(currentPing);

        getMockCatalog()->expectGetPing(
            [](StringData process) { ASSERT_EQUALS("otherProcess", process); }, pingDoc);

        getMockCatalog()->expectGetServerInfo(
            [&getServerInfoCallCount]() { getServerInfoCallCount++; },
            DistLockCatalog::ServerInfo(configServerLocalTime, OID()));

        auto status = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "bar", "", Milliseconds(0))
                          .getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }

    ASSERT_EQUALS(kLoopCount, getServerInfoCallCount);

    configServerLocalTime += kLockExpiration;
    getMockCatalog()->expectGetServerInfo(
        [&getServerInfoCallCount]() { getServerInfoCallCount++; },
        DistLockCatalog::ServerInfo(configServerLocalTime, OID()));

    OID lockTS;
    // Make sure that overtake is now ok since ping is no longer updated.
    getMockCatalog()->expectOvertakeLock(
        [this, &lockTS, &currentLockDoc](StringData lockID,
                                         const OID& lockSessionID,
                                         const OID& currentHolderTS,
                                         StringData who,
                                         StringData processId,
                                         Date_t time,
                                         StringData why) {
            ASSERT_EQUALS("bar", lockID);
            lockTS = lockSessionID;
            ASSERT_EQUALS(currentLockDoc.getLockID(), currentHolderTS);
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_EQUALS("foo", why);
        },
        currentLockDoc);  // return arbitrary valid lock document, for testing purposes only.

    int unlockCallCount = 0;
    OID unlockSessionIDPassed;

    {
        auto lockStatus = DistLockManager::get(operationContext())
                              ->lock(operationContext(), "bar", "foo", Milliseconds(0));

        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->expectUnLock(
            [&unlockCallCount, &unlockSessionIDPassed](const OID& lockSessionID, StringData name) {
                unlockCallCount++;
                unlockSessionIDPassed = lockSessionID;
            },
            Status::OK());
    }

    ASSERT_EQUALS(1, unlockCallCount);
    ASSERT_EQUALS(lockTS, unlockSessionIDPassed);
}

/**
 * 1. Try to grab lock multiple times.
 * 2. For each attempt, the owner of the lock is different and the config server clock is
 *    advanced by increments of lock expiration duration.
 * 3. All of the previous attempt should result in lock busy.
 * 4. Try to grab lock again when the ping was not updated and lock expiration has elapsed.
 */
TEST_F(DistLockManagerReplSetTest, CannotOvertakeIfOwnerJustChanged) {
    getMockCatalog()->expectGrabLock(
        [](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Don't care
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    LocksType currentLockDoc;
    currentLockDoc.setName("bar");
    currentLockDoc.setState(LocksType::LOCKED);
    currentLockDoc.setProcess("otherProcess");
    currentLockDoc.setLockID(OID("5572007fda9e476582bf3716"));
    currentLockDoc.setWho("me");
    currentLockDoc.setWhy("why");

    LockpingsType pingDoc;
    pingDoc.setProcess("otherProcess");
    pingDoc.setPing(Date_t());

    Date_t configServerLocalTime;
    int getServerInfoCallCount = 0;

    getMockCatalog()->expectGetPing(
        [](StringData process) { ASSERT_EQUALS("otherProcess", process); }, pingDoc);

    const int kLoopCount = 5;
    for (int x = 0; x < kLoopCount; x++) {
        // Advance config server time to reach lock expiration.
        configServerLocalTime += kLockExpiration;

        currentLockDoc.setLockID(OID::gen());

        getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                              currentLockDoc);

        getMockCatalog()->expectGetServerInfo(
            [&getServerInfoCallCount]() { getServerInfoCallCount++; },
            DistLockCatalog::ServerInfo(configServerLocalTime, OID()));

        auto status = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "bar", "", Milliseconds(0))
                          .getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }

    ASSERT_EQUALS(kLoopCount, getServerInfoCallCount);

    configServerLocalTime += kLockExpiration;
    getMockCatalog()->expectGetServerInfo(
        [&getServerInfoCallCount]() { getServerInfoCallCount++; },
        DistLockCatalog::ServerInfo(configServerLocalTime, OID()));

    OID lockTS;
    // Make sure that overtake is now ok since lock owner didn't change.
    getMockCatalog()->expectOvertakeLock(
        [this, &lockTS, &currentLockDoc](StringData lockID,
                                         const OID& lockSessionID,
                                         const OID& currentHolderTS,
                                         StringData who,
                                         StringData processId,
                                         Date_t time,
                                         StringData why) {
            ASSERT_EQUALS("bar", lockID);
            lockTS = lockSessionID;
            ASSERT_EQUALS(currentLockDoc.getLockID(), currentHolderTS);
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_EQUALS("foo", why);
        },
        currentLockDoc);  // return arbitrary valid lock document, for testing purposes only.

    int unlockCallCount = 0;
    OID unlockSessionIDPassed;

    {
        auto lockStatus = DistLockManager::get(operationContext())
                              ->lock(operationContext(), "bar", "foo", Milliseconds(0));

        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->expectUnLock(
            [&unlockCallCount, &unlockSessionIDPassed](const OID& lockSessionID, StringData name) {
                unlockCallCount++;
                unlockSessionIDPassed = lockSessionID;
            },
            Status::OK());
    }

    ASSERT_EQUALS(1, unlockCallCount);
    ASSERT_EQUALS(lockTS, unlockSessionIDPassed);
}

/**
 * 1. Try to grab lock multiple times.
 * 2. For each attempt, the electionId of the config server is different and the
 *    config server clock is advanced by increments of lock expiration duration.
 * 3. All of the previous attempt should result in lock busy.
 * 4. Try to grab lock again when the ping was not updated and lock expiration has elapsed.
 */
TEST_F(DistLockManagerReplSetTest, CannotOvertakeIfElectionIdChanged) {
    getMockCatalog()->expectGrabLock(
        [](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Don't care
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    LocksType currentLockDoc;
    currentLockDoc.setName("bar");
    currentLockDoc.setState(LocksType::LOCKED);
    currentLockDoc.setProcess("otherProcess");
    currentLockDoc.setLockID(OID("5572007fda9e476582bf3716"));
    currentLockDoc.setWho("me");
    currentLockDoc.setWhy("why");

    LockpingsType pingDoc;
    pingDoc.setProcess("otherProcess");
    pingDoc.setPing(Date_t());

    Date_t configServerLocalTime;
    int getServerInfoCallCount = 0;

    const LocksType& fixedLockDoc = currentLockDoc;
    const LockpingsType& fixedPingDoc = pingDoc;

    const int kLoopCount = 5;
    OID lastElectionId;
    for (int x = 0; x < kLoopCount; x++) {
        // Advance config server time to reach lock expiration.
        configServerLocalTime += kLockExpiration;

        getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                              fixedLockDoc);

        getMockCatalog()->expectGetPing(
            [](StringData process) { ASSERT_EQUALS("otherProcess", process); }, fixedPingDoc);

        lastElectionId = OID::gen();
        getMockCatalog()->expectGetServerInfo(
            [&getServerInfoCallCount]() { getServerInfoCallCount++; },
            DistLockCatalog::ServerInfo(configServerLocalTime, lastElectionId));

        auto status = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "bar", "", Milliseconds(0))
                          .getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }

    ASSERT_EQUALS(kLoopCount, getServerInfoCallCount);

    configServerLocalTime += kLockExpiration;
    getMockCatalog()->expectGetServerInfo(
        [&getServerInfoCallCount]() { getServerInfoCallCount++; },
        DistLockCatalog::ServerInfo(configServerLocalTime, lastElectionId));

    OID lockTS;
    // Make sure that overtake is now ok since electionId didn't change.
    getMockCatalog()->expectOvertakeLock(
        [this, &lockTS, &currentLockDoc](StringData lockID,
                                         const OID& lockSessionID,
                                         const OID& currentHolderTS,
                                         StringData who,
                                         StringData processId,
                                         Date_t time,
                                         StringData why) {
            ASSERT_EQUALS("bar", lockID);
            lockTS = lockSessionID;
            ASSERT_EQUALS(currentLockDoc.getLockID(), currentHolderTS);
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_EQUALS("foo", why);
        },
        currentLockDoc);  // return arbitrary valid lock document, for testing purposes only.

    int unlockCallCount = 0;
    OID unlockSessionIDPassed;

    {
        auto lockStatus = DistLockManager::get(operationContext())
                              ->lock(operationContext(), "bar", "foo", Milliseconds(0));

        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->expectUnLock(
            [&unlockCallCount, &unlockSessionIDPassed](const OID& lockSessionID, StringData name) {
                unlockCallCount++;
                unlockSessionIDPassed = lockSessionID;
            },
            Status::OK());
    }

    ASSERT_EQUALS(1, unlockCallCount);
    ASSERT_EQUALS(lockTS, unlockSessionIDPassed);
}

/**
 * 1. Try to grab lock multiple times.
 * 2. For each attempt, attempting to check the ping document results in NotWritablePrimary error.
 * 3. All of the previous attempt should result in lock busy.
 * 4. Try to grab lock again when the ping was not updated and lock expiration has elapsed.
 */
TEST_F(DistLockManagerReplSetTest, CannotOvertakeIfNoMaster) {
    getMockCatalog()->expectGrabLock(
        [](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Don't care
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    LocksType currentLockDoc;
    currentLockDoc.setName("bar");
    currentLockDoc.setState(LocksType::LOCKED);
    currentLockDoc.setProcess("otherProcess");
    currentLockDoc.setLockID(OID("5572007fda9e476582bf3716"));
    currentLockDoc.setWho("me");
    currentLockDoc.setWhy("why");

    LockpingsType pingDoc;
    pingDoc.setProcess("otherProcess");
    pingDoc.setPing(Date_t());

    int getServerInfoCallCount = 0;

    const LocksType& fixedLockDoc = currentLockDoc;
    const LockpingsType& fixedPingDoc = pingDoc;

    Date_t configServerLocalTime;
    const int kLoopCount = 4;
    OID lastElectionId;
    for (int x = 0; x < kLoopCount; x++) {
        configServerLocalTime += kLockExpiration;

        getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                              fixedLockDoc);

        getMockCatalog()->expectGetPing(
            [](StringData process) { ASSERT_EQUALS("otherProcess", process); }, fixedPingDoc);

        if (x == 0) {
            // initialize internal ping history first.
            lastElectionId = OID::gen();
            getMockCatalog()->expectGetServerInfo(
                [&getServerInfoCallCount]() { getServerInfoCallCount++; },
                DistLockCatalog::ServerInfo(configServerLocalTime, lastElectionId));
        } else {
            getMockCatalog()->expectGetServerInfo(
                [&getServerInfoCallCount]() { getServerInfoCallCount++; },
                {ErrorCodes::NotWritablePrimary, "not master"});
        }

        auto status = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "bar", "", Milliseconds(0))
                          .getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }

    ASSERT_EQUALS(kLoopCount, getServerInfoCallCount);

    getMockCatalog()->expectGetServerInfo(
        [&getServerInfoCallCount]() { getServerInfoCallCount++; },
        DistLockCatalog::ServerInfo(configServerLocalTime, lastElectionId));

    OID lockTS;
    // Make sure that overtake is now ok since electionId didn't change.
    getMockCatalog()->expectOvertakeLock(
        [this, &lockTS, &currentLockDoc](StringData lockID,
                                         const OID& lockSessionID,
                                         const OID& currentHolderTS,
                                         StringData who,
                                         StringData processId,
                                         Date_t time,
                                         StringData why) {
            ASSERT_EQUALS("bar", lockID);
            lockTS = lockSessionID;
            ASSERT_EQUALS(currentLockDoc.getLockID(), currentHolderTS);
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_EQUALS("foo", why);
        },
        currentLockDoc);  // return arbitrary valid lock document, for testing purposes only.

    int unlockCallCount = 0;
    OID unlockSessionIDPassed;

    {
        auto lockStatus = DistLockManager::get(operationContext())
                              ->lock(operationContext(), "bar", "foo", Milliseconds(0));

        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->expectUnLock(
            [&unlockCallCount, &unlockSessionIDPassed](const OID& lockSessionID, StringData name) {
                unlockCallCount++;
                unlockSessionIDPassed = lockSessionID;
            },
            Status::OK());
    }

    ASSERT_EQUALS(1, unlockCallCount);
    ASSERT_EQUALS(lockTS, unlockSessionIDPassed);
}

/**
 * Test scenario:
 * 1. Attempt to grab lock fails because lock is already owned.
 * 2. Try to get ping data and config server clock.
 * 3. Since we don't have previous ping data to compare with, we cannot
 *    decide whether it's ok to overtake, so we can't.
 * 4. Lock expiration has elapsed and the ping has not been updated since.
 * 5. 2nd attempt to grab lock still fails for the same reason.
 * 6. But since the ping is not fresh anymore, dist lock manager should overtake lock.
 * 7. Attempt to overtake resulted in an error.
 * 8. Check that unlock was called.
 */
TEST_F(DistLockManagerReplSetTest, LockOvertakingResultsInError) {
    getMockCatalog()->expectGrabLock(
        [](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Don't care
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    LocksType currentLockDoc;
    currentLockDoc.setName("bar");
    currentLockDoc.setState(LocksType::LOCKED);
    currentLockDoc.setProcess("otherProcess");
    currentLockDoc.setLockID(OID("5572007fda9e476582bf3716"));
    currentLockDoc.setWho("me");
    currentLockDoc.setWhy("why");

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                          currentLockDoc);

    LockpingsType pingDoc;
    pingDoc.setProcess("otherProcess");
    pingDoc.setPing(Date_t());

    getMockCatalog()->expectGetPing(
        [](StringData process) { ASSERT_EQUALS("otherProcess", process); }, pingDoc);

    getMockCatalog()->expectGetServerInfo([]() {}, DistLockCatalog::ServerInfo(Date_t(), OID()));

    // First attempt will record the ping data.
    {
        auto status = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "bar", "", Milliseconds(0))
                          .getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }

    // Advance config server time to exceed lock expiration.
    getMockCatalog()->expectGetServerInfo(
        []() {}, DistLockCatalog::ServerInfo(Date_t() + kLockExpiration + Milliseconds(1), OID()));

    OID lastTS;
    getMockCatalog()->expectOvertakeLock(
        [this, &lastTS, &currentLockDoc](StringData lockID,
                                         const OID& lockSessionID,
                                         const OID& currentHolderTS,
                                         StringData who,
                                         StringData processId,
                                         Date_t time,
                                         StringData why) {
            ASSERT_EQUALS("bar", lockID);
            lastTS = lockSessionID;
            ASSERT_EQUALS(currentLockDoc.getLockID(), currentHolderTS);
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_EQUALS("foo", why);
        },
        {ErrorCodes::NetworkTimeout, "bad test network"});

    OID unlockSessionIDPassed;

    auto unlockMutex = MONGO_MAKE_LATCH();
    stdx::condition_variable unlockCV;
    getMockCatalog()->expectUnLock(
        [&unlockSessionIDPassed, &unlockMutex, &unlockCV](const OID& lockSessionID,
                                                          StringData name) {
            stdx::unique_lock<Latch> lk(unlockMutex);
            unlockSessionIDPassed = lockSessionID;
            unlockCV.notify_all();
        },
        Status::OK());

    // Second attempt should overtake lock.
    auto lockStatus = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "bar", "foo", Milliseconds(0));

    ASSERT_NOT_OK(lockStatus.getStatus());

    bool didTimeout = false;
    {
        stdx::unique_lock<Latch> lk(unlockMutex);
        if (!unlockSessionIDPassed.isSet()) {
            didTimeout =
                unlockCV.wait_for(lk, kJoinTimeout.toSystemDuration()) == stdx::cv_status::timeout;
        }
    }

    // Join the background thread before trying to call asserts. Shutdown calls
    // stopPing and we don't care in this test.
    getMockCatalog()->expectStopPing([](StringData) {}, Status::OK());
    DistLockManager::get(operationContext())->shutDown(operationContext());

    // No assert until shutDown has been called to make sure that the background thread
    // won't be trying to access the local variables that were captured by lamdas that
    // may have gone out of scope when the assert unwinds the stack.
    // No need to grab testMutex since there is only one thread running at this point.

    ASSERT_FALSE(didTimeout);
    ASSERT_EQUALS(lastTS, unlockSessionIDPassed);
}

/**
 * Test scenario:
 * 1. Attempt to grab lock fails because lock is already owned.
 * 2. Try to get ping data and config server clock.
 * 3. Since we don't have previous ping data to compare with, we cannot
 *    decide whether it's ok to overtake, so we can't.
 * 4. Lock expiration has elapsed and the ping has not been updated since.
 * 5. 2nd attempt to grab lock still fails for the same reason.
 * 6. But since the ping is not fresh anymore, dist lock manager should overtake lock.
 * 7. Attempt to overtake resulted failed because someone beat us into it.
 */
TEST_F(DistLockManagerReplSetTest, LockOvertakingFailed) {
    getMockCatalog()->expectGrabLock(
        [](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Don't care
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    LocksType currentLockDoc;
    currentLockDoc.setName("bar");
    currentLockDoc.setState(LocksType::LOCKED);
    currentLockDoc.setProcess("otherProcess");
    currentLockDoc.setLockID(OID("5572007fda9e476582bf3716"));
    currentLockDoc.setWho("me");
    currentLockDoc.setWhy("why");

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                          currentLockDoc);

    LockpingsType pingDoc;
    pingDoc.setProcess("otherProcess");
    pingDoc.setPing(Date_t());

    getMockCatalog()->expectGetPing(
        [](StringData process) { ASSERT_EQUALS("otherProcess", process); }, pingDoc);

    getMockCatalog()->expectGetServerInfo([]() {}, DistLockCatalog::ServerInfo(Date_t(), OID()));

    // First attempt will record the ping data.
    {
        auto status = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "bar", "", Milliseconds(0))
                          .getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }

    // Advance config server time to exceed lock expiration.
    getMockCatalog()->expectGetServerInfo(
        []() {}, DistLockCatalog::ServerInfo(Date_t() + kLockExpiration + Milliseconds(1), OID()));

    // Second attempt should overtake lock.
    getMockCatalog()->expectOvertakeLock(
        [this, &currentLockDoc](StringData lockID,
                                const OID& lockSessionID,
                                const OID& currentHolderTS,
                                StringData who,
                                StringData processId,
                                Date_t time,
                                StringData why) {
            ASSERT_EQUALS("bar", lockID);
            ASSERT_EQUALS(currentLockDoc.getLockID(), currentHolderTS);
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_EQUALS("foo", why);
        },
        {ErrorCodes::LockStateChangeFailed, "nmod 0"});

    {
        auto status = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "bar", "foo", Milliseconds(0))
                          .getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }
}

/**
 * Test scenario:
 * 1. Attempt to grab lock fails because lock is already owned.
 * 2. Try to get ping data and config server clock.
 * 3. Since we don't have previous ping data to compare with, we cannot
 *    decide whether it's ok to overtake, so we can't.
 * 4. Lock expiration has elapsed and the ping has not been updated since.
 * 5. 2nd attempt to grab lock still fails for the same reason.
 * 6. But since the ping is not fresh anymore, dist lock manager should overtake lock.
 * 7. Attempt to overtake resulted failed because someone beat us into it.
 */
TEST_F(DistLockManagerReplSetTest, CannotOvertakeIfConfigServerClockGoesBackwards) {
    getMockCatalog()->expectGrabLock(
        [](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Don't care
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    LocksType currentLockDoc;
    currentLockDoc.setName("bar");
    currentLockDoc.setState(LocksType::LOCKED);
    currentLockDoc.setProcess("otherProcess");
    currentLockDoc.setLockID(OID("5572007fda9e476582bf3716"));
    currentLockDoc.setWho("me");
    currentLockDoc.setWhy("why");

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                          currentLockDoc);

    LockpingsType pingDoc;
    pingDoc.setProcess("otherProcess");
    pingDoc.setPing(Date_t());

    getMockCatalog()->expectGetPing(
        [](StringData process) { ASSERT_EQUALS("otherProcess", process); }, pingDoc);

    Date_t configClock(Date_t::now());
    getMockCatalog()->expectGetServerInfo([]() {}, DistLockCatalog::ServerInfo(configClock, OID()));

    // First attempt will record the ping data.
    {
        auto status = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "bar", "", Milliseconds(0))
                          .getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }

    // Make config server time go backwards by lock expiration duration.
    getMockCatalog()->expectGetServerInfo(
        []() {},
        DistLockCatalog::ServerInfo(configClock - kLockExpiration - Milliseconds(1), OID()));

    // Second attempt should not overtake lock.
    {
        auto status = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "bar", "foo", Milliseconds(0))
                          .getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }
}

TEST_F(DistLockManagerReplSetTest, LockAcquisitionRetriesOnNetworkErrorSuccess) {
    getMockCatalog()->expectGrabLock(
        [&](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Next acquisition should be successful
            LocksType currentLockDoc;
            currentLockDoc.setName("LockName");
            currentLockDoc.setState(LocksType::LOCKED);
            currentLockDoc.setProcess("otherProcess");
            currentLockDoc.setLockID(OID("5572007fda9e476582bf3716"));
            currentLockDoc.setWho("me");
            currentLockDoc.setWhy("Lock reason");

            getMockCatalog()->expectGrabLock(
                [&](StringData, const OID&, StringData, StringData, Date_t, StringData) {},
                currentLockDoc);
        },
        {ErrorCodes::NetworkTimeout, "network error"});

    getMockCatalog()->expectUnLock([&](const OID& lockSessionID, StringData name) {}, Status::OK());

    auto status = DistLockManager::get(operationContext())
                      ->lock(operationContext(), "LockName", "Lock reason", Milliseconds(0))
                      .getStatus();
    ASSERT_OK(status);
}

TEST_F(DistLockManagerReplSetTest, LockAcquisitionRetriesOnInterruptionNeverSucceeds) {
    getMockCatalog()->expectGrabLock(
        [&](StringData, const OID&, StringData, StringData, Date_t, StringData) {},
        {ErrorCodes::Interrupted, "operation interrupted"});

    getMockCatalog()->expectUnLock([&](const OID& lockSessionID, StringData name) {}, Status::OK());

    auto status = DistLockManager::get(operationContext())
                      ->lock(operationContext(), "bar", "foo", Milliseconds(0))
                      .getStatus();
    ASSERT_NOT_OK(status);
}

TEST_F(DistLockManagerReplSetTest, RecoverySuccess) {
    getMockCatalog()->expectUnlockAll(
        [&](StringData processID, boost::optional<long long> term) {});

    getMockCatalog()->expectGrabLock(
        [&](StringData, const OID&, StringData, StringData, Date_t, StringData) {},
        [&] {
            LocksType doc;
            doc.setName("RecoveryLock");
            doc.setState(LocksType::LOCKED);
            doc.setProcess(getProcessID());
            doc.setWho("me");
            doc.setWhy("because");
            doc.setLockID(OID::gen());
            return doc;
        }());

    auto replSetDistLockManager =
        checked_cast<ReplSetDistLockManager*>(DistLockManager::get(operationContext()));
    replSetDistLockManager->onStepUp(1LL);
    ASSERT_OK(replSetDistLockManager->lockDirect(
        operationContext(), "RecoveryLock", "because", DistLockManager::kDefaultLockTimeout));
}

class DistLockManagerReplSetTestWithMockTickSource : public DistLockManagerReplSetTest {
protected:
    DistLockManagerReplSetTestWithMockTickSource() {
        getServiceContext()->setTickSource(std::make_unique<TickSourceMock<>>());
    }

    /**
     * Returns the mock tick source.
     */
    TickSourceMock<>* getMockTickSource() {
        return dynamic_cast<TickSourceMock<>*>(getServiceContext()->getTickSource());
    }
};

/**
 * Test scenario:
 * 1. Grab lock fails up to 3 times.
 * 2. Check that each subsequent attempt uses the same lock session id.
 * 3. Unlock (on destructor of ScopedDistLock).
 * 4. Check lock id used in lock and unlock are the same.
 */
TEST_F(DistLockManagerReplSetTestWithMockTickSource, LockSuccessAfterRetry) {
    std::string lockName("test");
    std::string me("me");
    boost::optional<OID> lastTS;
    Date_t lastTime(Date_t::now());
    std::string whyMsg("because");

    int retryAttempt = 0;
    const int kMaxRetryAttempt = 3;

    LocksType goodLockDoc;
    goodLockDoc.setName(lockName);
    goodLockDoc.setState(LocksType::LOCKED);
    goodLockDoc.setProcess(getProcessID());
    goodLockDoc.setWho("me");
    goodLockDoc.setWhy(whyMsg);
    goodLockDoc.setLockID(OID::gen());

    getMockCatalog()->expectGrabLock(
        [this,
         &lockName,
         &lastTS,
         &me,
         &lastTime,
         &whyMsg,
         &retryAttempt,
         &kMaxRetryAttempt,
         &goodLockDoc](StringData lockID,
                       const OID& lockSessionID,
                       StringData who,
                       StringData processId,
                       Date_t time,
                       StringData why) {
            ASSERT_EQUALS(lockName, lockID);
            // Lock session ID should be the same after first attempt.
            if (lastTS) {
                ASSERT_EQUALS(*lastTS, lockSessionID);
            }
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_GREATER_THAN_OR_EQUALS(time, lastTime);
            ASSERT_EQUALS(whyMsg, why);

            lastTS = lockSessionID;
            lastTime = time;

            getMockTickSource()->advance(Milliseconds(1));

            if (++retryAttempt >= kMaxRetryAttempt) {
                getMockCatalog()->expectGrabLock(
                    [this, &lockName, &lastTS, &me, &lastTime, &whyMsg](StringData lockID,
                                                                        const OID& lockSessionID,
                                                                        StringData who,
                                                                        StringData processId,
                                                                        Date_t time,
                                                                        StringData why) {
                        ASSERT_EQUALS(lockName, lockID);
                        // Lock session ID should be the same after first attempt.
                        if (lastTS) {
                            ASSERT_EQUALS(*lastTS, lockSessionID);
                        }
                        ASSERT_TRUE(lockSessionID.isSet());
                        ASSERT_EQUALS(getProcessID(), processId);
                        ASSERT_GREATER_THAN_OR_EQUALS(time, lastTime);
                        ASSERT_EQUALS(whyMsg, why);

                        getMockCatalog()->expectNoGrabLock();

                        getMockCatalog()->expectGetLockByName(
                            [](StringData name) {
                                FAIL("should not attempt to overtake lock after successful lock");
                            },
                            LocksType());
                    },
                    goodLockDoc);
            }
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    //
    // Setup mock for lock overtaking.
    //

    LocksType currentLockDoc;
    currentLockDoc.setName("test");
    currentLockDoc.setState(LocksType::LOCKED);
    currentLockDoc.setProcess("otherProcess");
    currentLockDoc.setLockID(OID::gen());
    currentLockDoc.setWho("me");
    currentLockDoc.setWhy("why");

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("test", name); },
                                          currentLockDoc);

    LockpingsType pingDoc;
    pingDoc.setProcess("otherProcess");
    pingDoc.setPing(Date_t());

    getMockCatalog()->expectGetPing(
        [](StringData process) { ASSERT_EQUALS("otherProcess", process); }, pingDoc);

    // Config server time is fixed, so overtaking will never succeed.
    getMockCatalog()->expectGetServerInfo([]() {}, DistLockCatalog::ServerInfo(Date_t(), OID()));

    //
    // Try grabbing lock.
    //

    int unlockCallCount = 0;
    OID unlockSessionIDPassed;

    {
        auto lockStatus = DistLockManager::get(operationContext())
                              ->lock(operationContext(), lockName, whyMsg, Milliseconds(10));
        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->expectUnLock(
            [&unlockCallCount, &unlockSessionIDPassed](const OID& lockSessionID, StringData name) {
                unlockCallCount++;
                unlockSessionIDPassed = lockSessionID;
            },
            Status::OK());
    }

    ASSERT_EQUALS(1, unlockCallCount);
    ASSERT(lastTS);
    ASSERT_EQUALS(*lastTS, unlockSessionIDPassed);
}

/**
 * Test scenario:
 * 1. Grab lock fails up to 3 times.
 * 2. Check that each subsequent attempt uses the same lock session id.
 * 3. Grab lock errors out on the fourth try.
 * 4. Make sure that unlock is called to cleanup the last lock attempted that error out.
 */
TEST_F(DistLockManagerReplSetTestWithMockTickSource, LockFailsAfterRetry) {
    std::string lockName("test");
    std::string me("me");
    boost::optional<OID> lastTS;
    Date_t lastTime(Date_t::now());
    std::string whyMsg("because");

    int retryAttempt = 0;
    const int kMaxRetryAttempt = 3;

    getMockCatalog()->expectGrabLock(
        [this, &lockName, &lastTS, &me, &lastTime, &whyMsg, &retryAttempt, &kMaxRetryAttempt](
            StringData lockID,
            const OID& lockSessionID,
            StringData who,
            StringData processId,
            Date_t time,
            StringData why) {
            ASSERT_EQUALS(lockName, lockID);
            // Lock session ID should be the same after first attempt.
            if (lastTS) {
                ASSERT_EQUALS(*lastTS, lockSessionID);
            }
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_GREATER_THAN_OR_EQUALS(time, lastTime);
            ASSERT_EQUALS(whyMsg, why);

            lastTS = lockSessionID;
            lastTime = time;

            getMockTickSource()->advance(Milliseconds(1));

            if (++retryAttempt >= kMaxRetryAttempt) {
                getMockCatalog()->expectGrabLock(
                    [this, &lockName, &lastTS, &me, &lastTime, &whyMsg](StringData lockID,
                                                                        const OID& lockSessionID,
                                                                        StringData who,
                                                                        StringData processId,
                                                                        Date_t time,
                                                                        StringData why) {
                        ASSERT_EQUALS(lockName, lockID);
                        // Lock session ID should be the same after first attempt.
                        if (lastTS) {
                            ASSERT_EQUALS(*lastTS, lockSessionID);
                        }
                        lastTS = lockSessionID;
                        ASSERT_TRUE(lockSessionID.isSet());
                        ASSERT_EQUALS(getProcessID(), processId);
                        ASSERT_GREATER_THAN_OR_EQUALS(time, lastTime);
                        ASSERT_EQUALS(whyMsg, why);

                        getMockCatalog()->expectNoGrabLock();
                    },
                    {ErrorCodes::ExceededMemoryLimit, "bad remote server"});
            }
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    // Make mock return lock not found to skip lock overtaking.
    getMockCatalog()->expectGetLockByName([](StringData) {},
                                          {ErrorCodes::LockNotFound, "not found!"});

    auto unlockMutex = MONGO_MAKE_LATCH();
    stdx::condition_variable unlockCV;
    OID unlockSessionIDPassed;
    int unlockCallCount = 0;

    getMockCatalog()->expectUnLock(
        [&unlockMutex, &unlockCV, &unlockCallCount, &unlockSessionIDPassed](
            const OID& lockSessionID, StringData name) {
            stdx::unique_lock<Latch> lk(unlockMutex);
            unlockCallCount++;
            unlockSessionIDPassed = lockSessionID;
            unlockCV.notify_all();
        },
        Status::OK());

    {
        auto lockStatus = DistLockManager::get(operationContext())
                              ->lock(operationContext(), lockName, whyMsg, Milliseconds(10));
        ASSERT_NOT_OK(lockStatus.getStatus());
    }

    bool didTimeout = false;
    {
        stdx::unique_lock<Latch> lk(unlockMutex);
        if (unlockCallCount == 0) {
            didTimeout =
                unlockCV.wait_for(lk, kJoinTimeout.toSystemDuration()) == stdx::cv_status::timeout;
        }
    }

    // Join the background thread before trying to call asserts. Shutdown calls
    // stopPing and we don't care in this test.
    getMockCatalog()->expectStopPing([](StringData) {}, Status::OK());
    DistLockManager::get(operationContext())->shutDown(operationContext());

    // No assert until shutDown has been called to make sure that the background thread
    // won't be trying to access the local variables that were captured by lamdas that
    // may have gone out of scope when the assert unwinds the stack.
    // No need to grab unlockMutex since there is only one thread running at this point.

    ASSERT_FALSE(didTimeout);
    ASSERT_EQUALS(1, unlockCallCount);
    ASSERT(lastTS);
    ASSERT_EQUALS(*lastTS, unlockSessionIDPassed);
}

TEST_F(DistLockManagerReplSetTest, LockBusyNoRetry) {
    getMockCatalog()->expectGrabLock(
        [this](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            getMockCatalog()->expectNoGrabLock();  // Call only once.
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    // Make mock return lock not found to skip lock overtaking.
    getMockCatalog()->expectGetLockByName([](StringData) {},
                                          {ErrorCodes::LockNotFound, "not found!"});

    auto status = DistLockManager::get(operationContext())
                      ->lock(operationContext(), "", "", Milliseconds(0))
                      .getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
}

/**
 * Test scenario:
 * 1. Attempt to grab lock.
 * 2. Check that each subsequent attempt uses the same lock session id.
 * 3. Times out trying.
 * 4. Checks result is error.
 * 5. Implicitly check that unlock is not called (default setting of mock catalog).
 */
TEST_F(DistLockManagerReplSetTestWithMockTickSource, LockRetryTimeout) {
    std::string lockName("test");
    std::string me("me");
    boost::optional<OID> lastTS;
    Date_t lastTime(Date_t::now());
    std::string whyMsg("because");

    int retryAttempt = 0;

    getMockCatalog()->expectGrabLock(
        [this, &lockName, &lastTS, &me, &lastTime, &whyMsg, &retryAttempt](StringData lockID,
                                                                           const OID& lockSessionID,
                                                                           StringData who,
                                                                           StringData processId,
                                                                           Date_t time,
                                                                           StringData why) {
            ASSERT_EQUALS(lockName, lockID);
            // Lock session ID should be the same after first attempt.
            if (lastTS) {
                ASSERT_EQUALS(*lastTS, lockSessionID);
            }
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_GREATER_THAN_OR_EQUALS(time, lastTime);
            ASSERT_EQUALS(whyMsg, why);

            lastTS = lockSessionID;
            lastTime = time;
            retryAttempt++;

            getMockTickSource()->advance(Milliseconds(1));
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    // Make mock return lock not found to skip lock overtaking.
    getMockCatalog()->expectGetLockByName([](StringData) {},
                                          {ErrorCodes::LockNotFound, "not found!"});

    auto lockStatus = DistLockManager::get(operationContext())
                          ->lock(operationContext(), lockName, whyMsg, Milliseconds(5))
                          .getStatus();
    ASSERT_NOT_OK(lockStatus);

    ASSERT_EQUALS(ErrorCodes::LockBusy, lockStatus.code());
    ASSERT_GREATER_THAN(retryAttempt, 1);
}

/**
 * Test scenario:
 * 1. Attempt to grab lock fails because lock is already owned.
 * 2. Try to get ping data (does not exist) and config server clock.
 * 3. Since we don't have previous ping data to compare with, we cannot
 *    decide whether it's ok to overtake, so we can't.
 * 4. Lock expiration has elapsed and the ping still does not exist.
 * 5. 2nd attempt to grab lock still fails for the same reason.
 * 6. But since the ping has not been updated, dist lock manager should overtake lock.
 */
TEST_F(DistLockManagerReplSetTestWithMockTickSource, CanOvertakeIfNoPingDocument) {
    getMockCatalog()->expectGrabLock(
        [](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Don't care
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    LocksType currentLockDoc;
    currentLockDoc.setName("bar");
    currentLockDoc.setState(LocksType::LOCKED);
    currentLockDoc.setProcess("otherProcess");
    currentLockDoc.setLockID(OID("5572007fda9e476582bf3716"));
    currentLockDoc.setWho("me");
    currentLockDoc.setWhy("why");

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                          currentLockDoc);

    getMockCatalog()->expectGetPing(
        [](StringData process) { ASSERT_EQUALS("otherProcess", process); },
        {ErrorCodes::NoMatchingDocument, "no ping"});

    getMockCatalog()->expectGetServerInfo([]() {}, DistLockCatalog::ServerInfo(Date_t(), OID()));

    // First attempt will record the ping data.
    {
        auto status = DistLockManager::get(operationContext())
                          ->lock(operationContext(), "bar", "", Milliseconds(0))
                          .getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }

    OID lastTS;
    getMockCatalog()->expectGrabLock(
        [&lastTS](StringData, const OID& newTS, StringData, StringData, Date_t, StringData) {
            lastTS = newTS;
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                          currentLockDoc);

    getMockCatalog()->expectGetPing(
        [](StringData process) { ASSERT_EQUALS("otherProcess", process); },
        {ErrorCodes::NoMatchingDocument, "no ping"});

    getMockCatalog()->expectGetServerInfo(
        []() {}, DistLockCatalog::ServerInfo(Date_t() + kLockExpiration + Milliseconds(1), OID()));

    getMockCatalog()->expectOvertakeLock(
        [this, &lastTS, &currentLockDoc](StringData lockID,
                                         const OID& lockSessionID,
                                         const OID& currentHolderTS,
                                         StringData who,
                                         StringData processId,
                                         Date_t time,
                                         StringData why) {
            ASSERT_EQUALS("bar", lockID);
            ASSERT_EQUALS(lastTS, lockSessionID);
            ASSERT_EQUALS(currentLockDoc.getLockID(), currentHolderTS);
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_EQUALS("foo", why);
        },
        currentLockDoc);  // return arbitrary valid lock document, for testing purposes only.

    getMockCatalog()->expectUnLock(
        [](const OID&, StringData) {
            // Don't care
        },
        Status::OK());

    // Second attempt should overtake lock.
    {
        ASSERT_OK(DistLockManager::get(operationContext())
                      ->lock(operationContext(), "bar", "foo", Milliseconds(0))
                      .getStatus());
    }
}

TEST_F(DistLockManagerReplSetTest, TryLockWithLocalWriteConcernBusy) {
    std::string lockName("test");
    Date_t now(Date_t::now());
    std::string whyMsg("because");

    getMockCatalog()->expectGrabLock(
        [this, &lockName, &now, &whyMsg](StringData lockID,
                                         const OID& lockSessionID,
                                         StringData who,
                                         StringData processId,
                                         Date_t time,
                                         StringData why) {
            ASSERT_EQUALS(lockName, lockID);
            ASSERT_TRUE(lockSessionID.isSet());
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_GREATER_THAN_OR_EQUALS(time, now);
            ASSERT_EQUALS(whyMsg, why);

            getMockCatalog()->expectNoGrabLock();  // Call only once.
        },
        {ErrorCodes::LockStateChangeFailed, "Unable to take lock"});

    ASSERT_EQ(ErrorCodes::LockBusy,
              DistLockManager::get(operationContext())
                  ->tryLockDirectWithLocalWriteConcern(operationContext(), lockName, whyMsg));
}

}  // namespace
}  // namespace mongo
