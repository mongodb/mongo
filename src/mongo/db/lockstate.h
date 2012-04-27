// lockstate.h

#pragma once

#include "mongo/db/d_concurrency.h"

namespace mongo {

    // per thread
    class LockState {
    public:
        LockState();
        void dump();
        static void Dump();
        void reportState(BSONObjBuilder& b);
        
        unsigned recursiveCount() const { return _recursive; }

        /**
         * @return 0 rwRW
         */
        char threadState() const { return _threadState; }
        
        bool isRW() const; // RW
        bool isW() const; // W
        bool hasAnyReadLock() const; // explicitly rR
        
        bool isLocked( const StringData& ns ); // rwRW

        // ----


        void locked( char newState ); // RWrw
        void unlocked(); // _threadState = 0
        
        /**
         * you have to be locked already to call this
         * this is mostly for W_to_R or R_to_W
         */
        void changeLockState( char newstate );

        Lock::Nestable whichNestable() const { return _whichNestable; }
        int nestableCount() const { return _nestableCount; }
        
        int otherCount() const { return _otherCount; }
        string otherName() const { return _otherName; }
        WrapperForRWLock* otherLock() const { return _otherLock; }
        
        void enterScopedLock( Lock::ScopedLock* lock );
        Lock::ScopedLock* leaveScopedLock();

        void lockedNestable( Lock::Nestable what , int type );
        void unlockedNestable();
        void lockedOther( const string& db , int type , WrapperForRWLock* lock );
        void unlockedOther();
    private:
        unsigned _recursive;           // we allow recursively asking for a lock; we track that here

        // global lock related
        char _threadState;             // 0, 'r', 'w', 'R', 'W'

        // db level locking related
        Lock::Nestable _whichNestable;
        int _nestableCount;            // recursive lock count on local or admin db XXX - change name
        
        int _otherCount;               //   >0 means write lock, <0 read lock - XXX change name
        string _otherName;             // which database are we locking and working with (besides local/admin) 
        WrapperForRWLock* _otherLock;  // so we don't have to check the map too often (the map has a mutex)

        // for temprelease
        // for the nonrecursive case. otherwise there would be many
        // the first lock goes here, which is ok since we can't yield recursive locks
        Lock::ScopedLock* _scopedLk;   
        
    };

    class WrapperForRWLock : boost::noncopyable { 
        SimpleRWLock r;
    public:
        string name() const { return r.name; }
        LockStat stats;
        WrapperForRWLock(const char *name) : r(name) { }
        void lock()          { LockStat::Acquiring a(stats,'W'); r.lock();          }
        void lock_shared()   { LockStat::Acquiring a(stats,'R'); r.lock_shared();   }
        void unlock()        { stats.unlocking('W');             r.unlock();        }
        void unlock_shared() { stats.unlocking('R');             r.unlock_shared(); }
    };



}
