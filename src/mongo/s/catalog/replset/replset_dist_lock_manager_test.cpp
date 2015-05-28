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

#include <string>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/json.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/catalog/dist_lock_catalog_mock.h"
#include "mongo/s/type_lockpings.h"
#include "mongo/s/type_locks.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

    using std::string;

    const Milliseconds kUnlockTimeout(100);

    /**
     * Test scenario:
     * 1. Grab lock.
     * 2. Unlock (on destructor of ScopedDistLock).
     * 3. Check lock id used in lock and unlock are the same.
     */
    TEST(ReplSetDistLockManager, BasicLockLifeCycle) {
        string lockName("test");
        string processID("abcd");
        Date_t now(Date_t::now());
        string whyMsg("because");

        LocksType retLockDoc;
        retLockDoc.setName(lockName);
        retLockDoc.setState(LocksType::LOCKED);
        retLockDoc.setProcess(processID);
        retLockDoc.setWho("me");
        retLockDoc.setWhy(whyMsg);
        // Will be different from the actual lock session id. For testing only.
        retLockDoc.setLockID(OID::gen());

        auto mock = stdx::make_unique<DistLockCatalogMock>();
        auto rawMock = mock.get();

        OID lockSessionIDPassed;

        mock->setSucceedingExpectedGrabLock(
                [&lockName, &processID, &now, &whyMsg, &lockSessionIDPassed, &rawMock](
                        StringData lockID,
                        const OID& lockSessionID,
                        StringData who,
                        StringData processId,
                        Date_t time,
                        StringData why) {
            ASSERT_EQUALS(lockName, lockID);
            ASSERT_TRUE(lockSessionID.isSet());
            ASSERT_EQUALS(processID, processId);
            ASSERT_GREATER_THAN_OR_EQUALS(time, now);
            ASSERT_EQUALS(whyMsg, why);

            lockSessionIDPassed = lockSessionID;
            rawMock->expectNoGrabLock(); // Call only once.
        }, retLockDoc);

        ReplSetDistLockManager mgr(processID, std::move(mock));

        int unlockCallCount = 0;
        OID unlockSessionIDPassed;

        {
            auto lockStatus = mgr.lock(lockName,
                                       whyMsg,
                                       DistLockManager::kDefaultSingleLockAttemptTimeout,
                                       DistLockManager::kDefaultLockRetryInterval);
            ASSERT_OK(lockStatus.getStatus());

            rawMock->expectNoGrabLock();
            rawMock->setSucceedingExpectedUnLock([&unlockCallCount, &unlockSessionIDPassed](
                            const OID& lockSessionID) {
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
    TEST(ReplSetDistLockManager, LockSuccessAfterRetry) {
        string lockName("test");
        string me("me");
        string processID("abcd");
        OID lastTS;
        Date_t lastTime(Date_t::now());
        string whyMsg("because");

        int retryAttempt = 0;
        const int kMaxRetryAttempt = 3;
        LocksType invalidLockDoc;

        LocksType goodLockDoc;
        goodLockDoc.setName(lockName);
        goodLockDoc.setState(LocksType::LOCKED);
        goodLockDoc.setProcess(processID);
        goodLockDoc.setWho("me");
        goodLockDoc.setWhy(whyMsg);
        goodLockDoc.setLockID(OID::gen());

        auto mock = stdx::make_unique<DistLockCatalogMock>();
        auto rawMock = mock.get();
        mock->setSucceedingExpectedGrabLock(
                [&lockName,
                 &lastTS,
                 &me,
                 &processID,
                 &lastTime,
                 &whyMsg,
                 &retryAttempt,
                 &kMaxRetryAttempt,
                 &rawMock,
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
            ASSERT_EQUALS(processID, processId);
            ASSERT_GREATER_THAN_OR_EQUALS(time, lastTime);
            ASSERT_EQUALS(whyMsg, why);

            lastTS = lockSessionID;
            lastTime = time;

            if (++retryAttempt >= kMaxRetryAttempt) {
                rawMock->setSucceedingExpectedGrabLock([&lockName,
                                                        &lastTS,
                                                        &me,
                                                        &processID,
                                                        &lastTime,
                                                        &whyMsg,
                                                        &retryAttempt,
                                                        &rawMock](
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
                    ASSERT_EQUALS(processID, processId);
                    ASSERT_GREATER_THAN_OR_EQUALS(time, lastTime);
                    ASSERT_EQUALS(whyMsg, why);
                }, goodLockDoc);
            }
        }, invalidLockDoc);

        ReplSetDistLockManager mgr(processID, std::move(mock));

        int unlockCallCount = 0;
        OID unlockSessionIDPassed;

        {
            auto lockStatus = mgr.lock(lockName, whyMsg, Milliseconds(10), Milliseconds(1));
            ASSERT_OK(lockStatus.getStatus());

            rawMock->expectNoGrabLock();
            rawMock->setSucceedingExpectedUnLock([&unlockCallCount, &unlockSessionIDPassed](
                            const OID& lockSessionID) {
                unlockCallCount++;
                unlockSessionIDPassed = lockSessionID;
            }, Status::OK());
        }

        ASSERT_EQUALS(1, unlockCallCount);
        ASSERT_EQUALS(lastTS, unlockSessionIDPassed);
    }

    TEST(ReplSetDistLockManager, LockBusyNoRetry) {
        int retryAttempt = 0;
        LocksType invalidLockDoc;

        auto mock = stdx::make_unique<DistLockCatalogMock>();
        mock->setSucceedingExpectedGrabLock([&retryAttempt](StringData lockID,
                                                            const OID& lockSessionID,
                                                            StringData who,
                                                            StringData processId,
                                                            Date_t time,
                                                            StringData why) {
            retryAttempt++;
        }, invalidLockDoc);

        ReplSetDistLockManager mgr("", std::move(mock));
        auto status = mgr.lock("", "", Milliseconds(0), Milliseconds(0)).getStatus();
        ASSERT_NOT_OK(status);
        ASSERT_EQUALS(ErrorCodes::LockBusy, status.code());
        ASSERT_EQUALS(1, retryAttempt);
    }

    /**
     * Test scenario:
     * 1. Attempt to grab lock.
     * 2. Check that each attempt uses a unique lock session id.
     * 3. Times out trying.
     * 4. Checks result is error.
     * 5. Implicitly check that unlock is not called (default setting of mock catalog).
     */
    TEST(ReplSetDistLockManager, LockRetryTimeout) {
        string lockName("test");
        string me("me");
        string processID("abcd");
        OID lastTS;
        Date_t lastTime(Date_t::now());
        string whyMsg("because");

        int retryAttempt = 0;
        LocksType invalidLockDoc;

        auto mock = stdx::make_unique<DistLockCatalogMock>();
        mock->setSucceedingExpectedGrabLock(
                [&lockName,
                 &lastTS,
                 &me,
                 &processID,
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
            ASSERT_EQUALS(processID, processId);
            ASSERT_GREATER_THAN_OR_EQUALS(time, lastTime);
            ASSERT_EQUALS(whyMsg, why);

            lastTS = lockSessionID;
            lastTime = time;
            retryAttempt++;
        }, invalidLockDoc);

        ReplSetDistLockManager mgr(processID, std::move(mock));
        auto lockStatus = mgr.lock(lockName, whyMsg, Milliseconds(5), Milliseconds(1)).getStatus();
        ASSERT_NOT_OK(lockStatus);

        ASSERT_EQUALS(ErrorCodes::LockBusy, lockStatus.code());
        ASSERT_GREATER_THAN(retryAttempt, 1);
    }

} // unnamed namespace
} // namespace mongo
