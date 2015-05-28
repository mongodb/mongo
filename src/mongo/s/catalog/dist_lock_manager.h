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

#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/stdx/chrono.h"

namespace mongo {

    using DistLockHandle = OID;
    class Status;
    template <typename T> class StatusWith;

    /**
     * Interface for handling distributed locks.
     *
     * Usage:
     *
     * auto scopedDistLock = mgr->lock(...);
     *
     * if (!scopedDistLock.isOK()) {
     *   // Did not get lock. scopedLockStatus destructor will not call unlock.
     * }
     *
     * // To check if lock is still owned:
     * auto status = scopedDistLock.getValue().checkStatus();
     *
     * if (!status.isOK()) {
     *   // Someone took over the lock! Unlock will still be called at destructor, but will
     *   // practically be a no-op since it doesn't own the lock anymore.
     * }
     */
    class DistLockManager {
    public:

        static const stdx::chrono::milliseconds kDefaultSingleLockAttemptTimeout;
        static const stdx::chrono::milliseconds kDefaultLockRetryInterval;

        /**
         * RAII type for distributed lock. Not meant to be shared across multiple threads.
         */
        class ScopedDistLock {
            MONGO_DISALLOW_COPYING(ScopedDistLock);

        public:
            ScopedDistLock(); // TODO: SERVER-18007
            ScopedDistLock(DistLockHandle lockHandle, DistLockManager* lockManager);
            ~ScopedDistLock();

            ScopedDistLock(ScopedDistLock&& other);
            ScopedDistLock& operator=(ScopedDistLock&& other);

            /**
             * Checks whether the lock is still being held by querying the config server.
             */
            Status checkStatus();

        private:
            DistLockHandle _lockID;
            DistLockManager* _lockManager; // Not owned here.
        };

        virtual ~DistLockManager() = default;

        virtual void startUp() = 0;
        virtual void shutDown() = 0;

        /**
         * Tries multiple times to lock, using the specified lock try interval, until
         * a certain amount of time has passed or when any error that is not LockBusy
         * occurred.
         *
         * waitFor = 0 indicates there should only be one attempt to acquire the lock, and
         * no waiting.
         * waitFor = -1 indicates we should retry indefinitely.
         *
         * Returns OK if the lock was successfully acquired.
         * Returns ErrorCodes::DistributedClockSkewed when a clock skew is detected.
         * Returns ErrorCodes::LockBusy if the lock is being held.
         */
        virtual StatusWith<ScopedDistLock> lock(
                StringData name,
                StringData whyMessage,
                stdx::chrono::milliseconds waitFor = kDefaultSingleLockAttemptTimeout,
                stdx::chrono::milliseconds lockTryInterval = kDefaultLockRetryInterval) = 0;

    protected:

        /**
         * Unlocks the given lockHandle. Will attempt to retry again later if the config
         * server is not reachable.
         */
        virtual void unlock(const DistLockHandle& lockHandle) = 0;

        /**
         * Checks if the lockHandle still exists in the config server.
         */
        virtual Status checkStatus(const DistLockHandle& lockHandle) = 0;
    };
}
