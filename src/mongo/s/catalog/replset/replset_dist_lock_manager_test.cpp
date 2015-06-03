/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/s/catalog/replset/replset_dist_lock_manager.h"

#include "mongo/platform/basic.h"

#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/catalog/dist_lock_catalog_mock.h"
#include "mongo/s/type_lockpings.h"
#include "mongo/s/type_locks.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

/**
 * Tests for ReplSetDistLockManager. Note that unlock and ping operations are executed on a
 * separate thread. And since this thread cannot capture the assertion exceptions, all the
 * assertion calls should be performed on the main thread.
 */

namespace mongo {
namespace {

    using std::map;
    using std::string;
    using std::vector;

    const Milliseconds kUnlockTimeout(30 * 1000);
    const Milliseconds kPingInterval(2);

    /**
     * Basic fixture for ReplSetDistLockManager that starts it up before the test begins
     * and shuts it down when a test finishes.
     */
    class ReplSetDistLockManagerFixture: public mongo::unittest::Test {
    public:
        ReplSetDistLockManagerFixture():
            _dummyDoNotUse(stdx::make_unique<DistLockCatalogMock>()),
            _mockCatalog(_dummyDoNotUse.get()),
            _processID("test"),
            _mgr(_processID, std::move(_dummyDoNotUse), kPingInterval) {
        }

        /**
         * Returns the lock manager instance that is being tested.
         */
        ReplSetDistLockManager* getMgr() {
            return &_mgr;
        }

        /**
         * Returns the mocked catalog used by the lock manager being tested.
         */
        DistLockCatalogMock* getMockCatalog() {
            return _mockCatalog;
        }

        /**
         * Get the process id that was initialiezd with the lock manager being tested.
         */
        string getProcessID() const {
            return _processID;
        }

    protected:
        void setUp() override {
            _mgr.startUp();
        }

        void tearDown() override {
            // Don't care about what shutDown passes to stopPing here.
            _mockCatalog->setSucceedingExpectedStopPing([](StringData){}, Status::OK());
            _mgr.shutDown();
        }

    private:
        std::unique_ptr<DistLockCatalogMock> _dummyDoNotUse; // dummy placeholder
        DistLockCatalogMock* _mockCatalog;
        string _processID;
        ReplSetDistLockManager _mgr;
    };

    std::string mapToString(const std::map<OID, int>& map) {
        StringBuilder str;

        for (const auto& entry : map) {
            str << "(" << entry.first.toString() << ": " << entry.second << ")";
        }

        return str.str();
    };

    std::string vectorToString(const std::vector<OID>& list) {
        StringBuilder str;

        for (const auto& entry : list) {
            str << "(" << entry.toString() << ")";
        }

        return str.str();
    };

    /**
     * Test scenario:
     * 1. Grab lock.
     * 2. Unlock (on destructor of ScopedDistLock).
     * 3. Check lock id used in lock and unlock are the same.
     */
    TEST_F(ReplSetDistLockManagerFixture, BasicLockLifeCycle) {
        string lockName("test");
        Date_t now(Date_t::now());
        string whyMsg("because");

        LocksType retLockDoc;
        retLockDoc.setName(lockName);
        retLockDoc.setState(LocksType::LOCKED);
        retLockDoc.setProcess(getProcessID());
        retLockDoc.setWho("me");
        retLockDoc.setWhy(whyMsg);
        // Will be different from the actual lock session id. For testing only.
        retLockDoc.setLockID(OID::gen());

        OID lockSessionIDPassed;

        getMockCatalog()->setSucceedingExpectedGrabLock(
                [this, &lockName, &now, &whyMsg, &lockSessionIDPassed](
                        StringData lockID,
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
            getMockCatalog()->expectNoGrabLock(); // Call only once.
        }, retLockDoc);

        int unlockCallCount = 0;
        OID unlockSessionIDPassed;

        {
            auto lockStatus = getMgr()->lock(lockName,
                                             whyMsg,
                                             DistLockManager::kDefaultSingleLockAttemptTimeout,
                                             DistLockManager::kDefaultLockRetryInterval);
            ASSERT_OK(lockStatus.getStatus());

            getMockCatalog()->expectNoGrabLock();
            getMockCatalog()->setSucceedingExpectedUnLock(
                    [&unlockCallCount, &unlockSessionIDPassed](const OID& lockSessionID) {
                unlockCallCount++;
                unlockSessionIDPassed = lockSessionID;
            }, Status::OK());
        }

        ASSERT_EQUALS(1, unlockCallCount);
        ASSERT_EQUALS(lockSessionIDPassed, unlockSessionIDPassed);
    }

