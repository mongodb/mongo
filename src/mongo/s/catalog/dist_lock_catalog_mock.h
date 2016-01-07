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

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/s/catalog/dist_lock_catalog.h"
#include "mongo/s/catalog/type_lockpings.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

/**
 * Mock implementation of DistLockCatalog for testing.
 *
 * Example usage:
 *
 * DistLockCatalogMock mock;
 * LocksType badLock;
 * mock.expectGrabLock([](StringData lockID,
 *                                       const OID& lockSessionID,
 *                                       StringData who,
 *                                       StringData processId,
 *                                       Date_t time,
 *                                       StringData why) {
 *   ASSERT_EQUALS("test", lockID);
 * }, badLock);
 *
 * mock.grabLock("test", OID(), "me", "x", Date_t::now(), "end");
 *
 * It is also possible to chain the callbacks. For example, if we want to set the test
 * such that grabLock can only be called once, you can do this:
 *
 * DistLockCatalogMock mock;
 * mock.expectGrabLock([&mock](...) {
 *   mock.expectNoGrabLock();
 * }, Status::OK());
 */
class DistLockCatalogMock : public DistLockCatalog {
public:
    DistLockCatalogMock();
    virtual ~DistLockCatalogMock();

    using GrabLockFunc = stdx::function<void(StringData lockID,
                                             const OID& lockSessionID,
                                             StringData who,
                                             StringData processId,
                                             Date_t time,
                                             StringData why)>;
    using OvertakeLockFunc = stdx::function<void(StringData lockID,
                                                 const OID& lockSessionID,
                                                 const OID& currentHolderTS,
                                                 StringData who,
                                                 StringData processId,
                                                 Date_t time,
                                                 StringData why)>;
    using UnlockFunc = stdx::function<void(const OID& lockSessionID)>;
    using PingFunc = stdx::function<void(StringData processID, Date_t ping)>;
    using StopPingFunc = stdx::function<void(StringData processID)>;
    using GetPingFunc = StopPingFunc;
    using GetLockByTSFunc = stdx::function<void(const OID& ts)>;
    using GetLockByNameFunc = stdx::function<void(StringData name)>;
    using GetServerInfoFunc = stdx::function<void()>;

    virtual StatusWith<LockpingsType> getPing(OperationContext* txn, StringData processID) override;

    virtual Status ping(OperationContext* txn, StringData processID, Date_t ping) override;

    virtual StatusWith<LocksType> grabLock(OperationContext* txn,
                                           StringData lockID,
                                           const OID& lockSessionID,
                                           StringData who,
                                           StringData processId,
                                           Date_t time,
                                           StringData why) override;

    virtual StatusWith<LocksType> overtakeLock(OperationContext* txn,
                                               StringData lockID,
                                               const OID& lockSessionID,
                                               const OID& currentHolderTS,
                                               StringData who,
                                               StringData processId,
                                               Date_t time,
                                               StringData why) override;

    virtual Status unlock(OperationContext* txn, const OID& lockSessionID) override;

    virtual Status unlockAll(OperationContext* txn, const std::string& processID) override;

    virtual StatusWith<ServerInfo> getServerInfo(OperationContext* txn) override;

    virtual StatusWith<LocksType> getLockByTS(OperationContext* txn,
                                              const OID& lockSessionID) override;

    virtual StatusWith<LocksType> getLockByName(OperationContext* txn, StringData name) override;

    virtual Status stopPing(OperationContext* txn, StringData processId) override;

    /**
     * Sets the checker method to use and the return value for grabLock to return every
     * time it is called.
     */
    void expectGrabLock(GrabLockFunc checkerFunc, StatusWith<LocksType> returnThis);

    /**
     * Expect grabLock to never be called after this is called.
     */
    void expectNoGrabLock();

    /**
     * Sets the checker method to use and the return value for unlock to return every
     * time it is called.
     */
    void expectUnLock(UnlockFunc checkerFunc, Status returnThis);

    /**
     * Sets the checker method to use and its return value the every time ping is called.
     */
    void expectPing(PingFunc checkerFunc, Status returnThis);

    /**
     * Sets the checker method to use and its return value the every time stopPing is called.
     */
    void expectStopPing(StopPingFunc checkerFunc, Status returnThis);

    /**
     * Sets the checker method to use and its return value the every time
     * getLockByTS is called.
     */
    void expectGetLockByTS(GetLockByTSFunc checkerFunc, StatusWith<LocksType> returnThis);

    /**
     * Sets the checker method to use and its return value the every time
     * getLockByName is called.
     */
    void expectGetLockByName(GetLockByNameFunc checkerFunc, StatusWith<LocksType> returnThis);

    /**
     * Sets the checker method to use and its return value the every time
     * overtakeLock is called.
     */
    void expectOvertakeLock(OvertakeLockFunc checkerFunc, StatusWith<LocksType> returnThis);

    /**
     * Sets the checker method to use and its return value the every time
     * getPing is called.
     */
    void expectGetPing(GetPingFunc checkerFunc, StatusWith<LockpingsType> returnThis);

    /**
     * Sets the checker method to use and its return value the every time
     * getServerInfo is called.
     */
    void expectGetServerInfo(GetServerInfoFunc checkerFunc,
                             StatusWith<DistLockCatalog::ServerInfo> returnThis);

private:
    // Protects all the member variables.
    stdx::mutex _mutex;

    GrabLockFunc _grabLockChecker;
    StatusWith<LocksType> _grabLockReturnValue;

    UnlockFunc _unlockChecker;
    Status _unlockReturnValue;

    PingFunc _pingChecker;
    Status _pingReturnValue;

    StopPingFunc _stopPingChecker;
    Status _stopPingReturnValue;

    GetLockByTSFunc _getLockByTSChecker;
    StatusWith<LocksType> _getLockByTSReturnValue;

    GetLockByNameFunc _getLockByNameChecker;
    StatusWith<LocksType> _getLockByNameReturnValue;

    OvertakeLockFunc _overtakeLockChecker;
    StatusWith<LocksType> _overtakeLockReturnValue;

    GetPingFunc _getPingChecker;
    StatusWith<LockpingsType> _getPingReturnValue;

    GetServerInfoFunc _getServerInfoChecker;
    StatusWith<DistLockCatalog::ServerInfo> _getServerInfoReturnValue;
};
}
