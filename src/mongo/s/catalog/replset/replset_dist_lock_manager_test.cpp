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

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/replset/replset_dist_lock_manager.h"

#include <boost/optional.hpp>
#include <boost/optional/optional_io.hpp>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/dist_lock_catalog_mock.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_lockpings.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/tick_source_mock.h"
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

// Max duration to wait to satisfy test invariant before joining with main test thread.
const Seconds kJoinTimeout(30);
const Milliseconds kPingInterval(2);
const Seconds kLockExpiration(10);

/**
 * Basic fixture for ReplSetDistLockManager that starts it up before the test begins
 * and shuts it down when a test finishes.
 */
class ReplSetDistLockManagerFixture : public mongo::unittest::Test {
public:
    ReplSetDistLockManagerFixture()
        : _dummyDoNotUse(stdx::make_unique<DistLockCatalogMock>()),
          _mockCatalog(_dummyDoNotUse.get()),
          _processID("test") {
        setGlobalServiceContext(stdx::make_unique<ServiceContextNoop>());
        _mgr = stdx::make_unique<ReplSetDistLockManager>(getGlobalServiceContext(),
                                                         _processID,
                                                         std::move(_dummyDoNotUse),
                                                         kPingInterval,
                                                         kLockExpiration);
    }

    /**
     * Returns the lock manager instance that is being tested.
     */
    ReplSetDistLockManager* getMgr() {
        return _mgr.get();
    }

    /**
     * Returns the mocked catalog used by the lock manager being tested.
     */
    DistLockCatalogMock* getMockCatalog() {
        return _mockCatalog;
    }

    /**
     * Get the process id that was initialized with the lock manager being tested.
     */
    string getProcessID() const {
        return _processID;
    }

    OperationContext* txn() {
        return &_txn;
    }

protected:
    virtual std::unique_ptr<TickSource> makeTickSource() {
        return stdx::make_unique<SystemTickSource>();
    }

    void setUp() override {
        getGlobalServiceContext()->setTickSource(makeTickSource());

        // Set up grid just so that the config shard is accessible via the ShardRegistry.
        ConnectionString configCS = ConnectionString::forReplicaSet(
            "configReplSet", std::vector<HostAndPort>{HostAndPort{"config"}});

        auto targeterFactory = stdx::make_unique<RemoteCommandTargeterFactoryMock>();
        auto targeterFactoryPtr = targeterFactory.get();

        ShardFactory::BuilderCallable setBuilder =
            [targeterFactoryPtr](const ShardId& shardId, const ConnectionString& connStr) {
                return stdx::make_unique<ShardRemote>(
                    shardId, connStr, targeterFactoryPtr->create(connStr));
            };

        ShardFactory::BuilderCallable masterBuilder =
            [targeterFactoryPtr](const ShardId& shardId, const ConnectionString& connStr) {
                return stdx::make_unique<ShardRemote>(
                    shardId, connStr, targeterFactoryPtr->create(connStr));
            };

        ShardFactory::BuildersMap buildersMap{
            {ConnectionString::SET, std::move(setBuilder)},
            {ConnectionString::MASTER, std::move(masterBuilder)},
        };

        auto shardFactory =
            stdx::make_unique<ShardFactory>(std::move(buildersMap), std::move(targeterFactory));

        auto shardRegistry = stdx::make_unique<ShardRegistry>(std::move(shardFactory), configCS);
        grid.init(nullptr,
                  nullptr,
                  nullptr,
                  std::move(shardRegistry),
                  nullptr,
                  stdx::make_unique<BalancerConfiguration>(),
                  nullptr,
                  nullptr);

        _mgr->startUp();
    }

    void tearDown() override {
        // Don't care about what shutDown passes to stopPing here.
        _mockCatalog->expectStopPing([](StringData) {}, Status::OK());
        _mgr->shutDown(txn());
        grid.clearForUnitTests();
    }

