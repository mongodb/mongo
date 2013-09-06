// lockstate.h

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

#include "mongo/db/d_concurrency.h"

namespace mongo {

    class Acquiring;

    // per thread
    class LockState {
    public:
        LockState();

        void dump();
        static void Dump(); 
        BSONObj reportState();
        void reportState(BSONObjBuilder& b);
        
        unsigned recursiveCount() const { return _recursive; }

        /**
         * @return 0 rwRW
         */
        char threadState() const { return _threadState; }
        
        bool isRW() const; // RW
        bool isW() const; // W
        bool hasAnyReadLock() const; // explicitly rR
        bool hasAnyWriteLock() const; // wWX
        
        bool isLocked( const StringData& ns ); // rwRW

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
        const string& otherName() const { return _otherName; }
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
        string _otherName;             // which database are we locking and working with (besides local/admin) 
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
        SimpleRWLock r;
    public:
        string name() const { return r.name; }
        LockStat stats;
        WrapperForRWLock(const StringData& name) : r(name) { }
        void lock()          { r.lock(); }
        void lock_shared()   { r.lock_shared(); }
        void unlock()        { r.unlock(); }
        void unlock_shared() { r.unlock_shared(); }
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


}
