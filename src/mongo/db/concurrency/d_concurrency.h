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

#include <climits>  // For UINT_MAX

#include "mongo/db/concurrency/locker.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/timer.h"

namespace mongo {

class StringData;
class NamespaceStringOrUUID;

class Lock {
public:
    /**
     * General purpose RAII wrapper for a resource managed by the lock manager
     *
     * See LockMode for the supported modes. Unlike DBLock/Collection lock, this will not do
     * any additional checks/upgrades or global locking. Use ResourceLock for locking
     * resources other than RESOURCE_GLOBAL, RESOURCE_DATABASE and RESOURCE_COLLECTION.
     */
    class ResourceLock {
        ResourceLock(const ResourceLock&) = delete;
        ResourceLock& operator=(const ResourceLock&) = delete;

    public:
        ResourceLock(Locker* locker, ResourceId rid)
            : _rid(rid), _locker(locker), _result(LOCK_INVALID) {}

        ResourceLock(Locker* locker, ResourceId rid, LockMode mode)
            : ResourceLock(nullptr, locker, rid, mode) {}

        ResourceLock(OperationContext* opCtx, Locker* locker, ResourceId rid, LockMode mode)
            : _rid(rid), _locker(locker), _result(LOCK_INVALID) {
            lock(opCtx, mode);
        }

        ResourceLock(ResourceLock&& otherLock)
            : _rid(otherLock._rid), _locker(otherLock._locker), _result(otherLock._result) {
            // Mark as moved so the destructor doesn't invalidate the newly-
            // constructed lock.
            otherLock._result = LOCK_INVALID;
        }

        ~ResourceLock() {
            if (isLocked()) {
                unlock();
            }
        }

        /**
         * Acquires lock on this specified resource in the specified mode.
         *
         * If 'opCtx' is provided, it will be used to interrupt a LOCK_WAITING state.
         * If 'deadline' is provided, we will wait until 'deadline' for the lock to be granted.
         * Otherwise, this parameter defaults to an infinite deadline.
         *
         * This function may throw an exception if it is interrupted.
         */
        void lock(OperationContext* opCtx, LockMode mode, Date_t deadline = Date_t::max());

        void unlock();

        bool isLocked() const {
            return _result == LOCK_OK;
        }

    private:
        const ResourceId _rid;
        Locker* const _locker;

        LockResult _result;
    };

    class SharedLock;
    class ExclusiveLock;

    /**
     * For use as general mutex or readers/writers lock, outside the general multi-granularity
     * model. A ResourceMutex is not affected by yielding and two phase locking semantics inside
     * WUOWs. Lock with ResourceLock, SharedLock or ExclusiveLock. Uses same fairness as other
     * LockManager locks.
     */
    class ResourceMutex {
    public:
        ResourceMutex(std::string resourceLabel);

        std::string getName() const {
            return getName(_rid);
        };

        static std::string getName(ResourceId resourceId);

        bool isExclusivelyLocked(Locker* locker);

        bool isAtLeastReadLocked(Locker* locker);

    private:
        friend class Lock::SharedLock;
        friend class Lock::ExclusiveLock;

        /**
         * Each instantiation of this class allocates a new ResourceId.
         */
        ResourceId rid() const {
            return _rid;
        }

        const ResourceId _rid;
    };

    /**
     * Obtains a ResourceMutex for exclusive use.
     */
    class ExclusiveLock : public ResourceLock {
    public:
        ExclusiveLock(Locker* locker, ResourceMutex mutex)
            : ExclusiveLock(nullptr, locker, mutex) {}

        /**
         * Interruptible lock acquisition.
         */
        ExclusiveLock(OperationContext* opCtx, Locker* locker, ResourceMutex mutex)
            : ResourceLock(opCtx, locker, mutex.rid(), MODE_X) {}

        using ResourceLock::lock;

        /**
         * Parameterless overload to allow ExclusiveLock to be used with stdx::unique_lock and
         * stdx::condition_variable_any
         */
        void lock() {
            lock(nullptr, MODE_X);
        }
    };

    /**
     * Obtains a ResourceMutex for shared/non-exclusive use. This uses MODE_IS rather than MODE_S
     * to take advantage of optimizations in the lock manager for intent modes. This is OK as
     * this just has to conflict with exclusive locks.
     */
    class SharedLock : public ResourceLock {
    public:
        SharedLock(Locker* locker, ResourceMutex mutex) : SharedLock(nullptr, locker, mutex) {}

