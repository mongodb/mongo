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

#include "mongo/db/concurrency/locker.h"


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

        LockResult _lockImpl(const ResourceId& resId, LockMode mode, unsigned timeoutMs);

        bool _unlockAndUpdateRequestsList(const ResourceId& resId, LockRequest* request);


        typedef std::map<const ResourceId, LockRequest*> LockRequestsMap;
        typedef std::pair<const ResourceId, LockRequest*> LockRequestsPair;

        const uint64_t _id;

        // The only reason we have this spin lock here is for the diagnostic tools, which could
        // iterate through the LockRequestsMap on a separate thread and need it to be stable.
        // Apart from that, all accesses to the LockerImpl are always from a single thread.
        //
        // This has to be locked inside const methods, hence the mutable.
        mutable SpinLock _lock;
        LockRequestsMap _requests;

        CondVarLockGrantNotification _notify;

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
        virtual char threadState() const { return _threadState; }

        virtual bool isRW() const; // RW
        virtual bool isW() const; // W
        virtual bool hasAnyReadLock() const; // explicitly rR

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

        virtual void lockedStart(char newState); // RWrw
        virtual void unlocked(); // _threadState = 0

        /**
         * you have to be locked already to call this
         * this is mostly for W_to_R or R_to_W
         */
        virtual void changeLockState(char newstate);

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

        // global lock related
        char _threadState;             // 0, 'r', 'w', 'R', 'W'

        // for temprelease
        // for the nonrecursive case. otherwise there would be many
        // the first lock goes here, which is ok since we can't yield recursive locks
        Lock::ScopedLock* _scopedLk;

        bool _lockPending;
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