    std::unique_ptr<DistLockCatalogMock> _dummyDoNotUse;  // dummy placeholder
    DistLockCatalogMock* _mockCatalog;
    string _processID;
    std::unique_ptr<ReplSetDistLockManager> _mgr;
    OperationContextNoop _txn;
};

class RSDistLockMgrWithMockTickSource : public ReplSetDistLockManagerFixture {
public:
    /**
     * Override the way the fixture gets the tick source to install to use a mock tick source.
     */
    std::unique_ptr<TickSource> makeTickSource() override {
        return stdx::make_unique<TickSourceMock>();
    }

    /**
     * Returns the mock tick source.
     */
    TickSourceMock* getMockTickSource() {
        return dynamic_cast<TickSourceMock*>(getGlobalServiceContext()->getTickSource());
    }
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
        auto lockStatus = getMgr()->lock(txn(),
                                         lockName,
                                         whyMsg,
                                         DistLockManager::kSingleLockAttemptTimeout,
                                         DistLockManager::kDefaultLockRetryInterval);
        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->expectUnLock(
            [&unlockCallCount, &unlockSessionIDPassed](const OID& lockSessionID) {
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
 * 1. Grab lock fails up to 3 times.
 * 2. Check that each subsequent attempt uses the same lock session id.
 * 3. Unlock (on destructor of ScopedDistLock).
 * 4. Check lock id used in lock and unlock are the same.
 */
TEST_F(RSDistLockMgrWithMockTickSource, LockSuccessAfterRetry) {
    string lockName("test");
    string me("me");
    boost::optional<OID> lastTS;
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
                ASSERT_EQUALS(lastTS, lockSessionID);
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
                            ASSERT_EQUALS(lastTS, lockSessionID);
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
        auto lockStatus =
            getMgr()->lock(txn(), lockName, whyMsg, Milliseconds(10), Milliseconds(1));
        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->expectUnLock(
            [&unlockCallCount, &unlockSessionIDPassed](const OID& lockSessionID) {
                unlockCallCount++;
                unlockSessionIDPassed = lockSessionID;
            },
            Status::OK());
    }

    ASSERT_EQUALS(1, unlockCallCount);
    ASSERT_EQUALS(lastTS, unlockSessionIDPassed);
}

/**
 * Test scenario:
 * 1. Grab lock fails up to 3 times.
 * 2. Check that each subsequent attempt uses the same lock session id.
 * 3. Grab lock errors out on the fourth try.
 * 4. Make sure that unlock is called to cleanup the last lock attempted that error out.
 */
TEST_F(RSDistLockMgrWithMockTickSource, LockFailsAfterRetry) {
    string lockName("test");
    string me("me");
    boost::optional<OID> lastTS;
    Date_t lastTime(Date_t::now());
    string whyMsg("because");

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
                ASSERT_EQUALS(lastTS, lockSessionID);
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
                            ASSERT_EQUALS(lastTS, lockSessionID);
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

    stdx::mutex unlockMutex;
    stdx::condition_variable unlockCV;
    OID unlockSessionIDPassed;
    int unlockCallCount = 0;

    getMockCatalog()->expectUnLock(
        [&unlockMutex, &unlockCV, &unlockCallCount, &unlockSessionIDPassed](
            const OID& lockSessionID) {
            stdx::unique_lock<stdx::mutex> lk(unlockMutex);
            unlockCallCount++;
            unlockSessionIDPassed = lockSessionID;
            unlockCV.notify_all();
        },
        Status::OK());

    {
        auto lockStatus =
            getMgr()->lock(txn(), lockName, whyMsg, Milliseconds(10), Milliseconds(1));
        ASSERT_NOT_OK(lockStatus.getStatus());
    }

    bool didTimeout = false;
    {
        stdx::unique_lock<stdx::mutex> lk(unlockMutex);
        if (unlockCallCount == 0) {
            didTimeout =
                unlockCV.wait_for(lk, kJoinTimeout.toSystemDuration()) == stdx::cv_status::timeout;
        }
    }

    // Join the background thread before trying to call asserts. Shutdown calls
    // stopPing and we don't care in this test.
    getMockCatalog()->expectStopPing([](StringData) {}, Status::OK());
    getMgr()->shutDown(txn());

    // No assert until shutDown has been called to make sure that the background thread
    // won't be trying to access the local variables that were captured by lamdas that
    // may have gone out of scope when the assert unwinds the stack.
    // No need to grab unlockMutex since there is only one thread running at this point.

    ASSERT_FALSE(didTimeout);
    ASSERT_EQUALS(1, unlockCallCount);
    ASSERT_EQUALS(lastTS, unlockSessionIDPassed);
}

TEST_F(ReplSetDistLockManagerFixture, LockBusyNoRetry) {
    getMockCatalog()->expectGrabLock(
        [this](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            getMockCatalog()->expectNoGrabLock();  // Call only once.
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    // Make mock return lock not found to skip lock overtaking.
    getMockCatalog()->expectGetLockByName([](StringData) {},
                                          {ErrorCodes::LockNotFound, "not found!"});

    auto status = getMgr()->lock(txn(), "", "", Milliseconds(0), Milliseconds(0)).getStatus();
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
TEST_F(RSDistLockMgrWithMockTickSource, LockRetryTimeout) {
    string lockName("test");
    string me("me");
    boost::optional<OID> lastTS;
    Date_t lastTime(Date_t::now());
    string whyMsg("because");

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
                ASSERT_EQUALS(lastTS, lockSessionID);
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

    auto lockStatus =
        getMgr()->lock(txn(), lockName, whyMsg, Milliseconds(5), Milliseconds(1)).getStatus();
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

    stdx::mutex unlockMutex;
    stdx::condition_variable unlockCV;
    int unlockCallCount = 0;
    OID unlockSessionIDPassed;

    getMockCatalog()->expectUnLock(
        [&unlockMutex, &unlockCV, &unlockCallCount, &unlockSessionIDPassed](
            const OID& lockSessionID) {
            stdx::unique_lock<stdx::mutex> lk(unlockMutex);
            unlockCallCount++;
            unlockSessionIDPassed = lockSessionID;
            unlockCV.notify_all();
        },
        Status::OK());

    auto lockStatus =
        getMgr()->lock(txn(), lockName, whyMsg, Milliseconds(10), Milliseconds(1)).getStatus();
    ASSERT_NOT_OK(lockStatus);
    ASSERT_EQUALS(ErrorCodes::ExceededMemoryLimit, lockStatus.code());

    bool didTimeout = false;
    {
        stdx::unique_lock<stdx::mutex> lk(unlockMutex);
        if (unlockCallCount == 0) {
            didTimeout =
                unlockCV.wait_for(lk, kJoinTimeout.toSystemDuration()) == stdx::cv_status::timeout;
        }
    }

    // Join the background thread before trying to call asserts. Shutdown calls
    // stopPing and we don't care in this test.
    getMockCatalog()->expectStopPing([](StringData) {}, Status::OK());
    getMgr()->shutDown(txn());

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
TEST_F(ReplSetDistLockManagerFixture, LockPinging) {
    stdx::mutex testMutex;
    stdx::condition_variable ping3TimesCV;
    vector<string> processIDList;

    getMockCatalog()->expectPing(
        [&testMutex, &ping3TimesCV, &processIDList](StringData processIDArg, Date_t ping) {
            stdx::lock_guard<stdx::mutex> lk(testMutex);
            processIDList.push_back(processIDArg.toString());

            if (processIDList.size() >= 3) {
                ping3TimesCV.notify_all();
            }
        },
        Status::OK());

    bool didTimeout = false;
    {
        stdx::unique_lock<stdx::mutex> lk(testMutex);
        if (processIDList.size() < 3) {
            didTimeout = ping3TimesCV.wait_for(lk, kJoinTimeout.toSystemDuration()) ==
                stdx::cv_status::timeout;
        }
    }

    // Join the background thread before trying to call asserts. Shutdown calls
    // stopPing and we don't care in this test.
    getMockCatalog()->expectStopPing([](StringData) {}, Status::OK());
    getMgr()->shutDown(txn());

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
TEST_F(ReplSetDistLockManagerFixture, UnlockUntilNoError) {
    stdx::mutex unlockMutex;
    stdx::condition_variable unlockCV;
    const unsigned int kUnlockErrorCount = 3;
    vector<OID> lockSessionIDPassed;

    getMockCatalog()->expectUnLock(
        [this, &unlockMutex, &unlockCV, &kUnlockErrorCount, &lockSessionIDPassed](
            const OID& lockSessionID) {
            stdx::unique_lock<stdx::mutex> lk(unlockMutex);
            lockSessionIDPassed.push_back(lockSessionID);

            if (lockSessionIDPassed.size() >= kUnlockErrorCount) {
                getMockCatalog()->expectUnLock(
                    [&lockSessionIDPassed, &unlockMutex, &unlockCV](const OID& lockSessionID) {
                        stdx::unique_lock<stdx::mutex> lk(unlockMutex);
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

    { auto lockStatus = getMgr()->lock(txn(), "test", "why", Milliseconds(0), Milliseconds(0)); }

    bool didTimeout = false;
    {
        stdx::unique_lock<stdx::mutex> lk(unlockMutex);
        if (lockSessionIDPassed.size() < kUnlockErrorCount) {
            didTimeout =
                unlockCV.wait_for(lk, kJoinTimeout.toSystemDuration()) == stdx::cv_status::timeout;
        }
    }

    // Join the background thread before trying to call asserts. Shutdown calls
    // stopPing and we don't care in this test.
    getMockCatalog()->expectStopPing([](StringData) {}, Status::OK());
    getMgr()->shutDown(txn());

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
TEST_F(ReplSetDistLockManagerFixture, MultipleQueuedUnlock) {
    stdx::mutex testMutex;
    stdx::condition_variable unlockCV;

    vector<OID> lockSessionIDPassed;
    map<OID, int> unlockIDMap;  // id -> count

    /**
     * Returns true if all values in the map are greater than 2.
     */
    auto mapEntriesGreaterThanTwo = [](const decltype(unlockIDMap)& map) -> bool {
        auto iter = find_if(
            map.begin(),
            map.end(),
            [](const std::remove_reference<decltype(map)>::type::value_type& entry) -> bool {
                return entry.second < 3;
            });

        return iter == map.end();
    };

    getMockCatalog()->expectUnLock(
        [this, &unlockIDMap, &testMutex, &unlockCV, &mapEntriesGreaterThanTwo](
            const OID& lockSessionID) {
            stdx::unique_lock<stdx::mutex> lk(testMutex);
            unlockIDMap[lockSessionID]++;

            // Wait until we see at least 2 unique lockSessionID more than twice.
            if (unlockIDMap.size() >= 2 && mapEntriesGreaterThanTwo(unlockIDMap)) {
                getMockCatalog()->expectUnLock(
                    [&testMutex, &unlockCV](const OID& lockSessionID) {
                        stdx::unique_lock<stdx::mutex> lk(testMutex);
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
        [&testMutex, &lockSessionIDPassed](StringData lockID,
                                           const OID& lockSessionIDArg,
                                           StringData who,
                                           StringData processId,
                                           Date_t time,
                                           StringData why) {
            stdx::unique_lock<stdx::mutex> lk(testMutex);
            lockSessionIDPassed.push_back(lockSessionIDArg);
        },
        retLockDoc);

    {
        auto lockStatus = getMgr()->lock(txn(), "test", "why", Milliseconds(0), Milliseconds(0));
        auto otherStatus = getMgr()->lock(txn(), "lock", "why", Milliseconds(0), Milliseconds(0));
    }

    bool didTimeout = false;
    {
        stdx::unique_lock<stdx::mutex> lk(testMutex);

        if (unlockIDMap.size() < 2 || !mapEntriesGreaterThanTwo(unlockIDMap)) {
            didTimeout =
                unlockCV.wait_for(lk, kJoinTimeout.toSystemDuration()) == stdx::cv_status::timeout;
        }
    }

    // Join the background thread before trying to call asserts. Shutdown calls
    // stopPing and we don't care in this test.
    getMockCatalog()->expectStopPing([](StringData) {}, Status::OK());
    getMgr()->shutDown(txn());

    // No assert until shutDown has been called to make sure that the background thread
    // won't be trying to access the local variables that were captured by lamdas that
    // may have gone out of scope when the assert unwinds the stack.
    // No need to grab testMutex since there is only one thread running at this point.

    ASSERT_FALSE(didTimeout);
    ASSERT_EQUALS(2u, lockSessionIDPassed.size());

    for (const auto& id : lockSessionIDPassed) {
        ASSERT_GREATER_THAN(unlockIDMap[id], 2)
            << "lockIDList: " << vectorToString(lockSessionIDPassed)
            << ", map: " << mapToString(unlockIDMap);
    }
}

TEST_F(ReplSetDistLockManagerFixture, CleanupPingOnShutdown) {
    bool stopPingCalled = false;
    getMockCatalog()->expectStopPing(
        [this, &stopPingCalled](StringData processID) {
            ASSERT_EQUALS(getProcessID(), processID);
            stopPingCalled = true;
        },
        Status::OK());

    getMgr()->shutDown(txn());
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

    getMockCatalog()->expectGrabLock(
        [&lockSessionID](StringData, const OID& ts, StringData, StringData, Date_t, StringData) {
            lockSessionID = ts;
        },
        retLockDoc);


    auto lockStatus = getMgr()->lock(txn(), "a", "", Milliseconds(0), Milliseconds(0));
    ASSERT_OK(lockStatus.getStatus());

    getMockCatalog()->expectNoGrabLock();
    getMockCatalog()->expectUnLock(
        [](const OID&) {
            // Don't care
        },
        Status::OK());

    auto& scopedLock = lockStatus.getValue();

    getMockCatalog()->expectNoGrabLock();
    getMockCatalog()->expectGetLockByTS(
        [&lockSessionID](const OID& ts) { ASSERT_EQUALS(lockSessionID, ts); }, retLockDoc);

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

    getMockCatalog()->expectGrabLock(
        [&lockSessionID](StringData, const OID& ts, StringData, StringData, Date_t, StringData) {
            lockSessionID = ts;
        },
        retLockDoc);


    auto lockStatus = getMgr()->lock(txn(), "a", "", Milliseconds(0), Milliseconds(0));
    ASSERT_OK(lockStatus.getStatus());

    getMockCatalog()->expectNoGrabLock();
    getMockCatalog()->expectUnLock(
        [](const OID&) {
            // Don't care
        },
        Status::OK());

    auto& scopedLock = lockStatus.getValue();

    getMockCatalog()->expectNoGrabLock();
    getMockCatalog()->expectGetLockByTS(
        [&lockSessionID](const OID& ts) { ASSERT_EQUALS(lockSessionID, ts); },
        {ErrorCodes::LockNotFound, "no lock"});

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

    getMockCatalog()->expectGrabLock(
        [&lockSessionID](StringData, const OID& ts, StringData, StringData, Date_t, StringData) {
            lockSessionID = ts;
        },
        retLockDoc);


    auto lockStatus = getMgr()->lock(txn(), "a", "", Milliseconds(0), Milliseconds(0));
    ASSERT_OK(lockStatus.getStatus());

    getMockCatalog()->expectNoGrabLock();
    getMockCatalog()->expectUnLock(
        [](const OID&) {
            // Don't care
        },
        Status::OK());

    auto& scopedLock = lockStatus.getValue();

    getMockCatalog()->expectNoGrabLock();
    getMockCatalog()->expectGetLockByTS(
        [&lockSessionID](const OID& ts) { ASSERT_EQUALS(lockSessionID, ts); },
        {ErrorCodes::NetworkTimeout, "bad test network"});

    ASSERT_NOT_OK(scopedLock.checkStatus());
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
TEST_F(ReplSetDistLockManagerFixture, LockOvertakingAfterLockExpiration) {
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
        auto status =
            getMgr()->lock(txn(), "bar", "", Milliseconds(0), Milliseconds(0)).getStatus();
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
        auto lockStatus = getMgr()->lock(txn(), "bar", "foo", Milliseconds(0), Milliseconds(0));

        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->expectUnLock(
            [&unlockCallCount, &unlockSessionIDPassed](const OID& lockSessionID) {
                unlockCallCount++;
                unlockSessionIDPassed = lockSessionID;
            },
            Status::OK());
    }

    ASSERT_EQUALS(1, unlockCallCount);
    ASSERT_EQUALS(lastTS, unlockSessionIDPassed);
}

/**
 * Test scenario:
 * 1. Attempt to grab lock with lockSessionID fails because lock is already owned.
 * 2. Then the the lock is overtaken because the lockSessionID matches the lock owner.
 */
TEST_F(ReplSetDistLockManagerFixture, LockOvertakingWithSessionID) {
    OID passedLockSessionID("5572007fda9e476582bf3716");

    LocksType currentLockDoc;
    currentLockDoc.setName("bar");
    currentLockDoc.setState(LocksType::LOCKED);
    currentLockDoc.setProcess("otherProcess");
    currentLockDoc.setLockID(passedLockSessionID);
    currentLockDoc.setWho("me");
    currentLockDoc.setWhy("why");

    getMockCatalog()->expectGrabLock(
        [&passedLockSessionID, &currentLockDoc](
            StringData, const OID& lockSessionID, StringData, StringData, Date_t, StringData) {
            ASSERT_EQUALS(passedLockSessionID, lockSessionID);
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                          currentLockDoc);

    LockpingsType pingDoc;
    pingDoc.setProcess("otherProcess");
    pingDoc.setPing(Date_t());

    getMockCatalog()->expectGetPing(
        [](StringData process) { ASSERT_EQUALS("otherProcess", process); }, pingDoc);

    getMockCatalog()->expectGetServerInfo([]() {}, DistLockCatalog::ServerInfo(Date_t(), OID()));

    getMockCatalog()->expectOvertakeLock(
        [this, &passedLockSessionID, &currentLockDoc](StringData lockID,
                                                      const OID& lockSessionID,
                                                      const OID& currentHolderTS,
                                                      StringData who,
                                                      StringData processId,
                                                      Date_t time,
                                                      StringData why) {
            ASSERT_EQUALS("bar", lockID);
            ASSERT_EQUALS(passedLockSessionID, lockSessionID);
            ASSERT_EQUALS(currentLockDoc.getLockID(), currentHolderTS);
            ASSERT_EQUALS(getProcessID(), processId);
            ASSERT_EQUALS("foo", why);
        },
        currentLockDoc);

    int unlockCallCount = 0;
    OID unlockSessionIDPassed;

    {
        auto lockStatus = getMgr()->lockWithSessionID(
            txn(), "bar", "foo", passedLockSessionID, Milliseconds(0), Milliseconds(0));

        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->expectUnLock(
            [&unlockCallCount, &unlockSessionIDPassed](const OID& lockSessionID) {
                unlockCallCount++;
                unlockSessionIDPassed = lockSessionID;
            },
            Status::OK());
    }

    ASSERT_EQUALS(1, unlockCallCount);
    ASSERT_EQUALS(passedLockSessionID, unlockSessionIDPassed);
}

TEST_F(ReplSetDistLockManagerFixture, CannotOvertakeIfExpirationHasNotElapsed) {
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
        auto status =
            getMgr()->lock(txn(), "bar", "", Milliseconds(0), Milliseconds(0)).getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }

    // Advance config server time to 1 millisecond before lock expiration.
    getMockCatalog()->expectGetServerInfo(
        []() {}, DistLockCatalog::ServerInfo(Date_t() + kLockExpiration - Milliseconds(1), OID()));

    // Second attempt should still not overtake lock.
    {
        auto status =
            getMgr()->lock(txn(), "bar", "", Milliseconds(0), Milliseconds(0)).getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }
}

TEST_F(ReplSetDistLockManagerFixture, GetPingErrorWhileOvertaking) {
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

    auto status = getMgr()->lock(txn(), "bar", "", Milliseconds(0), Milliseconds(0)).getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::NetworkTimeout, status.code());
}

TEST_F(ReplSetDistLockManagerFixture, GetInvalidPingDocumentWhileOvertaking) {
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

    auto status = getMgr()->lock(txn(), "bar", "", Milliseconds(0), Milliseconds(0)).getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, status.code());
}

TEST_F(ReplSetDistLockManagerFixture, GetServerInfoErrorWhileOvertaking) {
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

    auto status = getMgr()->lock(txn(), "bar", "", Milliseconds(0), Milliseconds(0)).getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::NetworkTimeout, status.code());
}

TEST_F(ReplSetDistLockManagerFixture, GetLockErrorWhileOvertaking) {
    getMockCatalog()->expectGrabLock(
        [](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Don't care
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                          {ErrorCodes::NetworkTimeout, "bad test network"});

    auto status = getMgr()->lock(txn(), "bar", "", Milliseconds(0), Milliseconds(0)).getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::NetworkTimeout, status.code());
}

TEST_F(ReplSetDistLockManagerFixture, GetLockDisappearedWhileOvertaking) {
    getMockCatalog()->expectGrabLock(
        [](StringData, const OID&, StringData, StringData, Date_t, StringData) {
            // Don't care
        },
        {ErrorCodes::LockStateChangeFailed, "nMod 0"});

    getMockCatalog()->expectGetLockByName([](StringData name) { ASSERT_EQUALS("bar", name); },
                                          {ErrorCodes::LockNotFound, "disappeared!"});

    auto status = getMgr()->lock(txn(), "bar", "", Milliseconds(0), Milliseconds(0)).getStatus();
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
TEST_F(ReplSetDistLockManagerFixture, CannotOvertakeIfPingIsActive) {
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

        auto status =
            getMgr()->lock(txn(), "bar", "", Milliseconds(0), Milliseconds(0)).getStatus();
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
        auto lockStatus = getMgr()->lock(txn(), "bar", "foo", Milliseconds(0), Milliseconds(0));

        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->expectUnLock(
            [&unlockCallCount, &unlockSessionIDPassed](const OID& lockSessionID) {
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
TEST_F(ReplSetDistLockManagerFixture, CannotOvertakeIfOwnerJustChanged) {
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

        auto status =
            getMgr()->lock(txn(), "bar", "", Milliseconds(0), Milliseconds(0)).getStatus();
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
        auto lockStatus = getMgr()->lock(txn(), "bar", "foo", Milliseconds(0), Milliseconds(0));

        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->expectUnLock(
            [&unlockCallCount, &unlockSessionIDPassed](const OID& lockSessionID) {
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
TEST_F(ReplSetDistLockManagerFixture, CannotOvertakeIfElectionIdChanged) {
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

        auto status =
            getMgr()->lock(txn(), "bar", "", Milliseconds(0), Milliseconds(0)).getStatus();
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
        auto lockStatus = getMgr()->lock(txn(), "bar", "foo", Milliseconds(0), Milliseconds(0));

        ASSERT_OK(lockStatus.getStatus());

        getMockCatalog()->expectNoGrabLock();
        getMockCatalog()->expectUnLock(
            [&unlockCallCount, &unlockSessionIDPassed](const OID& lockSessionID) {
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
TEST_F(ReplSetDistLockManagerFixture, LockOvertakingResultsInError) {
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
        auto status =
            getMgr()->lock(txn(), "bar", "", Milliseconds(0), Milliseconds(0)).getStatus();
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

    stdx::mutex unlockMutex;
    stdx::condition_variable unlockCV;
    getMockCatalog()->expectUnLock(
        [&unlockSessionIDPassed, &unlockMutex, &unlockCV](const OID& lockSessionID) {
            stdx::unique_lock<stdx::mutex> lk(unlockMutex);
            unlockSessionIDPassed = lockSessionID;
            unlockCV.notify_all();
        },
        Status::OK());

    // Second attempt should overtake lock.
    auto lockStatus = getMgr()->lock(txn(), "bar", "foo", Milliseconds(0), Milliseconds(0));

    ASSERT_NOT_OK(lockStatus.getStatus());

    bool didTimeout = false;
    {
        stdx::unique_lock<stdx::mutex> lk(unlockMutex);
        if (!unlockSessionIDPassed.isSet()) {
            didTimeout =
                unlockCV.wait_for(lk, kJoinTimeout.toSystemDuration()) == stdx::cv_status::timeout;
        }
    }

    // Join the background thread before trying to call asserts. Shutdown calls
    // stopPing and we don't care in this test.
    getMockCatalog()->expectStopPing([](StringData) {}, Status::OK());
    getMgr()->shutDown(txn());

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
TEST_F(ReplSetDistLockManagerFixture, LockOvertakingFailed) {
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
        auto status =
            getMgr()->lock(txn(), "bar", "", Milliseconds(0), Milliseconds(0)).getStatus();
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
        auto status =
            getMgr()->lock(txn(), "bar", "foo", Milliseconds(0), Milliseconds(0)).getStatus();
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
TEST_F(ReplSetDistLockManagerFixture, CannotOvertakeIfConfigServerClockGoesBackwards) {
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
        auto status =
            getMgr()->lock(txn(), "bar", "", Milliseconds(0), Milliseconds(0)).getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }

    // Make config server time go backwards by lock expiration duration.
    getMockCatalog()->expectGetServerInfo(
        []() {},
        DistLockCatalog::ServerInfo(configClock - kLockExpiration - Milliseconds(1), OID()));

    // Second attempt should not overtake lock.
    {
        auto status =
            getMgr()->lock(txn(), "bar", "foo", Milliseconds(0), Milliseconds(0)).getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
    }
}

TEST_F(ReplSetDistLockManagerFixture, LockAcquisitionRetriesOnNetworkErrorSuccess) {
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

    getMockCatalog()->expectUnLock([&](const OID& lockSessionID) {}, Status::OK());

    auto status = getMgr()
                      ->lock(txn(), "LockName", "Lock reason", Milliseconds(0), Milliseconds(0))
                      .getStatus();
    ASSERT_OK(status);
}

TEST_F(ReplSetDistLockManagerFixture, LockAcquisitionRetriesOnInterruptionNeverSucceeds) {
    getMockCatalog()->expectGrabLock(
        [&](StringData, const OID&, StringData, StringData, Date_t, StringData) {},
        {ErrorCodes::Interrupted, "operation interrupted"});

    getMockCatalog()->expectUnLock([&](const OID& lockSessionID) {}, Status::OK());

    auto status = getMgr()->lock(txn(), "bar", "foo", Milliseconds(0), Milliseconds(0)).getStatus();
    ASSERT_NOT_OK(status);
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
TEST_F(RSDistLockMgrWithMockTickSource, CanOvertakeIfNoPingDocument) {
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
        auto status =
            getMgr()->lock(txn(), "bar", "", Milliseconds(0), Milliseconds(0)).getStatus();
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
        [](const OID&) {
            // Don't care
        },
        Status::OK());

    // Second attempt should overtake lock.
    {
        ASSERT_OK(
            getMgr()->lock(txn(), "bar", "foo", Milliseconds(0), Milliseconds(0)).getStatus());
    }
}

}  // unnamed namespace
}  // namespace mongo
