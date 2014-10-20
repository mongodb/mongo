/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <queue>

#include "mongo/db/concurrency/fast_map_noalloc.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/db/concurrency/locker.h"

namespace mongo {

    /**
     * Notfication callback, which stores the last notification result and signals a condition
     * variable, which can be waited on.
     */
    class CondVarLockGrantNotification : public LockGrantNotification {
        MONGO_DISALLOW_COPYING(CondVarLockGrantNotification);
    public:
        CondVarLockGrantNotification();

        /**
         * Clears the object so it can be reused.
         */
        void clear();

        /**
         * Uninterruptible blocking method, which waits for the notification to fire.
         *
         * @param timeoutMs How many milliseconds to wait before returning LOCK_TIMEOUT.
         */
        LockResult wait(unsigned timeoutMs);

    private:

        virtual void notify(const ResourceId& resId, LockResult result);

        // These two go together to implement the conditional variable pattern.
        boost::mutex _mutex;
        boost::condition_variable _cond;

        // Result from the last call to notify
        LockResult _result;
    };


    /**
     * Interface for acquiring locks. One of those objects will have to be instantiated for each
     * request (transaction).
     *
     * Lock/unlock methods must always be called from a single thread.
     *
     * All instances reference a single global lock manager.
     *
     * @param IsForMMAPV1 Whether to compile-in the flush lock functionality, which is specific to
     *          the way the MMAP V1 (legacy) storag engine does commit concurrency control.
     */
    template<bool IsForMMAPV1>
    class LockerImpl : public Locker {
    public:

        /**
         * Instantiates a new lock space with the specified unique identifier used for
         * disambiguation.
         */
        LockerImpl(LockerId id);

        virtual ~LockerImpl();

        virtual LockerId getId() const { return _id; }

        virtual LockResult lockGlobal(LockMode mode, unsigned timeoutMs = UINT_MAX);
        virtual void downgradeGlobalXtoSForMMAPV1();
        virtual bool unlockAll();

        virtual void beginWriteUnitOfWork();
        virtual void endWriteUnitOfWork();

        virtual bool inAWriteUnitOfWork() const { return _wuowNestingLevel > 0; }

        virtual LockResult lock(const ResourceId& resId,
                                LockMode mode, 
                                unsigned timeoutMs = UINT_MAX,
                                bool checkDeadlock = false);

        virtual bool unlock(const ResourceId& resId);

        virtual LockMode getLockMode(const ResourceId& resId) const;
        virtual bool isLockHeldForMode(const ResourceId& resId, LockMode mode) const;

        virtual ResourceId getWaitingResource() const;

        virtual bool saveLockStateAndUnlock(LockSnapshot* stateOut);

        virtual void restoreLockState(const LockSnapshot& stateToRestore);

        /**
         * Posts a request to the lock manager for the specified lock to be acquired and returns
         * immediately.
         *
         * NOTE: Must only be used to implement the actual lock call and for unit tests, because
         * it skips any internal consistency checks.
         */
        LockResult lockImpl(const ResourceId& resId, LockMode mode);

    private:

        friend class AutoYieldFlushLockForMMAPV1Commit;

        typedef FastMapNoAlloc<ResourceId, LockRequest, 16> LockRequestsMap;


        /**
         * The main functionality of the unlock method, except accepts iterator in order to avoid
         * additional lookups during unlockAll.
         */
        bool _unlockImpl(LockRequestsMap::Iterator& it);

        /**
         * Temporarily yields the flush lock, if not in a write unit of work so that the commit
         * thread can take turn. This is called automatically at each lock acquisition point, but
         * can also be called more frequently than that if need be.
         */
        void _yieldFlushLockForMMAPV1();

        /**
         * MMAP V1 locking code yields and re-acquires the flush lock occasionally in order to
         * allow the flush thread proceed. This call returns in what mode the flush lock should be
         * acquired. It is based on the type of the operation (IS for readers, IX for writers).
         */
        LockMode _getModeForMMAPV1FlushLock() const;


        // Used to disambiguate different lockers
        const LockerId _id;

        // The only reason we have this spin lock here is for the diagnostic tools, which could
        // iterate through the LockRequestsMap on a separate thread and need it to be stable.
        // Apart from that, all accesses to the LockerImpl are always from a single thread.
        //
        // This has to be locked inside const methods, hence the mutable.
        mutable SpinLock _lock;
        LockRequestsMap _requests;

        // Reuse the notification object across requests so we don't have to create a new mutex
        // and condition variable every time.
        CondVarLockGrantNotification _notify;

