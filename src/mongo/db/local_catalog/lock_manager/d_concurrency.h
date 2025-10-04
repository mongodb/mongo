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

#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/lock_manager/locker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/intent_guard.h"
#include "mongo/db/repl/intent_registry.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/time_support.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class Lock {
public:
    // TODO (SERVER-102774): Remove this aliasing by substituting all usages of Lock::ResourceMutex
    // with ResourceMutex.
    using ResourceMutex = mongo::ResourceMutex;

    /**
     * General purpose RAII wrapper for a resource managed by the lock manager
     *
     * See LockMode for the supported modes. Unlike DBLock/Collection lock, this will not do
     * any additional checks/upgrades or global locking. Use ResourceLock for locking
     * resources other than RESOURCE_GLOBAL, RESOURCE_DATABASE and RESOURCE_COLLECTION.
     */
    class ResourceLock {
    public:
        ResourceLock(OperationContext* opCtx,
                     ResourceId rid,
                     LockMode mode,
                     Date_t deadline = Date_t::max())
            : _opCtx(opCtx), _rid(rid) {
            _lock(mode, deadline);
        }

        ResourceLock(ResourceLock&& other);

        ~ResourceLock() {
            _unlock();
        }

    protected:
        /**
         * Acquires lock on this specified resource in the specified mode.
         *
         * If 'deadline' is provided, we will wait until 'deadline' for the lock to be granted.
         * Otherwise, this parameter defaults to an infinite deadline.
         *
         * This function may throw an exception if it is interrupted.
         */
        void _lock(LockMode mode, Date_t deadline = Date_t::max());
        void _unlock();
        bool _isLocked() const {
            return _result == LOCK_OK;
        }

        OperationContext* _opCtx;

    private:
        ResourceId _rid;

        LockResult _result{LOCK_INVALID};
    };

    /**
     * Obtains a ResourceMutex for exclusive use.
     */
    class ExclusiveLock : public ResourceLock {
    public:
        ExclusiveLock(OperationContext* opCtx, ResourceMutex mutex)
            : ResourceLock(opCtx, mutex.getRid(), MODE_X) {}

        // Lock/unlock overloads to allow ExclusiveLock to be used with condition_variable-like
        // utilities such as stdx::condition_variable_any and waitForConditionOrInterrupt
        void lock();
        void unlock() {
            _unlock();
        }

        bool isLocked() const {
            return _isLocked();
        }
    };

    /**
     * Obtains a ResourceMutex for shared/non-exclusive use. This uses MODE_IS rather than MODE_S
     * to take advantage of optimizations in the lock manager for intent modes. This is OK as
     * this just has to conflict with exclusive locks.
     */
    class SharedLock : public ResourceLock {
    public:
        SharedLock(OperationContext* opCtx, ResourceMutex mutex)
            : ResourceLock(opCtx, mutex.getRid(), MODE_IS) {}
    };

    /**
     * The interrupt behavior is used to tell a lock how to handle an interrupted lock acquisition.
     */
    enum class InterruptBehavior {
        kThrow,         // Throw the interruption exception.
        kLeaveUnlocked  // Suppress the exception, but leave unlocked such that a call to isLocked()
                        // returns false.
    };

    struct GlobalLockOptions {
        bool skipFlowControlTicket = false;
        bool skipRSTLLock = false;
        bool skipDirectConnectionChecks = false;
        boost::optional<rss::consensus::IntentRegistry::Intent> explicitIntent = boost::none;
    };

    /**
     * Global lock.
     *
     * Grabs global resource lock. Allows further (recursive) acquisition of the global lock
     * in any mode, see LockMode. An outermost GlobalLock, when not in a WriteUnitOfWork, calls
     * abandonSnapshot() on destruction. This allows the storage engine to release resources, such
     * as snapshots or locks, that it may have acquired during the transaction.
     */
    class GlobalLock {
    public:
        /**
         * A GlobalLock without a deadline defaults to Date_t::max() and an InterruptBehavior of
         * kThrow.
         */
        GlobalLock(OperationContext* opCtx, LockMode lockMode)
            : GlobalLock(opCtx, lockMode, Date_t::max(), InterruptBehavior::kThrow) {}

        GlobalLock(OperationContext* opCtx, LockMode lockMode, GlobalLockOptions skipOptions)
            : GlobalLock(opCtx, lockMode, Date_t::max(), InterruptBehavior::kThrow, skipOptions) {}

        /**
         * A GlobalLock with a deadline requires the interrupt behavior to be explicitly defined.
         */
        GlobalLock(OperationContext* opCtx,
                   LockMode lockMode,
                   Date_t deadline,
                   InterruptBehavior behavior);

        GlobalLock(OperationContext* opCtx,
                   LockMode lockMode,
                   Date_t deadline,
                   InterruptBehavior behavior,
                   GlobalLockOptions skipOptions);

        GlobalLock(GlobalLock&&);

        ~GlobalLock();

        bool isLocked() const {
            return _result == LOCK_OK;
        }

    private:
        /**
         * Constructor helper functions, to handle skipping or taking the RSTL lock.
         */
        void _takeGlobalLockOnly(LockMode lockMode, Date_t deadline);
        void _takeGlobalAndRSTLLocks(
            LockMode lockMode,
            Date_t deadline,
            boost::optional<rss::consensus::IntentRegistry::Intent> explicitIntent = boost::none);
        void _declareIntent(LockMode lockMode,
                            boost::optional<rss::consensus::IntentRegistry::Intent> explicitIntent);

        void _unlock();

        OperationContext* const _opCtx;
        LockResult _result{LOCK_INVALID};

        boost::optional<ResourceLock> _multiDocTxnBarrier;

        InterruptBehavior _interruptBehavior;
        bool _skipRSTLLock;
        boost::optional<rss::consensus::IntentGuard> _guard;
    };

    /**
     * Global exclusive lock
     *
     * Allows exclusive write access to all databases and collections, blocking all other
     * access. Allows further (recursive) acquisition of the global lock in any mode,
     * see LockMode.
     */
    class GlobalWrite : public GlobalLock {
    public:
        explicit GlobalWrite(OperationContext* opCtx)
            : GlobalWrite(opCtx, Date_t::max(), InterruptBehavior::kThrow) {}
        explicit GlobalWrite(OperationContext* opCtx, Date_t deadline, InterruptBehavior behavior)
            : GlobalLock(
                  opCtx,
                  MODE_X,
                  deadline,
                  behavior,
                  {false, false, false, rss::consensus::IntentRegistry::Intent::LocalWrite}) {}
    };

    /**
     * Global shared lock
     *
     * Allows concurrent read access to all databases and collections, blocking any writers.
     * Allows further (recursive) acquisition of the global lock in shared (S) or intent-shared
     * (IS) mode, see LockMode.
     */
    class GlobalRead : public GlobalLock {
    public:
        explicit GlobalRead(OperationContext* opCtx)
            : GlobalRead(opCtx, Date_t::max(), InterruptBehavior::kThrow) {}
        explicit GlobalRead(OperationContext* opCtx, Date_t deadline, InterruptBehavior behavior)
            : GlobalLock(opCtx, MODE_S, deadline, behavior) {}
    };

    using DBLockSkipOptions = GlobalLockOptions;

    /**
     *  Tenant lock.
     *
     *  Controls access to resources belonging to a tenant.
     *
     * This lock supports four modes (see Lock_Mode):
     *   MODE_IS: concurrent access to tenant's resources, requiring further database read locks
     *   MODE_IX: concurrent access to tenant's resources, requiring further database read or write
     * locks
     *   MODE_S: shared read access to tenant's resources, blocking any writers
     *   MODE_X: exclusive access to tenant's resources, blocking all other readers and writers.
     */
    class TenantLock {
        TenantLock(const TenantLock&) = delete;
        TenantLock& operator=(const TenantLock&) = delete;

    public:
        TenantLock(OperationContext* opCtx,
                   const TenantId& tenantId,
                   LockMode mode,
                   Date_t deadline = Date_t::max());

        TenantLock(TenantLock&&);
        ~TenantLock();

    private:
        ResourceId _id;
        OperationContext* _opCtx;
    };

    /**
     * Database lock.
     *
     * This lock supports four modes (see Lock_Mode):
     *   MODE_IS: concurrent database access, requiring further collection read locks
     *   MODE_IX: concurrent database access, requiring further collection read or write locks
     *   MODE_S:  shared read access to the database, blocking any writers
     *   MODE_X:  exclusive access to the database, blocking all other readers and writers
     *
     * For MODE_IS or MODE_S also acquires global lock in intent-shared (IS) mode, and
     * for MODE_IX or MODE_X also acquires global lock in intent-exclusive (IX) mode.
     * For storage engines that do not support collection-level locking, MODE_IS will be
     * upgraded to MODE_S and MODE_IX will be upgraded to MODE_X.
     *
     * If the database belongs to a tenant, then acquires a tenant lock before the database lock.
     * For 'mode' MODE_IS or MODE_S acquires tenant lock in intent-shared (IS) mode, otherwise,
     * acquires a tenant lock in intent-exclusive (IX) mode. A different, stronger tenant lock mode
     * to acquire can be specified with 'tenantLockMode' parameter. Passing boost::none for the
     * tenant lock mode does not skip the tenant lock, but indicates that the tenant lock in default
     * mode should be acquired.
     */
    class DBLock {
    public:
        DBLock(OperationContext* opCtx,
               const DatabaseName& dbName,
               LockMode mode,
               Date_t deadline = Date_t::max(),
               boost::optional<LockMode> tenantLockMode = boost::none);

        DBLock(OperationContext* opCtx,
               const DatabaseName& dbName,
               LockMode mode,
               Date_t deadline,
               DBLockSkipOptions skipOptions,
               boost::optional<LockMode> tenantLockMode = boost::none);

        DBLock(DBLock&&);
        ~DBLock();

        bool isLocked() const {
            return _result == LOCK_OK;
        }

        LockMode mode() const {
            return _mode;
        }

    private:
        const ResourceId _id;
        OperationContext* const _opCtx;
        LockResult _result;

        // May be changed through relockWithMode. The global lock mode won't change though,
        // because we never change from IS/S to IX/X or vice versa, just convert locks from
        // IX -> X.
        LockMode _mode;

        // Acquires the global lock on our behalf.
        boost::optional<GlobalLock> _globalLock;

        // Acquires the tenant lock on behalf of this DB lock.
        boost::optional<TenantLock> _tenantLock;
        bool _oldBlockingAllowed;
    };

    /**
     * Collection lock.
     *
     * This lock supports four modes (see Lock_Mode):
     *   MODE_IS: concurrent collection access, requiring read locks
     *   MODE_IX: concurrent collection access, requiring read or write locks
     *   MODE_S:  shared read access to the collection, blocking any writers
     *   MODE_X:  exclusive access to the collection, blocking all other readers and writers
     *
     * An appropriate DBLock must already be held before locking a collection: it is an error,
     * checked with a dassert(), to not have a suitable database lock before locking the collection.
     */
    class CollectionLock {
        CollectionLock(const CollectionLock&) = delete;
        CollectionLock& operator=(const CollectionLock&) = delete;

    public:
        CollectionLock(OperationContext* opCtx,
                       const NamespaceString& ns,
                       LockMode mode,
                       Date_t deadline = Date_t::max());

        CollectionLock(CollectionLock&&);
        CollectionLock& operator=(CollectionLock&& other);
        ~CollectionLock();

    private:
        ResourceId _id;
        OperationContext* _opCtx;
        bool _oldBlockingAllowed;
    };
};

}  // namespace mongo
