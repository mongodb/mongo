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

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

#include "mongo/base/string_data.h"
#include "mongo/s/catalog/dist_lock_catalog.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/dist_lock_ping_info.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"

namespace mongo {

class ServiceContext;

class ReplSetDistLockManager final : public DistLockManager {
public:
    // How frequently should the dist lock pinger thread run and write liveness information about
    // this instance of the dist lock manager
    static const Seconds kDistLockPingInterval;

    // How long should the lease on a distributed lock last
    static const Minutes kDistLockExpirationTime;

    ReplSetDistLockManager(ServiceContext* globalContext,
                           StringData processID,
                           std::unique_ptr<DistLockCatalog> catalog,
                           Milliseconds pingInterval,
                           Milliseconds lockExpiration);

    virtual ~ReplSetDistLockManager();

    virtual void startUp() override;
    virtual void shutDown(OperationContext* txn) override;

    virtual std::string getProcessID() override;

    virtual StatusWith<ScopedDistLock> lock(OperationContext* txn,
                                            StringData name,
                                            StringData whyMessage,
                                            Milliseconds waitFor,
                                            Milliseconds lockTryInterval) override;

    virtual StatusWith<ScopedDistLock> lockWithSessionID(OperationContext* txn,
                                                         StringData name,
                                                         StringData whyMessage,
                                                         const OID lockSessionID,
                                                         Milliseconds waitFor,
                                                         Milliseconds lockTryInterval) override;

    virtual void unlockAll(OperationContext* txn, const std::string& processID) override;

protected:
    virtual void unlock(OperationContext* txn, const DistLockHandle& lockSessionID) override;

    virtual Status checkStatus(OperationContext* txn, const DistLockHandle& lockSessionID) override;

private:
    /**
     * Queue a lock to be unlocked asynchronously with retry until it doesn't error.
     */
    void queueUnlock(const DistLockHandle& lockSessionID);

    /**
     * Periodically pings and checks if there are locks queued that needs unlocking.
     */
    void doTask();

    /**
     * Returns true if shutDown was called.
     */
    bool isShutDown();

    /**
     * Returns true if the current process that owns the lock has no fresh pings since
     * the lock expiration threshold.
     */
    StatusWith<bool> isLockExpired(OperationContext* txn,
                                   const LocksType lockDoc,
                                   const Milliseconds& lockExpiration);

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (F) Self synchronizing.
    // (M) Must hold _mutex for access.
    // (I) Immutable, no synchronization needed.
    // (S) Can only be called inside startUp/shutDown.
    //

    ServiceContext* const _serviceContext;  // (F)

    const std::string _processID;                     // (I)
    const std::unique_ptr<DistLockCatalog> _catalog;  // (I)
    const Milliseconds _pingInterval;                 // (I)
    const Milliseconds _lockExpiration;               // (I)

    stdx::mutex _mutex;
    std::unique_ptr<stdx::thread> _execThread;  // (S)

    // Contains the list of locks queued for unlocking. Cases when unlock operation can
    // be queued include:
    // 1. First attempt on unlocking resulted in an error.
    // 2. Attempting to grab or overtake a lock resulted in an error where we are uncertain
    //    whether the modification was actually applied or not, and call unlock to make
    //    sure that it was cleaned up.
    std::deque<DistLockHandle> _unlockList;  // (M)

    bool _isShutDown = false;              // (M)
    stdx::condition_variable _shutDownCV;  // (M)

    // Map of lockName to last ping information.
    std::unordered_map<std::string, DistLockPingInfo> _pingHistory;  // (M)
};
}
