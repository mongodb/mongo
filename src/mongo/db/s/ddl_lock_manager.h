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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"

namespace mongo {

/**
 * Service to manage DDL locks.
 */
class DDLLockManager {
public:
    // Default timeout which will be used if one is not passed to the lock method.
    static const Minutes kDefaultLockTimeout;

    // Timeout value, which specifies that if the lock is not available immediately, no attempt
    // should be made to wait for it to become free.
    static const Milliseconds kSingleLockAttemptTimeout;

    /**
     * RAII type for the DDL lock.
     */
    class ScopedLock {
        ScopedLock(const ScopedLock&) = delete;
        ScopedLock& operator=(const ScopedLock&) = delete;

    public:
        ScopedLock(StringData lockName, StringData reason, DDLLockManager* lockManager);
        ~ScopedLock();

        ScopedLock(ScopedLock&& other);

        StringData getNs() {
            return _ns;
        }
        StringData getReason() {
            return _reason;
        }

    private:
        std::string _ns;
        std::string _reason;
        DDLLockManager* _lockManager;
    };

    DDLLockManager() = default;
    ~DDLLockManager() = default;

    /**
     * Retrieves the DDLLockManager singleton.
     */
    static DDLLockManager* get(ServiceContext* service);
    static DDLLockManager* get(OperationContext* opCtx);

    /**
     * Returns a RAII style lock on the given namespace @ns.
     *
     * @ns		Namespace to lock (both database and collections).
     * @reason 	Reson for which the lock is being acquired (e.g. 'createCollection').
     * @timeout Time after which this acquisition attempt will give up in case of lock contention.
     * 			A timeout value of -1 means the acquisition will be retried forever.
     *
     * Throws ErrorCodes::LockBusy in case the timeout is reached.
     * Throws ErrorCategory::Interruption in case the opeartion context is interrupted.
     * Throws ErrorCodes::LockTimeout when not being on kPrimaryAndRecovered state and timeout is
     * reached (except if the caller is a DDL coordinator)
     *
     * Note that `lock` can only be called from a replica set primary node, however, the caller has
     * the responsibility to release the acquired locks on step-downs
     */
    ScopedLock lock(OperationContext* opCtx,
                    StringData ns,
                    StringData reason,
                    Milliseconds timeout) {
        return lock(opCtx, ns, reason, timeout, true /*waitForRecovery*/);
    }

protected:
    struct NSLock {
        NSLock(StringData reason) : reason(reason.toString()) {}

        stdx::condition_variable cvLocked;
        int numWaiting = 1;
        bool isInProgress = true;
        std::string reason;
    };

    Mutex _mutex = MONGO_MAKE_LATCH("DDLLockManager::_mutex");
    StringMap<std::shared_ptr<NSLock>> _inProgressMap;

    enum class State {
        /**
         * When the node become secondary the state is set to kPaused and all the lock acquisitions
         * will be blocked except if the request comes from a DDLCoordinator.
         */
        kPaused,

        /**
         * After the node became primary and the ShardingDDLCoordinatorService already re-acquired
         * all the previously acquired DDL locks for ongoing DDL coordinators the state transition
         * to kPrimaryAndRecovered and the lock acquisitions are unblocked.
         */
        kPrimaryAndRecovered,
    };

    State _state = State::kPaused;
    mutable stdx::condition_variable _stateCV;

    void setState(const State& state);

    ScopedLock lock(OperationContext* opCtx,
                    StringData ns,
                    StringData reason,
                    Milliseconds timeout,
                    bool waitForRecovery);

    friend class ShardingDDLCoordinator;
    friend class ShardingDDLCoordinatorService;
    friend class ShardingDDLCoordinatorServiceTest;
};

}  // namespace mongo
