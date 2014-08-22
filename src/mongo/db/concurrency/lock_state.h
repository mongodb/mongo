// lock_state.h

/**
*    Copyright (C) 2008 10gen Inc.
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

    class Acquiring;


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
         */
        LockResult wait();

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
         * Shortcut for lockExtended, which blocks until a request is granted or deadlocked. Refer
         * to the comments for lockExtended.
         *
         * @return All LockResults except for LOCK_WAITING, because it blocks.
         */
        LockResult lock(const ResourceId& resId, LockMode mode);

        /**
         * Acquires lock on the specified resource in the specified mode and returns the outcome
         * of the operation. See the details for LockResult for more information on what the
         * different results mean.
         *
         * Acquiring the same resource twice increments the reference count of the lock so each
         * call to lock must be matched with a call to unlock.
         *
         * @param resId Id of the resource to be locked.
         * @param mode Mode in which the resource should be locked. Lock upgrades are allowed.
         * @param notify This value cannot be NULL. If the return value is not LOCK_WAITING, this
         *               pointer can be freed and will not be used any more.
         *
         *               If the return value is LOCK_WAITING, the notification method will be
         *               called at some point into the future, when the lock either becomes granted
         *               or a deadlock is discovered. If unlock is called before the lock becomes
         *               granted, the notification will not be invoked.
         *
         *               If the return value is LOCK_WAITING, the notification object *must* live
         *               at least until the notfy method has been invoked or unlock has been called
         *               for the resource it was assigned to. Failure to do so will cause the lock
         *               manager to call into an invalid memory location.
         */
        LockResult lockExtended(const ResourceId& resId,
                                LockMode mode,
                                LockGrantNotification* notify);

        /**
         * Releases a lock previously acquired through a lock call. It is an error to try to
         * release lock which has not been previously acquired (invariant violation).
         */
        void unlock(const ResourceId& resId);

        /**
         * Checks whether the lock held for a particular resource covers the specified mode.
         *
         * For example MODE_X covers MODE_S.
         */
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
         */
        static void changeGlobalLockManagerForTestingOnly(LockManager* newLockMgr);

    private:

        // Shortcut to do the lookup in _requests. Must be called with the spinlock acquired.
        LockRequest* _find(const ResourceId& resId) const;


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


    // per thread
    class LockState : public newlm::Locker {
    public:
        LockState();

        void dump() const;

        BSONObj reportState();
        void reportState(BSONObjBuilder* b);
        
        unsigned recursiveCount() const { return _recursive; }

        /**
         * @return 0 rwRW
         */
        char threadState() const { return _threadState; }
        
        bool isRW() const; // RW
        bool isW() const; // W
        bool hasAnyReadLock() const; // explicitly rR
        
        bool isLocked(const StringData& ns) const; // rwRW
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

        Lock::Nestable whichNestable() const { return _whichNestable; }
        int nestableCount() const { return _nestableCount; }
        
        int otherCount() const { return _otherCount; }
        const std::string& otherName() const { return _otherName; }
        WrapperForRWLock* otherLock() const { return _otherLock; }
        
        void enterScopedLock( Lock::ScopedLock* lock );
        Lock::ScopedLock* leaveScopedLock();

        void lockedNestable( Lock::Nestable what , int type );
        void unlockedNestable();
        void lockedOther( const StringData& db , int type , WrapperForRWLock* lock );
        void lockedOther( int type );  // "same lock as last time" case 
        void unlockedOther();
        bool _batchWriter;

        LockStat* getRelevantLockStat();
        void recordLockTime() { _scopedLk->recordTime(); }
        void resetLockTime() { _scopedLk->resetTime(); }
        
    private:
        unsigned _recursive;           // we allow recursively asking for a lock; we track that here

        // global lock related
        char _threadState;             // 0, 'r', 'w', 'R', 'W'

        // db level locking related
        Lock::Nestable _whichNestable;
        int _nestableCount;            // recursive lock count on local or admin db XXX - change name
        
        int _otherCount;               //   >0 means write lock, <0 read lock - XXX change name
        std::string _otherName;             // which database are we locking and working with (besides local/admin)
        WrapperForRWLock* _otherLock;  // so we don't have to check the map too often (the map has a mutex)

        // for temprelease
        // for the nonrecursive case. otherwise there would be many
        // the first lock goes here, which is ok since we can't yield recursive locks
        Lock::ScopedLock* _scopedLk;   
        
        bool _lockPending;
        bool _lockPendingParallelWriter;

        friend class Acquiring;
        friend class AcquiringParallelWriter;
    };

    class WrapperForRWLock : boost::noncopyable {
        SimpleRWLock rw;
        SimpleMutex m;
        bool sharedLatching;
        LockStat stats;
    public:
        std::string name() const { return rw.name; }
        LockStat& getStats() { return stats; }

        WrapperForRWLock(const StringData& name)
            : rw(name), m(name) {
            // For the local datbase, all operations are short,
            // either writing one entry, or doing a tail.
            // In tests, use a SimpleMutex is much faster for the local db.
            sharedLatching = name != "local";
        }
        void lock()          { if ( sharedLatching ) { rw.lock(); } else { m.lock(); } }
        void lock_shared()   { if ( sharedLatching ) { rw.lock_shared(); } else { m.lock(); } }
        void unlock()        { if ( sharedLatching ) { rw.unlock(); } else { m.unlock(); } }
        void unlock_shared() { if ( sharedLatching ) { rw.unlock_shared(); } else { m.unlock(); } }
    };

    class ScopedLock;

    class Acquiring {
    public:
        Acquiring( Lock::ScopedLock* lock, LockState& ls );
        ~Acquiring();
    private:
        Lock::ScopedLock* _lock;
        LockState& _ls;
    };
        
    class AcquiringParallelWriter {
    public:
        AcquiringParallelWriter( LockState& ls );
        ~AcquiringParallelWriter();

    private:
        LockState& _ls;
    };

} // namespace mongo