    /**
     * Test scenario:
     * 1. Grab lock fails up to 3 times.
     * 2. Check that each attempt uses a unique lock session id.
     * 3. Unlock (on destructor of ScopedDistLock).
     * 4. Check lock id used in lock and unlock are the same.
     */
    TEST_F(ReplSetDistLockManagerFixture, LockSuccessAfterRetry) {
        string lockName("test");
        string me("me");
        OID lastTS;
        Date_t lastTime(Date_t::now());
        string whyMsg("because");

        int retryAttempt = 0;
        const int kMaxRetryAttempt = 3;

        LocksType goodLockDoc;
        goodLockDoc.setName(lockName);
        goodLockDoc.setState(LocksType::LOCKED);
        goodLockDoc.setProcess(getProcessID());
        goodLockDoc.setWho("me");
        goodLockDoc.setWhy(whyMsg);
        goodLockDoc.setLockID(OID::gen());

        getMockCatalog()->setSucceedingExpectedGrabLock(
                [this,
                 &lockName,
                 &lastTS,
                 &me,
                 &lastTime,
                 &whyMsg,
                 &retryAttempt,
                 &kMaxRetryAttempt,
                 &goodLockDoc](
                        StringData lockID,
                        const OID& lockSessionID,
                        StringData who,
                        StringData processId,
                        Date_t time,
                        StringData why) {
            ASSERT_EQUALS(lockName, lockID);
            // Every attempt should have a unique sesssion ID.
            ASSERT_NOT_EQUALS(lastTS, lockSessionID);
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_GREATER_THAN_OR_EQUALS(time, lastTime);
            ASSERT_EQUALS(whyMsg, why);

            lastTS = lockSessionID;
            lastTime = time;

            if (++retryAttempt >= kMaxRetryAttempt) {
                getMockCatalog()->setSucceedingExpectedGrabLock([this,
                                                                 &lockName,
                                                                 &lastTS,
                                                                 &me,
                                                                 &lastTime,
                                                                 &whyMsg](
                        StringData lockID,
                        const OID& lockSessionID,
                        StringData who,
                        StringData processId,
                        Date_t time,
                        StringData why) {
                    ASSERT_EQUALS(lockName, lockID);
                    // Every attempt should have a unique sesssion ID.
                    ASSERT_NOT_EQUALS(lastTS, lockSessionID);
                    lastTS = lockSessionID;
                    ASSERT_TRUE(lockSessionID.isSet());
                    ASSERT_EQUALS(getProcessID(), processId);
                    ASSERT_GREATER_THAN_OR_EQUALS(time, lastTime);
                    ASSERT_EQUALS(whyMsg, why);

                    getMockCatalog()->expectNoGrabLock();
                }, goodLockDoc);
            }
        }, {ErrorCodes::LockStateChangeFailed, "nMod 0"});

        int unlockCallCount = 0;
        OID unlockSessionIDPassed;

        {
            auto lockStatus = getMgr()->lock(lockName, whyMsg, Milliseconds(10), Milliseconds(1));
            ASSERT_OK(lockStatus.getStatus());

            getMockCatalog()->expectNoGrabLock();
            getMockCatalog()->setSucceedingExpectedUnLock(
                    [&unlockCallCount, &unlockSessionIDPassed](const OID& lockSessionID) {
                unlockCallCount++;
                unlockSessionIDPassed = lockSessionID;
            }, Status::OK());
        }

        ASSERT_EQUALS(1, unlockCallCount);
        ASSERT_EQUALS(lastTS, unlockSessionIDPassed);
    }