        // Delays release of exclusive/intent-exclusive locked resources until the write unit of
        // work completes. Value of 0 means we are not inside a write unit of work.
        int _wuowNestingLevel;
        std::queue<ResourceId> _resourcesToUnlockAtEndOfUnitOfWork;


        //////////////////////////////////////////////////////////////////////////////////////////
        //
        // Methods merged from LockState, which should eventually be removed or changed to methods
        // on the LockerImpl interface.
        //

    public:

        virtual void dump() const;

        virtual BSONObj reportState();
        virtual void reportState(BSONObjBuilder* b);

        virtual unsigned recursiveCount() const { return _recursive; }

        virtual bool isW() const;
        virtual bool isR() const;
        virtual bool hasAnyReadLock() const;

        virtual bool isLocked() const;
        virtual bool isWriteLocked() const;
        virtual bool isWriteLocked(const StringData& ns) const;
        virtual bool isDbLockedForMode(const StringData& dbName, LockMode mode) const;
        virtual bool isAtLeastReadLocked(const StringData& ns) const;
        virtual bool isRecursive() const;

        virtual void assertWriteLocked(const StringData& ns) const;

        /** 
         * Pending means we are currently trying to get a lock.
         */
        virtual bool hasLockPending() const { return _lockPending || _lockPendingParallelWriter; }

        // ----

        // Those are only used for TempRelease. Eventually they should be removed.
        virtual void enterScopedLock(Lock::ScopedLock* lock);
        virtual Lock::ScopedLock* getCurrentScopedLock() const;
        virtual void leaveScopedLock(Lock::ScopedLock* lock);

        virtual void recordLockTime() { _scopedLk->recordTime(); }
        virtual void resetLockTime() { _scopedLk->resetTime(); }

        virtual void setIsBatchWriter(bool newValue) { _batchWriter = newValue; }
        virtual bool isBatchWriter() const { return _batchWriter; }
        virtual void setLockPendingParallelWriter(bool newValue) { 
            _lockPendingParallelWriter = newValue;
        }

    private:
        /**
         * Indicates the mode of acquisition of the GlobalLock by this particular thread. The
         * return values are '0' (no global lock is held), 'r', 'w', 'R', 'W'.
         */
        char threadState() const;

        bool _batchWriter;
        bool _lockPendingParallelWriter;

        unsigned _recursive;           // we allow recursively asking for a lock; we track that here

        // for temprelease
        // for the nonrecursive case. otherwise there would be many
        // the first lock goes here, which is ok since we can't yield recursive locks
        Lock::ScopedLock* _scopedLk;

        bool _lockPending;
    };

    typedef LockerImpl<true> MMAPV1LockerImpl;


    /**
     * At global synchronization points, such as drop database we are running under a global
     * exclusive lock and without an active write unit of work, doing changes which require global
     * commit. This utility allows the flush lock to be temporarily dropped so the flush thread
     * could run in such circumstances. Should not be used where write units of work are used,
     * because these have different mechanism of yielding the flush lock.
     */
    class AutoYieldFlushLockForMMAPV1Commit {
    public:
        AutoYieldFlushLockForMMAPV1Commit(Locker* locker);
        ~AutoYieldFlushLockForMMAPV1Commit();

    private:
        MMAPV1LockerImpl* _locker;
    };


    /**
     * The resourceIdMMAPV1Flush lock is used to implement the MMAP V1 storage engine durability
     * system synchronization. This is how it works :
     *
     * Every server operation (OperationContext), which calls lockGlobal as the first locking
     * action (it is illegal to acquire any other locks without calling this first). This action
     * acquires the global and flush locks in the appropriate modes (IS for read operations, IX
     * for write operations). Having the flush lock in one of these modes indicates to the flush
     * thread that there is an active reader or writer.
     *
     * Whenever the flush thread(dur.cpp) activates, it goes through the following steps :
     *
     *  - Acquire the flush lock in S - mode by creating a stack instance of
     *      AutoAcquireFlushLockForMMAPV1Commit. This waits till all write activity on the system
     *      completes and does not allow new write operations to start. Readers may still proceed.
     *
     * - Once the flush lock is granted in S - mode, the flush thread writes the journal entries
     *      to disk and applies them to the shared view. After that, it upgrades the S - lock to X
     *      and remaps the private view.
     *
     * NOTE: There should be only one usage of this class and this should be in dur.cpp.
     *
     */
    class AutoAcquireFlushLockForMMAPV1Commit {
    public:
        AutoAcquireFlushLockForMMAPV1Commit(Locker* locker);
        ~AutoAcquireFlushLockForMMAPV1Commit();

    private:
        MMAPV1LockerImpl* _locker;
    };


    /**
     * Retrieves the global lock manager instance.
     */
    LockManager* getGlobalLockManager();

} // namespace mongo
