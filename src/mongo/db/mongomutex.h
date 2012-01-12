// @file mongomutex.h

/*
 *    Copyright (C) 2010 10gen Inc.
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
 */

/* Mutex heirarchy (1 = "leaf")
     name                   level
     Logstream::mutex       1
     ClientCursor::ccmutex  2
     dblock                 3

     End func name with _inlock to indicate "caller must lock before calling".
*/

#pragma once

#include "../util/concurrency/rwlock.h"
#include "../util/mmap.h"
#include "../util/time_support.h"
#include "d_globals.h"

namespace mongo {

    class Client;
    Client* curopWaitingForLock( int type );
    void curopGotLock(Client*);

    /* mongomutex time stats */
    class MutexInfo {
        unsigned long long enter, timeLocked; // microseconds
        int locked;
        unsigned long long start; // last as we touch this least often
    public:
        MutexInfo() : timeLocked(0) , locked(0) {
            start = curTimeMicros64();
        }
        void entered() {
            if ( locked == 0 )
                enter = curTimeMicros64();
            locked++;
            assert( locked >= 1 );
        }
        void leaving() {
            locked--;
            assert( locked >= 0 );
            if ( locked == 0 )
                timeLocked += curTimeMicros64() - enter;
        }
        int isLocked() const { return locked; }
        void getTimingInfo(unsigned long long &s, unsigned long long &tl) const {
            s = start;
            tl = timeLocked;
        }
        unsigned long long getTimeLocked() const { return timeLocked; }
    };

    /** the 'big lock'. a read/write lock.
        there is one of these, d.dbMutex.

        generally if you need to declare a mutex use the right primitive class, not this.

        use readlock and writelock classes for scoped locks on this rather than direct
        manipulation.
       */
    class MongoMutex {
    public:
        MongoMutex(const char * name);

        /** @return
         *    > 0  write lock
         *    = 0  no lock
         *    < 0  read lock
         */
        int getState() const { return _state.get(); }

        bool atLeastReadLocked() const { return _state.get() != 0; }
        void assertAtLeastReadLocked() const { assert(atLeastReadLocked()); }
        bool isWriteLocked/*by our thread*/() const { return getState() > 0; }
        void assertWriteLocked() const {
            assert( getState() > 0 );
            DEV assert( !_releasedEarly.get() );
        }

        // write lock.  use the writelock scoped lock class, not this directly.
        void lock() {
            if ( _writeLockedAlready() )
                return;

            _state.set(1);

            curopWaitingForLock( 1 ); // stats
            _m.lock();
            MongoFile::markAllWritable(); // for _DEBUG validation -- a no op for release build
            _acquiredWriteLock();
        }

        // try write lock
        bool lock_try( int millis ) {
            if ( _writeLockedAlready() ) // adjusts _state
                return true;

            curopWaitingForLock( 1 );
            bool got = _m.lock_try( millis );

            if ( got ) {
                _state.set(1);
                MongoFile::markAllWritable(); // for _DEBUG validation -- a no op for release build
                _acquiredWriteLock();
            }

            return got;
        }

        // un write lock
        void unlock() {
            int s = _state.get();
            if( s > 1 ) {
                _state.set(s-1); // recursive lock case
                return;
            }
            if( s != 1 ) {
                if( _releasedEarly.get() ) {
                    _releasedEarly.set(false);
                    return;
                }
                massert( 12599, "internal error: attempt to unlock when wasn't in a write lock", false);
            }
            _releasingWriteLock();
            MongoFile::unmarkAllWritable(); // _DEBUG validation
            _state.set(0);
            _m.unlock();
        }

        /* unlock (write lock), and when unlock() is called later,
           be smart then and don't unlock it again.
           */
        void releaseEarly() {
            assert( getState() == 1 ); // must not be recursive
            assert( !_releasedEarly.get() );
            _releasedEarly.set(true);
            unlock();
        }

        // read lock. don't call directly, use readlock.
        void lock_shared() {
            int s = _state.get();
            if( s ) {
                if( s > 0 ) {
                    // already in write lock - just be recursive and stay write locked
                    _state.set(s+1);
                }
                else {
                    // already in read lock - recurse
                    _state.set(s-1);
                }
            }
            else {
                _state.set(-1);
                Client *c = curopWaitingForLock( -1 );
                _m.lock_shared();
                curopGotLock(c);
            }
        }

        // try read lock
        bool lock_shared_try( int millis ) {
            int s = _state.get();
            if ( s ) {
                // we already have a lock, so no need to try
                lock_shared();
                return true;
            }

            /* [dm] should there be
                             Client *c = curopWaitingForLock( 1 );
               here?  i think so.  seems to be missing.
               */
            bool got = _m.lock_shared_try( millis );
            if ( got )
                _state.set(-1);
            return got;
        }