    /**
     * Test scenario:
     * 1. Grab lock fails up to 3 times.
     * 2. Check that each attempt uses a unique lock session id.
     * 3. Grab lock errors out on the fourth try.
     * 4. Make sure that unlock is called to cleanup the last lock attempted that error out.
     */
    TEST_F(ReplSetDistLockManagerFixture, LockFailsAfterRetry) {
        string lockName("test");
        string me("me");
        OID lastTS;
        Date_t lastTime(Date_t::now());
        string whyMsg("because");

        int retryAttempt = 0;
        const int kMaxRetryAttempt = 3;

        getMockCatalog()->setSucceedingExpectedGrabLock(
                [this,
                 &lockName,
                 &lastTS,
                 &me,
                 &lastTime,
                 &whyMsg,
                 &retryAttempt,
                 &kMaxRetryAttempt](
                        StringData lockID,
                        const OID& lockSessionID,
                        StringData who,
                        StringData processId,
                        Date_t time,
                        StringData why) {
            ASSERT_EQUALS(lockName, lockID);
            // Every attempt should have a unique sesssion ID.
            ASSERT_NOT_EQUALS(lastTS, lockSessionID);
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_GREATER_THAN_OR_EQUALS(time, lastTime);
            ASSERT_EQUALS(whyMsg, why);

            lastTS = lockSessionID;
            lastTime = time;

            if (++retryAttempt >= kMaxRetryAttempt) {
                getMockCatalog()->setSucceedingExpectedGrabLock([this,
                                                                 &lockName,
                                                                 &lastTS,
                                                                 &me,
                                                                 &lastTime,
                                                                 &whyMsg](
                        StringData lockID,
                        const OID& lockSessionID,
                        StringData who,
                        StringData processId,
                        Date_t time,
                        StringData why) {
                    ASSERT_EQUALS(lockName, lockID);
                    // Every attempt should have a unique sesssion ID.
                    ASSERT_NOT_EQUALS(lastTS, lockSessionID);
                    lastTS = lockSessionID;
                    ASSERT_TRUE(lockSessionID.isSet());
                    ASSERT_EQUALS(getProcessID(), processId);
                    ASSERT_GREATER_THAN_OR_EQUALS(time, lastTime);
                    ASSERT_EQUALS(whyMsg, why);

                    getMockCatalog()->expectNoGrabLock();
                }, {ErrorCodes::NetworkTimeout, "bad test network"});
            }
        }, {ErrorCodes::LockStateChangeFailed, "nMod 0"});

        stdx::mutex unlockMutex;
        stdx::condition_variable unlockCV;
        OID unlockSessionIDPassed;
        int unlockCallCount = 0;

        getMockCatalog()->setSucceedingExpectedUnLock(
                [&unlockMutex, &unlockCV, &unlockCallCount, &unlockSessionIDPassed](
                        const OID& lockSessionID) {
            stdx::unique_lock<stdx::mutex> ul(unlockMutex);
            unlockCallCount++;
            unlockSessionIDPassed = lockSessionID;
            unlockCV.notify_all();
        }, Status::OK());

        {
            auto lockStatus = getMgr()->lock(lockName, whyMsg, Milliseconds(10), Milliseconds(1));
            ASSERT_NOT_OK(lockStatus.getStatus());
        }

        stdx::unique_lock<stdx::mutex> ul(unlockMutex);
        if (unlockCallCount == 0) {
            ASSERT(unlockCV.wait_for(ul, kUnlockTimeout) == stdx::cv_status::no_timeout);
        }

        ASSERT_EQUALS(1, unlockCallCount);
        ASSERT_EQUALS(lastTS, unlockSessionIDPassed);
    }

