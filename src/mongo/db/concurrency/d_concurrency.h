/**
 *    Copyright (C) 2008-2014 MongoDB Inc.
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

#include <climits>  // For UINT_MAX

#include "mongo/db/concurrency/locker.h"
#include "mongo/util/timer.h"

namespace mongo {

class StringData;
class NamespaceString;

class Lock {
public:
    /**
     * NOTE: DO NOT add any new usages of TempRelease. It is being deprecated/removed.
     */
    class TempRelease {
        MONGO_DISALLOW_COPYING(TempRelease);

    public:
        explicit TempRelease(Locker* lockState);
        ~TempRelease();

    private:
        // Not owned
        Locker* const _lockState;

        // If _locksReleased is true, this stores the persisted lock information to be restored
        // in the destructor. Otherwise it is empty.
        Locker::LockSnapshot _lockSnapshot;

        // False if locks could not be released because of recursive locking
        const bool _locksReleased;
    };


    /**
     * General purpose RAII wrapper for a resource managed by the lock manager
     *
     * See LockMode for the supported modes. Unlike DBLock/Collection lock, this will not do
     * any additional checks/upgrades or global locking. Use ResourceLock for locking
     * resources other than RESOURCE_GLOBAL, RESOURCE_DATABASE and RESOURCE_COLLECTION.
     */
    class ResourceLock {
        MONGO_DISALLOW_COPYING(ResourceLock);

    public:
        ResourceLock(Locker* locker, ResourceId rid)
            : _rid(rid), _locker(locker), _result(LOCK_INVALID) {}

        ResourceLock(Locker* locker, ResourceId rid, LockMode mode)
            : _rid(rid), _locker(locker), _result(LOCK_INVALID) {
            lock(mode);
        }

        ~ResourceLock() {
            unlock();
        }

        void lock(LockMode mode);
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
     * model. A ResourceMutex is not affected by yielding/temprelease and two phase locking
     * semantics inside WUOWs. Lock with ResourceLock, SharedLock or ExclusiveLock. Uses same
     * fairness as other LockManager locks.
     */
    class ResourceMutex {
    public:
        ResourceMutex();

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
            : ResourceLock(locker, mutex.rid(), MODE_X) {}
    };

    /**
     * Obtains a ResourceMutex for shared/non-exclusive use. This uses MODE_IS rather than MODE_S
     * to take advantage of optimizations in the lock manager for intent modes. This is OK as
     * this just has to conflict with exclusive locks.
     */
    class SharedLock : public ResourceLock {
    public:
        SharedLock(Locker* locker, ResourceMutex mutex)
            : ResourceLock(locker, mutex.rid(), MODE_IS) {}
    };

    /**
     * Global lock.
     *
     * Grabs global resource lock. Allows further (recursive) acquisition of the global lock
     * in any mode, see LockMode.
     * NOTE: Does not acquire flush lock.
     */
    class GlobalLock {
    public:
        class EnqueueOnly {};

        explicit GlobalLock(Locker* locker);
        GlobalLock(Locker* locker, LockMode lockMode, unsigned timeoutMs);

        /**
         * Enqueues lock but does not block on lock acquisition.
         * Call waitForLock() to complete locking process.
         */
        GlobalLock(Locker* locker, LockMode lockMode, EnqueueOnly enqueueOnly);

        ~GlobalLock() {
            _unlock();
        }

        /**
         * Waits for lock to be granted.
         */
        void waitForLock(unsigned timeoutMs);

        bool isLocked() const {
            return _result == LOCK_OK;
        }

    private:
        void _enqueue(LockMode lockMode);
        void _unlock();

        Locker* const _locker;
        LockResult _result;
        ResourceLock _pbwm;
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
        explicit GlobalWrite(Locker* locker, unsigned timeoutMs = UINT_MAX)
            : GlobalLock(locker, MODE_X, timeoutMs) {
            if (isLocked()) {
                locker->lockMMAPV1Flush();
            }
        }
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
        explicit GlobalRead(Locker* locker, unsigned timeoutMs = UINT_MAX)
            : GlobalLock(locker, MODE_S, timeoutMs) {
            if (isLocked()) {
                locker->lockMMAPV1Flush();
            }
        }
    };


    /**
     * Database lock with support for collection- and document-level locking
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
        DBLock(Locker* locker, StringData db, LockMode mode);
        ~DBLock();

        /**
         * Releases the DBLock and reacquires it with the new mode. The global intent
         * lock is retained (so the database can't disappear). Relocking from MODE_IS or
         * MODE_S to MODE_IX or MODE_X is not allowed to avoid violating the global intent.
         * Use relockWithMode() instead of upgrading to avoid deadlock.
         */
        void relockWithMode(LockMode newMode);

    private:
        const ResourceId _id;
        Locker* const _locker;

        // May be changed through relockWithMode. The global lock mode won't change though,
        // because we never change from IS/S to IX/X or vice versa, just convert locks from
        // IX -> X.
        LockMode _mode;

        // Acquires the global lock on our behalf.
        GlobalLock _globalLock;
    };


    /**
     * Collection lock with support for document-level locking
     *
     * This lock supports four modes (see Lock_Mode):
     *   MODE_IS: concurrent collection access, requiring document level locking read locks
     *   MODE_IX: concurrent collection access, requiring document level read or write locks
     *   MODE_S:  shared read access to the collection, blocking any writers
     *   MODE_X:  exclusive access to the collection, blocking all other readers and writers
     *
     * An appropriate DBLock must already be held before locking a collection: it is an error,
     * checked with a dassert(), to not have a suitable database lock before locking the
     * collection. For storage engines that do not support document-level locking, MODE_IS
     * will be upgraded to MODE_S and MODE_IX will be upgraded to MODE_X.
     */
    class CollectionLock {
        MONGO_DISALLOW_COPYING(CollectionLock);

    public:
        CollectionLock(Locker* lockState, StringData ns, LockMode mode);
        ~CollectionLock();

        /**
         * When holding the collection in MODE_IX or MODE_X, calling this will release the
         * collection and database locks, and relocks the database in MODE_X. This is typically
         * used if the collection still needs to be created. Upgrading would not be safe as
         * it could lead to deadlock, similarly for relocking the database without releasing
         * the collection lock. The collection lock will also be reacquired even though it is
         * not really needed, as it simplifies invariant checking: the CollectionLock class
         * has as invariant that a collection lock is being held.
         */
        void relockAsDatabaseExclusive(Lock::DBLock& dbLock);

    private:
        const ResourceId _id;
        Locker* const _lockState;
    };

    /**
     * Like the CollectionLock, but optimized for the local oplog. Always locks in MODE_IX,
     * must call serializeIfNeeded() before doing any concurrent operations in order to
     * support storage engines without document level locking. It is an error, checked with a
     * dassert(), to not have a suitable database lock when taking this lock.
     */
    class OplogIntentWriteLock {
        MONGO_DISALLOW_COPYING(OplogIntentWriteLock);

    public:
        explicit OplogIntentWriteLock(Locker* lockState);
        ~OplogIntentWriteLock();
        void serializeIfNeeded();

    private:
        Locker* const _lockState;
        bool _serialized;
    };


    /**
     * Turn on "parallel batch writer mode" by locking the global ParallelBatchWriterMode
     * resource in exclusive mode. This mode is off by default.
     * Note that only one thread creates a ParallelBatchWriterMode object; the other batch
     * writers just call setIsBatchWriter().
     */
    class ParallelBatchWriterMode {
        MONGO_DISALLOW_COPYING(ParallelBatchWriterMode);

    public:
        explicit ParallelBatchWriterMode(Locker* lockState);
        ~ParallelBatchWriterMode();

    private:
        ResourceLock _pbwm;
        Locker* const _lockState;
    };
};

/**
 * Takes a lock on resourceCappedInFlight in MODE_IX which will be held until the end of your
 * WUOW. This ensures that a MODE_X lock on this resource will wait for all in-flight capped
 * inserts to either commit or rollback and block new ones from starting.
 */
void synchronizeOnCappedInFlightResource(Locker* txn, const NamespaceString& cappedNs);
}