        void unlock_shared() {
            int s = _state.get();
            if( s > 0 ) {
                wassert( s > 1 ); /* we must have done a lock write first to have s > 1 */
                _state.set(s-1);
                return;
            }
            if( s < -1 ) {
                _state.set(s+1);
                return;
            }
            wassert( s == -1 );
            _state.set(0);
            _m.unlock_shared();
        }

        MutexInfo& info() { return _minfo; }

    private:
        void lockedExclusively();
        void unlockingExclusively();
        void _acquiredWriteLock();
        void _releasingWriteLock();

        /* @return true if was already write locked.  increments recursive lock count. */
        bool _writeLockedAlready();

        RWLock _m;

        /* > 0 write lock with recurse count
           < 0 read lock
        */
        ThreadLocalValue<int> _state;

        MutexInfo _minfo;

    public:
        // indicates we need to call dur::REMAPPRIVATEVIEW on the next write lock
        bool _remapPrivateViewRequested;

    private:
        /* See the releaseEarly() method.
           we use a separate TLS value for releasedEarly - that is ok as
           our normal/common code path, we never even touch it */
        ThreadLocalValue<bool> _releasedEarly;

        /* this is for fsyncAndLock command.  otherwise write lock's greediness will
           make us block on any attempted write lock the the fsync's lock.
           */
        //volatile bool _blockWrites;
    };

    namespace dur {
        void REMAPPRIVATEVIEW();
        void releasingWriteLock(); // because it's hard to include dur.h here
    }

    inline void MongoMutex::_releasingWriteLock() {
        dur::releasingWriteLock();
        unlockingExclusively();
    }

    inline void MongoMutex::_acquiredWriteLock() {
        lockedExclusively();
        if( _remapPrivateViewRequested ) {
            dur::REMAPPRIVATEVIEW();
            dassert( !_remapPrivateViewRequested );
        }
    }

    string sayClientState();

    /* @return true if was already write locked.  increments recursive lock count. */
    inline bool MongoMutex::_writeLockedAlready() {
        int s = _state.get();
        if( s > 0 ) {
            _state.set(s+1);
            return true;
        }
        massert( 10293 , string("internal error: locks are not upgradeable: ") + sayClientState() , s == 0 );
        return false;
    }

    struct writelock {
        writelock() { d.dbMutex.lock(); }
        writelock(const string& ns) { d.dbMutex.lock(); }
        ~writelock() {
            DESTRUCTOR_GUARD(
                d.dbMutex.unlock();
            );
        }
    };

    struct readlock {
        readlock(const string& ns) {
            d.dbMutex.lock_shared();
        }
        readlock() { d.dbMutex.lock_shared(); }
        ~readlock() {
            DESTRUCTOR_GUARD(
                d.dbMutex.unlock_shared();
            );
        }
    };
    struct readlocktry {
        readlocktry( const string&ns , int tryms ) {
            _got = d.dbMutex.lock_shared_try( tryms );
        }
        ~readlocktry() {
            if ( _got ) {
                d.dbMutex.unlock_shared();
            }
        }
        bool got() const { return _got; }
    private:
        bool _got;
    };

    struct writelocktry {
        writelocktry( const string&ns , int tryms ) {
            _got = d.dbMutex.lock_try( tryms );
        }
        ~writelocktry() {
            if ( _got ) {
                d.dbMutex.unlock();
            }
        }
        bool got() const { return _got; }
    private:
        bool _got;
    };

    struct readlocktryassert : public readlocktry {
        readlocktryassert(const string& ns, int tryms) :
            readlocktry(ns,tryms) {
            uassert(13142, "timeout getting readlock", got());
        }
    };

    /** assure we have at least a read lock - they key with this being
        if you have a write lock, that's ok too.
    */
    struct atleastreadlock {
        atleastreadlock( const string& ns = "" ) {
            _prev = d.dbMutex.getState();
            if ( _prev == 0 )
                d.dbMutex.lock_shared();
        }
        ~atleastreadlock() {
            if ( _prev == 0 )
                d.dbMutex.unlock_shared();
        }
    private:
        int _prev;
    };

    /* parameterized choice of read or write locking
       use readlock and writelock instead of this when statically known which you want
    */
    class mongolock {
        bool _writelock;
    public:
        mongolock(bool write) : _writelock(write) {
            if( _writelock ) {
                d.dbMutex.lock();
            }
            else
                d.dbMutex.lock_shared();
        }
        ~mongolock() {
            DESTRUCTOR_GUARD(
            if( _writelock ) {
                d.dbMutex.unlock();
            }
            else {
                d.dbMutex.unlock_shared();
            }
            );
        }
        /* this unlocks, does NOT upgrade. that works for our current usage */
        //void releaseAndWriteLock();
    };

    /* deprecated - use writelock and readlock instead */
    struct dblock : public writelock {
        dblock() : writelock("") { }
    };

    // eliminate this - we should just type "d.dbMutex.assertWriteLocked();" instead
    inline void assertInWriteLock() { d.dbMutex.assertWriteLocked(); }

}
