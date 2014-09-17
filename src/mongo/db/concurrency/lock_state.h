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

#include "mongo/db/concurrency/locker.h"
#include "mongo/platform/unordered_map.h"


namespace mongo {
namespace newlm {
    
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
     */
    class LockerImpl : public Locker {
    public:

        /**
         * Instantiates a new lock space with the specified unique identifier used for
         * disambiguation.
         */
        LockerImpl(uint64_t id);
        LockerImpl();

        virtual ~LockerImpl();

        virtual uint64_t getId() const { return _id; }

        virtual LockResult lockGlobal(LockMode mode, unsigned timeoutMs = UINT_MAX);
        virtual void downgradeGlobalXtoSForMMAPV1();
        virtual bool unlockGlobal();

        virtual void beginWriteUnitOfWork();
        virtual void endWriteUnitOfWork();

        virtual bool inAWriteUnitOfWork() const { return _wuowNestingLevel > 0; }

        virtual LockResult lock(const ResourceId& resId,
                                LockMode mode, 
                                unsigned timeoutMs = UINT_MAX);

        virtual bool unlock(const ResourceId& resId);

        virtual LockMode getLockMode(const ResourceId& resId) const;
        virtual bool isLockHeldForMode(const ResourceId& resId, LockMode mode) const;

        /**
         * Dumps all locks, on the global lock manager to the log for debugging purposes.
         */
        static void dumpGlobalLockManager();


        //
        // Methods used for unit-testing only
        //

        /**
         * Used for testing purposes only - changes the global lock manager. Doesn't delete the
         * previous instance, so make sure that it doesn't leak.
         *
         * @param newLockMgr New lock manager to be used. If NULL is passed, the original lock
         *                      manager is restored.
         */
        static void changeGlobalLockManagerForTestingOnly(LockManager* newLockMgr);

    private:

        /**
         * Shortcut to do the lookup in _requests. Must be called with the spinlock acquired.
         */
        LockRequest* _find(const ResourceId& resId) const;

        bool _unlockAndUpdateRequestsList(const ResourceId& resId, LockRequest* request);

        // BEGIN MMAP V1 SPECIFIC
        //

        // These methods, along with the resourceIdMMAPV1Flush lock, implement the MMAP V1 storage
        // engine durability system synchronization. This is the way it works:
        //
        // Every operation, which starts calls lockGlobal, which acquires the global and flush
        // locks in the appropriate mode (IS for read operations, IX for write operations). Having
        // the flush lock in these modes indicates that there is an active reader/write
        // respectively.
        //
        // Whenever the flush thread (dur.cpp) activates, it goes through the following steps:
        // - Acquires the flush lock in X-mode (by creating a stack instance of
        //      AutoAcquireFlushLockForMMAPV1Commit). This waits till all activity on the system
        //      completes and does not allow new operations to start.
        // 
        // This works, because as long as an operation is not in a write transaction
        // (beginWriteUnitOfWork has not been called), occasionally, on each lock acquisition
        // point, the locker will yield the flush lock and then acquire it again, so that the flush
        // thread can take turn. This is safe to do outside of a write transaction, because there
        // are no partially written changes.

        /**
         * Temporarily yields the flush lock, if not in a write unit of work so that the commit
         * thread can take turn. This is called automatically at each lock acquisition point, but
         * can also be called more frequently than that if need be.
         */
        void _yieldFlushLockForMMAPV1();

        //
        // END MMAP V1 SPECIFIC


        typedef unordered_map<ResourceId, LockRequest*> LockRequestsMap;
        typedef LockRequestsMap::value_type LockRequestsPair;

        const uint64_t _id;

        // The only reason we have this spin lock here is for the diagnostic tools, which could
        // iterate through the LockRequestsMap on a separate thread and need it to be stable.
        // Apart from that, all accesses to the LockerImpl are always from a single thread.
        //
        // This has to be locked inside const methods, hence the mutable.
        mutable SpinLock _lock;
        LockRequestsMap _requests;

        CondVarLockGrantNotification _notify;

        std::queue<ResourceId> _resourcesToUnlockAtEndOfUnitOfWork;
        int _wuowNestingLevel; // if > 0 we are inside of a WriteUnitOfWork


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

        /**
         * Indicates the mode of acquisition of the GlobalLock by this particular thread. The
         * return values are '0' (no global lock is held), 'r', 'w', 'R', 'W'. See the commends of
         * QLock for more information on what these modes mean.
         */
        virtual char threadState() const;

        virtual bool isW() const;
        virtual bool isR() const;
        virtual bool hasAnyReadLock() const;

        virtual bool isLocked() const;
        virtual bool isWriteLocked() const;
        virtual bool isWriteLocked(const StringData& ns) const;
        virtual bool isAtLeastReadLocked(const StringData& ns) const;
        virtual bool isLockedForCommitting() const;
        virtual bool isRecursive() const;

        virtual void assertWriteLocked(const StringData& ns) const;
        virtual void assertAtLeastReadLocked(const StringData& ns) const;

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
        bool _batchWriter;
        bool _lockPendingParallelWriter;

        unsigned _recursive;           // we allow recursively asking for a lock; we track that here

        // for temprelease
        // for the nonrecursive case. otherwise there would be many
        // the first lock goes here, which is ok since we can't yield recursive locks
        Lock::ScopedLock* _scopedLk;

        bool _lockPending;
    };


    /**
     * At the end of a write transaction, we cannot release any of the exclusive locks before the
     * data which was written as part of the transaction is at least journaled. This is done by the
     * flush thread (dur.cpp). However, the flush thread cannot take turn while we are holding the
     * flush lock. This class releases *only* the flush lock, while in scope so that the flush
     * thread can run. It then re-acquires the flush lock in the original mode in which it was
     * acquired.
     */
    class AutoYieldFlushLockForMMAPV1Commit {
    public:
        AutoYieldFlushLockForMMAPV1Commit(Locker* locker);
        ~AutoYieldFlushLockForMMAPV1Commit();

    private:
        Locker* _locker;
    };


    /**
     * There should be only one instance of this class used anywhere (outside of unit-tests) and it
     * should be in dur.cpp. See the comments above, in the MMAP V1 SPECIFIC section for more
     * information on how this is used.
     */
    class AutoAcquireFlushLockForMMAPV1Commit {
    public:
        explicit AutoAcquireFlushLockForMMAPV1Commit(Locker* locker);
        ~AutoAcquireFlushLockForMMAPV1Commit();

    private:
        Locker* _locker;
    };
    
} // namespace newlm


    /**
     * This will go away as a separate step.
     */
    class LockState : public newlm::LockerImpl {
    public:
        LockState() { }

    };

} // namespace mongo
