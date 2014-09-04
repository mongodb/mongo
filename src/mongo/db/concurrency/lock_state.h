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

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_mgr_new.h"


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
     *
     * TODO: At some point, this will have to be renamed to LockState.
     */
    class Locker {
        MONGO_DISALLOW_COPYING(Locker);
    public:

        /**
         * Instantiates a new lock space with the specified unique identifier used for
         * disambiguation.
         */
        explicit Locker(uint64_t id);
        ~Locker();

        inline uint64_t getId() const { return _id; }

        /**
         * Acquires lock on the specified resource in the specified mode and returns the outcome
         * of the operation. See the details for LockResult for more information on what the
         * different results mean.
         *
         * Acquiring the same resource twice increments the reference count of the lock so each
         * call to lock, which doesn't time out (return value LOCK_TIMEOUT) must be matched with a
         * corresponding call to unlock.
         *
         * @param resId Id of the resource to be locked.
         * @param mode Mode in which the resource should be locked. Lock upgrades are allowed.
         * @param timeoutMs How many milliseconds to wait for the lock to be granted, before
         *              returning LOCK_TIMEOUT. This parameter defaults to UINT_MAX, which means
         *              wait infinitely. If 0 is passed, the request will return immediately, if
         *              the request could not be granted right away.
         *
         * @return All LockResults except for LOCK_WAITING, because it blocks.
         */
        LockResult lock(const ResourceId& resId, LockMode mode, unsigned timeoutMs = UINT_MAX);

        /**
         * Releases a lock previously acquired through a lock call. It is an error to try to
         * release lock which has not been previously acquired (invariant violation).
         *
         * @return true if the lock was actually released; false if only the reference count was 
         *              decremented, but the lock is still held.
         */
        bool unlock(const ResourceId& resId);

        /**
         * Retrieves the mode in which a lock is held or checks whether the lock held for a
         * particular resource covers the specified mode.
         *
         * For example MODE_X covers MODE_S.
         */
        LockMode getLockMode(const ResourceId& resId) const;
        bool isLockHeldForMode(const ResourceId& resId, LockMode mode) const;

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

        LockResult _lockImpl(const ResourceId& resId, LockMode mode, unsigned timeoutMs);

        bool _unlockAndUpdateRequestsList(const ResourceId& resId, LockRequest* request);


        typedef std::map<const ResourceId, LockRequest*> LockRequestsMap;
        typedef std::pair<const ResourceId, LockRequest*> LockRequestsPair;

        const uint64_t _id;

        // The only reason we have this spin lock here is for the diagnostic tools, which could
        // iterate through the LockRequestsMap on a separate thread and need it to be stable.
        // Apart from that, all accesses to the Locker are always from a single thread.
        //
        // This has to be locked inside const methods, hence the mutable.
        mutable SpinLock _lock;
        LockRequestsMap _requests;

        CondVarLockGrantNotification _notify;
    };
    
} // namespace newlm


    /**
     * One of these exists per OperationContext and serves as interface for acquiring locks and
     * obtaining lock statistics for this particular operation.
     *
     * TODO: It is only temporary that this class inherits from Locker. Both will eventually be
     * merged and most of the code in LockState will go away (i.e., once we move the GlobalLock to
     * be its own lock resource under the lock manager).
     */
    class LockState : public newlm::Locker {
    public:
        LockState();

        void dump() const;

        BSONObj reportState();
        void reportState(BSONObjBuilder* b);
        
        unsigned recursiveCount() const { return _recursive; }

        /**
         * Indicates the mode of acquisition of the GlobalLock by this particular thread. The
         * return values are '0' (no global lock is held), 'r', 'w', 'R', 'W'. See the commends of
         * QLock for more information on what these modes mean.
         */
        char threadState() const { return _threadState; }
        
        bool isRW() const; // RW
        bool isW() const; // W
        bool hasAnyReadLock() const; // explicitly rR
        
        bool isLocked() const;
        bool isWriteLocked() const;
        bool isWriteLocked(const StringData& ns) const;
        bool isAtLeastReadLocked(const StringData& ns) const;
        bool isLockedForCommitting() const;
        bool isRecursive() const;

        void assertWriteLocked(const StringData& ns) const;
        void assertAtLeastReadLocked(const StringData& ns) const;

        /** pending means we are currently trying to get a lock */
        bool hasLockPending() const { return _lockPending || _lockPendingParallelWriter; }

        // ----


        void lockedStart( char newState ); // RWrw
        void unlocked(); // _threadState = 0
        
        /**
         * you have to be locked already to call this
         * this is mostly for W_to_R or R_to_W
         */
        void changeLockState( char newstate );
        
        // Those are only used for TempRelease. Eventually they should be removed.
        void enterScopedLock(Lock::ScopedLock* lock);
        Lock::ScopedLock* getCurrentScopedLock() const;
        void leaveScopedLock(Lock::ScopedLock* lock);

        bool _batchWriter;

        void recordLockTime() { _scopedLk->recordTime(); }
        void resetLockTime() { _scopedLk->resetTime(); }
        
    private:
        unsigned _recursive;           // we allow recursively asking for a lock; we track that here

        // global lock related
        char _threadState;             // 0, 'r', 'w', 'R', 'W'

        // for temprelease
        // for the nonrecursive case. otherwise there would be many
        // the first lock goes here, which is ok since we can't yield recursive locks
        Lock::ScopedLock* _scopedLk;   
        
        bool _lockPending;
        bool _lockPendingParallelWriter;

        friend class AcquiringParallelWriter;
    };

        
    class AcquiringParallelWriter {
    public:
        AcquiringParallelWriter( LockState& ls );
        ~AcquiringParallelWriter();

    private:
        LockState& _ls;
    };

} // namespace mongo