        /**
         * Interruptible lock acquisition.
         */
        SharedLock(OperationContext* opCtx, Locker* locker, ResourceMutex mutex)
            : ResourceLock(opCtx, locker, mutex.rid(), MODE_IS) {}
    };

    /**
     * The interrupt behavior is used to tell a lock how to handle an interrupted lock acquisition.
     */
    enum class InterruptBehavior {
        kThrow,         // Throw the interruption exception.
        kLeaveUnlocked  // Suppress the exception, but leave unlocked such that a call to isLocked()
                        // returns false.
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

        /**
         * A GlobalLock with a deadline requires the interrupt behavior to be explicitly defined.
         */
        GlobalLock(OperationContext* opCtx,
                   LockMode lockMode,
                   Date_t deadline,
                   InterruptBehavior behavior,
                   bool skipRSTLLock = false);

        GlobalLock(GlobalLock&&);

        ~GlobalLock() {
            // Preserve the original lock result which will be overridden by unlock().
            auto lockResult = _result;
            if (isLocked()) {
                // Abandon our snapshot if destruction of the GlobalLock object results in actually
                // unlocking the global lock. Recursive locking and the two-phase locking protocol
                // may prevent lock release.
                const bool willReleaseLock = _isOutermostLock &&
                    !(_opCtx->lockState() && _opCtx->lockState()->inAWriteUnitOfWork());
                if (willReleaseLock) {
                    _opCtx->recoveryUnit()->abandonSnapshot();
                }
                _unlock();
            }
            if (!_skipRSTLLock && (lockResult == LOCK_OK || lockResult == LOCK_WAITING)) {
                _opCtx->lockState()->unlock(resourceIdReplicationStateTransitionLock);
            }
        }

        bool isLocked() const {
            return _result == LOCK_OK;
        }

    private:
        /**
         * Constructor helper functions, to handle skipping or taking the RSTL lock.
         */
        void _takeGlobalLockOnly(LockMode lockMode, Date_t deadline);
        void _takeGlobalAndRSTLLocks(LockMode lockMode, Date_t deadline);

        void _unlock();

        OperationContext* const _opCtx;
        LockResult _result;
        ResourceLock _pbwm;
        ResourceLock _fcvLock;
        InterruptBehavior _interruptBehavior;
        bool _skipRSTLLock;
        const bool _isOutermostLock;
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
            : GlobalLock(opCtx, MODE_X, deadline, behavior) {}
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
     */
    class DBLock {
    public:
        DBLock(OperationContext* opCtx,
               const DatabaseName& dbName,
               LockMode mode,
               Date_t deadline = Date_t::max(),
               bool skipGlobalAndRSTLLocks = false);
        DBLock(DBLock&&);
        ~DBLock();

        /**
         * Releases the DBLock and reacquires it with the new mode. The global intent
         * lock is retained (so the database can't disappear). Relocking from MODE_IS or
         * MODE_S to MODE_IX or MODE_X is not allowed to avoid violating the global intent.
         * Use relockWithMode() instead of upgrading to avoid deadlock.
         */
        void relockWithMode(LockMode newMode);

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
                       const NamespaceStringOrUUID& nssOrUUID,
                       LockMode mode,
                       Date_t deadline = Date_t::max());

        CollectionLock(CollectionLock&&);
        ~CollectionLock();

    private:
        ResourceId _id;
        OperationContext* _opCtx;
    };

    /**
     * Turn on "parallel batch writer mode" by locking the global ParallelBatchWriterMode
     * resource in exclusive mode. This mode is off by default.
     * Note that only one thread creates a ParallelBatchWriterMode object; the other batch
     * writers just call setShouldConflictWithSecondaryBatchApplication(false).
     */
    class ParallelBatchWriterMode {
        ParallelBatchWriterMode(const ParallelBatchWriterMode&) = delete;
        ParallelBatchWriterMode& operator=(const ParallelBatchWriterMode&) = delete;

    public:
        explicit ParallelBatchWriterMode(Locker* lockState);

    private:
        ResourceLock _pbwm;
        ShouldNotConflictWithSecondaryBatchApplicationBlock _shouldNotConflictBlock;
    };
};

}  // namespace mongo