    TEST_F(ReplSetDistLockManagerFixture, LockBusyNoRetry) {
        getMockCatalog()->setSucceedingExpectedGrabLock([this](StringData lockID,
                                                               const OID& lockSessionID,
                                                               StringData who,
                                                               StringData processId,
                                                               Date_t time,
                                                               StringData why) {
            getMockCatalog()->expectNoGrabLock(); // Call only once.
        }, {ErrorCodes::LockStateChangeFailed, "nMod 0"});

        auto status = getMgr()->lock("", "", Milliseconds(0), Milliseconds(0)).getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }

    /**
     * Test scenario:
     * 1. Attempt to grab lock.
     * 2. Check that each attempt uses a unique lock session id.
     * 3. Times out trying.
     * 4. Checks result is error.
     * 5. Implicitly check that unlock is not called (default setting of mock catalog).
     */
    TEST_F(ReplSetDistLockManagerFixture, LockRetryTimeout) {
        string lockName("test");
        string me("me");
        OID lastTS;
        Date_t lastTime(Date_t::now());
        string whyMsg("because");

        int retryAttempt = 0;

        getMockCatalog()->setSucceedingExpectedGrabLock(
                [this,
                 &lockName,
                 &lastTS,
                 &me,
                 &lastTime,
                 &whyMsg,
                 &retryAttempt](
                        StringData lockID,
                        const OID& lockSessionID,
                        StringData who,
                        StringData processId,
                        Date_t time,
                        StringData why) {
            ASSERT_EQUALS(lockName, lockID);
            // Every attempt should have a unique sesssion ID.
            ASSERT_NOT_EQUALS(lastTS, lockSessionID);
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_GREATER_THAN_OR_EQUALS(time, lastTime);
            ASSERT_EQUALS(whyMsg, why);

            lastTS = lockSessionID;
            lastTime = time;
            retryAttempt++;
        }, {ErrorCodes::LockStateChangeFailed, "nMod 0"});

        auto lockStatus = getMgr()->lock(lockName,
                                         whyMsg,
                                         Milliseconds(5),
                                         Milliseconds(1)).getStatus();
        ASSERT_NOT_OK(lockStatus);

        ASSERT_EQUALS(ErrorCodes::LockBusy, lockStatus.code());
        ASSERT_GREATER_THAN(retryAttempt, 1);
    }

    /**
     * Test scenario:
     * 1. Set mock to error on grab lock.
     * 2. Grab lock attempted.
     * 3. Wait for unlock to be called.
     * 4. Check that lockSessionID used on all unlock is the same as the one used to grab lock.
     */
    TEST_F(ReplSetDistLockManagerFixture, MustUnlockOnLockError) {
        string lockName("test");
        string me("me");
        OID lastTS;
        string whyMsg("because");

        getMockCatalog()->setSucceedingExpectedGrabLock(
                [this,
                 &lockName,
                 &lastTS,
                 &me,
                 &whyMsg](
                        StringData lockID,
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
        }, {ErrorCodes::NetworkTimeout, "bad test network"});

        stdx::mutex unlockMutex;
        stdx::condition_variable unlockCV;
        int unlockCallCount = 0;
        OID unlockSessionIDPassed;

        getMockCatalog()->setSucceedingExpectedUnLock(
                [&unlockMutex, &unlockCV, &unlockCallCount, &unlockSessionIDPassed](
                        const OID& lockSessionID) {
            stdx::unique_lock<stdx::mutex> ul(unlockMutex);
            unlockCallCount++;
            unlockSessionIDPassed = lockSessionID;
            unlockCV.notify_all();
        }, Status::OK());

        auto lockStatus = getMgr()->lock(lockName,
                                         whyMsg,
                                         Milliseconds(10),
                                         Milliseconds(1)).getStatus();
        ASSERT_NOT_OK(lockStatus);
        ASSERT_EQUALS(ErrorCodes::NetworkTimeout, lockStatus.code());

        stdx::unique_lock<stdx::mutex> ul(unlockMutex);
        if (unlockCallCount == 0) {
            ASSERT(unlockCV.wait_for(ul, kUnlockTimeout) == stdx::cv_status::no_timeout);
        }

        ASSERT_EQUALS(1, unlockCallCount);
        ASSERT_EQUALS(lastTS, unlockSessionIDPassed);
    }

