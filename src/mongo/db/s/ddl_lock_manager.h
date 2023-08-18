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

#include <memory>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/duration.h"
#include "mongo/util/string_map.h"

namespace mongo {

/**
 * Service to manage DDL locks.
 */
class DDLLockManager {

    /**
     * ScopedBaseDDLLock will hold a DDL lock for the given resource without performing any check.
     */
    class ScopedBaseDDLLock {
    public:
        ScopedBaseDDLLock(OperationContext* opCtx,
                          Locker* locker,
                          const NamespaceString& ns,
                          StringData reason,
                          LockMode mode,
                          bool waitForRecovery);

        ScopedBaseDDLLock(OperationContext* opCtx,
                          Locker* locker,
                          const DatabaseName& db,
                          StringData reason,
                          LockMode mode,
                          bool waitForRecovery);

        virtual ~ScopedBaseDDLLock();

        ScopedBaseDDLLock(ScopedBaseDDLLock&& other);

        StringData getResourceName() const {
            return _resourceName;
        }
        StringData getReason() const {
            return _reason;
        }

    protected:
        ScopedBaseDDLLock(const ScopedBaseDDLLock&) = delete;
        ScopedBaseDDLLock& operator=(const ScopedBaseDDLLock&) = delete;

        ScopedBaseDDLLock(OperationContext* opCtx,
                          Locker* locker,
                          StringData resName,
                          const ResourceId& resId,
                          StringData reason,
                          LockMode mode,
                          bool waitForRecovery);

        static const Minutes kDefaultLockTimeout;
        static Milliseconds _getTimeout();

        // Attributes
        const std::string _resourceName;
        const ResourceId _resourceId;
        const std::string _reason;
        const LockMode _mode;
        LockResult _result;
        Locker* _locker;
        DDLLockManager* _lockManager;
    };

public:
    // Timeout value, which specifies that if the lock is not available immediately, no attempt
    // should be made to wait for it to become free.
    static const Milliseconds kSingleLockAttemptTimeout;

    // RAII-style class to acquire a DDL lock on the given database
    class ScopedDatabaseDDLLock : public ScopedBaseDDLLock {
    public:
        /**
         * Constructs a ScopedDatabaseDDLLock object
         *
         * @db      Database to lock.
         * @reason 	Reason for which the lock is being acquired (e.g. 'createCollection').
         * @mode    Lock mode.
         *
         * Throws:
         *     ErrorCodes::LockBusy in case the timeout is reached.
         *     ErrorCodes::LockTimeout when not being on kPrimaryAndRecovered state and the internal
         *            timeout is reached.
         *     ErrorCategory::Interruption in case the operation context is interrupted.
         *     ErrorCodes::IllegalOperation in case of not being on the db primary shard.
         *
         * It's caller's responsibility to ensure this lock is acquired only on primary node of
         * replica set and released on step-down.
         */
        ScopedDatabaseDDLLock(OperationContext* opCtx,
                              const DatabaseName& db,
                              StringData reason,
                              LockMode mode);
    };

    // RAII-style class to acquire a DDL lock on the given collection. The database DDL lock will
    // also be implicitly acquired in the corresponding intent mode.
    class ScopedCollectionDDLLock {
    public:
        /**
         * Constructs a ScopedCollectionDDLLock object
         *
         * @ns      Collection to lock.
         * @reason 	Reason for which the lock is being acquired (e.g. 'createCollection').
         * @mode    Lock mode.
         *
         * Throws:
         *     ErrorCodes::LockBusy in case the timeout is reached.
         *     ErrorCodes::LockTimeout when not being on kPrimaryAndRecovered state and the internal
         *            timeout is reached.
         *     ErrorCategory::Interruption in case the operation context is interrupted.
         *     ErrorCodes::IllegalOperation in case of not being on the db primary shard.
         *
         * It's caller's responsibility to ensure this lock is acquired only on primary node of
         * replica set and released on step-down.
         */
        ScopedCollectionDDLLock(OperationContext* opCtx,
                                const NamespaceString& ns,
                                StringData reason,
                                LockMode mode);

    private:
        // Make sure _dbLock is instantiated before _collLock to don't break the hierarchy locking
        // acquisition order
        boost::optional<ScopedBaseDDLLock> _dbLock;
        boost::optional<ScopedBaseDDLLock> _collLock;
    };

    DDLLockManager() = default;
    ~DDLLockManager() = default;

    /**
     * Retrieves the DDLLockManager singleton.
     */
    static DDLLockManager* get(ServiceContext* service);
    static DDLLockManager* get(OperationContext* opCtx);

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

    void _lock(OperationContext* opCtx,
               Locker* locker,
               StringData ns,
               const ResourceId& resId,
               StringData reason,
               LockMode mode,
               Date_t deadline,
               bool waitForRecovery);

    void _unlock(
        Locker* locker, StringData ns, const ResourceId& resId, StringData reason, LockMode mode);

    // Stores how many holders either are trying to acquire or are holding a specific resource at
    // that moment.
    stdx::unordered_map<ResourceId, int32_t> _numHoldersPerResource;

    /**
     * Register/Unregister a resourceName into the ResourceCatalog for debuggability purposes.
     */
    void _registerResourceName(WithLock lk, ResourceId resId, StringData resName);
    void _unregisterResourceNameIfNoLongerNeeded(WithLock lk, ResourceId resId, StringData resName);


    friend class ShardingCatalogManager;
    friend class ShardingDDLCoordinator;
    friend class ShardingDDLCoordinatorService;
    friend class ShardingDDLCoordinatorServiceTest;
};

}  // namespace mongo
