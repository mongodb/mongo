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
#include "mongo/s/type_lockpings.h"
#include "mongo/s/type_locks.h"
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
     * mock.setSucceedingExpectedGrabLock([](StringData lockID,
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
     * mock.setSucceedingExpectedGrabLock([&mock](...) {
     *   mock.expectNoGrabLock();
     * }, Status::OK());
     */
    class DistLockCatalogMock : public DistLockCatalog {
    public:
        DistLockCatalogMock();
        virtual ~DistLockCatalogMock();

        using GrabLockFunc = stdx::function<void (StringData lockID,
                                                  const OID& lockSessionID,
                                                  StringData who,
                                                  StringData processId,
                                                  Date_t time,
                                                  StringData why)>;
        using UnlockFunc = stdx::function<void (const OID& lockSessionID)>;

        virtual StatusWith<LockpingsType> getPing(StringData processID) override;

        virtual Status ping(StringData processID, Date_t ping) override;

        virtual StatusWith<LocksType> grabLock(StringData lockID,
                                               const OID& lockSessionID,
                                               StringData who,
                                               StringData processId,
                                               Date_t time,
                                               StringData why) override;

        virtual StatusWith<LocksType> overtakeLock(StringData lockID,
                                                   const OID& lockSessionID,
                                                   const OID& lockTS,
                                                   StringData who,
                                                   StringData processId,
                                                   Date_t time,
                                                   StringData why) override;

        virtual Status unlock(const OID& lockSessionID) override;

        virtual StatusWith<ServerInfo> getServerInfo() override;

        virtual StatusWith<LocksType> getLockByTS(const OID& ts) override;

        /**
         * Sets the checker method to use and the return value for grabLock to return every
         * time it is called.
         */
        void setSucceedingExpectedGrabLock(GrabLockFunc checkerFunc,
                                           StatusWith<LocksType> returnThis);

        /**
         * Expect grabLock to never be called after this is called.
         */
        void expectNoGrabLock();

        /**
         * Sets the checker method to use and the return value for unlock to return every
         * time it is called.
         */
        void setSucceedingExpectedUnLock(UnlockFunc checkerFunc, Status returnThis);

    private:
        // Protects all the member variables.
        stdx::mutex _mutex;

        GrabLockFunc _grabLockChecker;
        StatusWith<LocksType> _grabLockReturnValue;

        UnlockFunc _unlockChecker;
        Status _unlockReturnValue;
    };
}