    /**
     * Test scenario:
     * 1. Ping thread started during setUp of fixture.
     * 2. Wait until ping was called at least 3 times.
     * 3. Check that correct process is being pinged.
     * 4. Check that ping values are unique (based on the assumption that the system
     *    clock supports 2ms granularity).
     */
    TEST_F(ReplSetDistLockManagerFixture, LockPinging) {
        stdx::mutex testMutex;
        stdx::condition_variable ping3TimesCV;
        vector<Date_t> pingValues;
        vector<string> processIDList;

        getMockCatalog()->setSucceedingExpectedPing(
                [&testMutex, &ping3TimesCV, &processIDList, &pingValues](
                        StringData processIDArg, Date_t ping) {
            stdx::lock_guard<stdx::mutex> sl(testMutex);
            processIDList.push_back(processIDArg.toString());
            pingValues.push_back(ping);

            if (processIDList.size() >= 3) {
                ping3TimesCV.notify_all();
            }
        }, Status::OK());

        {
            stdx::unique_lock<stdx::mutex> ul(testMutex);
            if (processIDList.size() < 3) {
                ASSERT_TRUE(ping3TimesCV.wait_for(ul, Milliseconds(50)) ==
                            stdx::cv_status::no_timeout);
            }
        }

        Date_t lastPing;
        for (const auto& ping : pingValues) {
            ASSERT_NOT_EQUALS(lastPing, ping);
            lastPing = ping;
        }

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
    TEST_F(ReplSetDistLockManagerFixture, UnlockUntilNoError) {
        stdx::mutex unlockMutex;
        stdx::condition_variable unlockCV;
        const unsigned int kUnlockErrorCount = 3;
        vector<OID> lockSessionIDPassed;

        getMockCatalog()->setSucceedingExpectedUnLock(
                [this, &unlockMutex, &unlockCV, &kUnlockErrorCount, &lockSessionIDPassed](
                        const OID& lockSessionID) {
            stdx::unique_lock<stdx::mutex> ul(unlockMutex);
            lockSessionIDPassed.push_back(lockSessionID);

            if (lockSessionIDPassed.size() >= kUnlockErrorCount) {
                getMockCatalog()->setSucceedingExpectedUnLock(
                        [&lockSessionIDPassed, &unlockMutex, &unlockCV](
                                const OID& lockSessionID) {
                    stdx::unique_lock<stdx::mutex> ul(unlockMutex);
                    lockSessionIDPassed.push_back(lockSessionID);
                    unlockCV.notify_all();
                }, Status::OK());
            }
        }, {ErrorCodes::NetworkTimeout, "bad test network"});

        OID lockSessionID;
        LocksType retLockDoc;
        retLockDoc.setName("test");
        retLockDoc.setState(LocksType::LOCKED);
        retLockDoc.setProcess(getProcessID());
        retLockDoc.setWho("me");
        retLockDoc.setWhy("why");
        // Will be different from the actual lock session id. For testing only.
        retLockDoc.setLockID(OID::gen());

        getMockCatalog()->setSucceedingExpectedGrabLock([&lockSessionID](
                StringData lockID,
                const OID& lockSessionIDArg,
                StringData who,
                StringData processId,
                Date_t time,
                StringData why) {
            lockSessionID = lockSessionIDArg;
        }, retLockDoc);

        {
            auto lockStatus = getMgr()->lock("test", "why", Milliseconds(0), Milliseconds(0));
        }

        stdx::unique_lock<stdx::mutex> ul(unlockMutex);
        if (lockSessionIDPassed.size() < kUnlockErrorCount) {
            ASSERT(unlockCV.wait_for(ul, kUnlockTimeout) == stdx::cv_status::no_timeout);
        }

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
    TEST_F(ReplSetDistLockManagerFixture, MultipleQueuedUnlock) {
        stdx::mutex testMutex;
        stdx::condition_variable unlockCV;

        vector<OID> lockSessionIDPassed;
        map<OID, int> unlockIDMap; // id -> count

        /**
         * Returns true if all values in the map are greater than 2.
         */
        auto mapEntriesGreaterThanTwo = [](const decltype(unlockIDMap)& map) -> bool {
            auto iter = find_if(map.begin(), map.end(),
                    [](const std::remove_reference<decltype(map)>::type::value_type& entry)
                        -> bool {
                return entry.second < 3;
            });

            return iter == map.end();
        };

        getMockCatalog()->setSucceedingExpectedUnLock(
                [this, &unlockIDMap, &testMutex, &unlockCV, &mapEntriesGreaterThanTwo](
                        const OID& lockSessionID) {
            stdx::unique_lock<stdx::mutex> ul(testMutex);
            unlockIDMap[lockSessionID]++;

            // Wait until we see at least 2 unique lockSessionID more than twice.
            if (unlockIDMap.size() >= 2 && mapEntriesGreaterThanTwo(unlockIDMap)) {
                getMockCatalog()->setSucceedingExpectedUnLock(
                        [&testMutex, &unlockCV](const OID& lockSessionID) {
                    stdx::unique_lock<stdx::mutex> ul(testMutex);
                    unlockCV.notify_all();
                }, Status::OK());
            }
        }, {ErrorCodes::NetworkTimeout, "bad test network"});

        LocksType retLockDoc;
        retLockDoc.setName("test");
        retLockDoc.setState(LocksType::LOCKED);
        retLockDoc.setProcess(getProcessID());
        retLockDoc.setWho("me");
        retLockDoc.setWhy("why");
        // Will be different from the actual lock session id. For testing only.
        retLockDoc.setLockID(OID::gen());

        getMockCatalog()->setSucceedingExpectedGrabLock([&testMutex, &lockSessionIDPassed](
                StringData lockID,
                const OID& lockSessionIDArg,
                StringData who,
                StringData processId,
                Date_t time,
                StringData why) {
            stdx::unique_lock<stdx::mutex> ul(testMutex);
            lockSessionIDPassed.push_back(lockSessionIDArg);
        }, retLockDoc);

        {
            auto lockStatus = getMgr()->lock("test", "why", Milliseconds(0), Milliseconds(0));
            auto otherStatus = getMgr()->lock("lock", "why", Milliseconds(0), Milliseconds(0));
        }

        stdx::unique_lock<stdx::mutex> ul(testMutex);
        ASSERT_EQUALS(2u, lockSessionIDPassed.size());

        if (unlockIDMap.size() < 2 || !mapEntriesGreaterThanTwo(unlockIDMap)) {
            ASSERT(unlockCV.wait_for(ul, kUnlockTimeout) == stdx::cv_status::no_timeout);
        }

        for (const auto& id : lockSessionIDPassed) {
            ASSERT_GREATER_THAN(unlockIDMap[id], 2)
                    << "lockIDList: " << vectorToString(lockSessionIDPassed)
                    << ", map: " << mapToString(unlockIDMap);
        }
    }

    TEST_F(ReplSetDistLockManagerFixture, CleanupPingOnShutdown) {
        bool stopPingCalled = false;
        getMockCatalog()->setSucceedingExpectedStopPing([this, & stopPingCalled](
                StringData processID) {
            ASSERT_EQUALS(getProcessID(), processID);
            stopPingCalled = true;
        }, Status::OK());

        getMgr()->shutDown();
        ASSERT_TRUE(stopPingCalled);
    }

    TEST_F(ReplSetDistLockManagerFixture, CheckLockStatusOK) {
        LocksType retLockDoc;
        retLockDoc.setName("test");
        retLockDoc.setState(LocksType::LOCKED);
        retLockDoc.setProcess(getProcessID());
        retLockDoc.setWho("me");
        retLockDoc.setWhy("why");
        // Will be different from the actual lock session id. For testing only.
        retLockDoc.setLockID(OID::gen());

        OID lockSessionID;

        getMockCatalog()->setSucceedingExpectedGrabLock([&lockSessionID]
                (StringData, const OID& ts, StringData, StringData, Date_t, StringData) {
            lockSessionID = ts;
        }, retLockDoc);


        auto lockStatus = getMgr()->lock("a", "", Milliseconds(0), Milliseconds(0));
        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->setSucceedingExpectedUnLock([](const OID&) {
            // Don't care
        }, Status::OK());

        auto& scopedLock = lockStatus.getValue();

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->setSucceedingExpectedGetLockByTS([&lockSessionID](const OID& ts) {
            ASSERT_EQUALS(lockSessionID, ts);
        }, retLockDoc);

        ASSERT_OK(scopedLock.checkStatus());
    }

    TEST_F(ReplSetDistLockManagerFixture, CheckLockStatusNoLongerOwn) {
        LocksType retLockDoc;
        retLockDoc.setName("test");
        retLockDoc.setState(LocksType::LOCKED);
        retLockDoc.setProcess(getProcessID());
        retLockDoc.setWho("me");
        retLockDoc.setWhy("why");
        // Will be different from the actual lock session id. For testing only.
        retLockDoc.setLockID(OID::gen());

        OID lockSessionID;

        getMockCatalog()->setSucceedingExpectedGrabLock([&lockSessionID]
                (StringData, const OID& ts, StringData, StringData, Date_t, StringData) {
            lockSessionID = ts;
        }, retLockDoc);


        auto lockStatus = getMgr()->lock("a", "", Milliseconds(0), Milliseconds(0));
        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->setSucceedingExpectedUnLock([](const OID&) {
            // Don't care
        }, Status::OK());

        auto& scopedLock = lockStatus.getValue();

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->setSucceedingExpectedGetLockByTS([&lockSessionID](const OID& ts) {
            ASSERT_EQUALS(lockSessionID, ts);
        }, {ErrorCodes::LockNotFound, "no lock"});

        ASSERT_NOT_OK(scopedLock.checkStatus());
    }

    TEST_F(ReplSetDistLockManagerFixture, CheckLockStatusError) {
        LocksType retLockDoc;
        retLockDoc.setName("test");
        retLockDoc.setState(LocksType::LOCKED);
        retLockDoc.setProcess(getProcessID());
        retLockDoc.setWho("me");
        retLockDoc.setWhy("why");
        // Will be different from the actual lock session id. For testing only.
        retLockDoc.setLockID(OID::gen());

        OID lockSessionID;

        getMockCatalog()->setSucceedingExpectedGrabLock([&lockSessionID]
                (StringData, const OID& ts, StringData, StringData, Date_t, StringData) {
            lockSessionID = ts;
        }, retLockDoc);


        auto lockStatus = getMgr()->lock("a", "", Milliseconds(0), Milliseconds(0));
        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->setSucceedingExpectedUnLock([](const OID&) {
            // Don't care
        }, Status::OK());

        auto& scopedLock = lockStatus.getValue();

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->setSucceedingExpectedGetLockByTS([&lockSessionID](const OID& ts) {
            ASSERT_EQUALS(lockSessionID, ts);
        }, {ErrorCodes::NetworkTimeout, "bad test network"});

        ASSERT_NOT_OK(scopedLock.checkStatus());
    }

} // unnamed namespace
} // namespace mongo
